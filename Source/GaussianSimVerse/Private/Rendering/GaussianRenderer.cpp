// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianRenderer.h"
#include "Rendering/GaussianViewExtension.h"
#include "Rendering/GaussianRenderGraph.h"
#include "Rendering/GaussianRenderSettings.h"
#include "GaussianScene.h"
#include "GaussianSimVerse.h"
#include "Engine/Engine.h"
#include "RendererInterface.h"
#include "RenderingThread.h"
#include "SceneRendering.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "SceneTextures.h"
#include "SystemTextures.h"
#include "Rendering/GaussianShaderTypes.h"
#include "Rendering/GaussianRelighting.h"
#include "PixelShaderUtils.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "RHIStaticStates.h"
#include "CommonRenderResources.h"
#include "RenderGraphUtils.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"

FGaussianRenderer& FGaussianRenderer::Get()
{
	static FGaussianRenderer Instance;
	return Instance;
}

FIntRect FGaussianRenderer::GetEffectiveViewRect(const FSceneView& View)
{
	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	if (ViewInfo.ViewRect.Width() > 0 && ViewInfo.ViewRect.Height() > 0)
	{
		return ViewInfo.ViewRect;
	}
	return View.UnscaledViewRect;
}

void FGaussianRenderer::EnsureViewExtensionRegistered()
{
	if (ViewExtension.IsValid() || !GEngine || !GEngine->ViewExtensions)
	{
		return;
	}

	ViewExtension = FSceneViewExtensions::NewExtension<FGaussianViewExtension>(*this);
	UE_LOG(LogGaussianSimVerse, Log, TEXT("GaussianSimVerse ViewExtension registered with GEngine"));
}

void FGaussianRenderer::Initialize()
{
	if (bInitialized)
	{
		return;
	}

	EnsureViewExtensionRegistered();

	IRendererModule& RendererModule = FModuleManager::GetModuleChecked<IRendererModule>(TEXT("Renderer"));
	PostOpaqueDelegateHandle = RendererModule.RegisterPostOpaqueRenderDelegate(
		FPostOpaqueRenderDelegate::CreateRaw(this, &FGaussianRenderer::OnPostOpaqueRender));
	ResolvedSceneColorDelegateHandle = RendererModule.GetResolvedSceneColorCallbacks().AddLambda(
		[this](FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures)
		{
			OnResolvedSceneColor(GraphBuilder, SceneTextures);
		});

	bInitialized = true;

	UE_LOG(LogGaussianSimVerse, Log, TEXT("GaussianRenderer initialized (ViewExtension post-process + PostOpaque sync)"));
}

void FGaussianRenderer::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}

	ViewExtension.Reset();

	if (PostOpaqueDelegateHandle.IsValid())
	{
		if (IRendererModule* RendererModule = FModuleManager::GetModulePtr<IRendererModule>(TEXT("Renderer")))
		{
			RendererModule->RemovePostOpaqueRenderDelegate(PostOpaqueDelegateHandle);
		}
		PostOpaqueDelegateHandle.Reset();
	}

	if (ResolvedSceneColorDelegateHandle.IsValid())
	{
		if (IRendererModule* RendererModule = FModuleManager::GetModulePtr<IRendererModule>(TEXT("Renderer")))
		{
			RendererModule->GetResolvedSceneColorCallbacks().Remove(ResolvedSceneColorDelegateHandle);
		}
		ResolvedSceneColorDelegateHandle.Reset();
	}

	ENQUEUE_RENDER_COMMAND(GaussianSimVerse_ClearProxies)(
		[this](FRHICommandListImmediate& RHICmdList)
		{
			FScopeLock Lock(&SceneLock);
			RenderThreadSceneProxies.Reset();
		});

	FlushRenderingCommands();

	{
		FScopeLock Lock(&SceneLock);
		RegisteredScenes.Reset();
		GameThreadSceneProxies.Reset();
	}

	bInitialized = false;
	UE_LOG(LogGaussianSimVerse, Log, TEXT("GaussianRenderer shutdown"));
}

void FGaussianRenderer::RegisterScene(UGaussianScene* Scene)
{
	if (!Scene)
	{
		return;
	}

	PruneInvalidRegisteredScenes();

	bool bAlreadyRegistered = false;
	{
		FScopeLock Lock(&SceneLock);
		for (const FRegisteredScene& Entry : RegisteredScenes)
		{
			if (Entry.Scene.Get() == Scene)
			{
				bAlreadyRegistered = true;
				break;
			}
		}

		if (!bAlreadyRegistered)
		{
			FRegisteredScene NewEntry;
			NewEntry.Scene = Scene;
			NewEntry.SceneId = NextSceneId++;
			NewEntry.bDirty = true;
			RegisteredScenes.Add(NewEntry);
			bProxiesDirty = true;
		}
	}

	// Async proxy upload — FlushRenderingCommands here stalls the game thread on every stream in.
	RebuildSceneProxies_GameThread();
}

void FGaussianRenderer::UnregisterScene(UGaussianScene* Scene)
{
	if (!Scene)
	{
		return;
	}

	{
		FScopeLock Lock(&SceneLock);
		RegisteredScenes.RemoveAll([Scene](const FRegisteredScene& Entry)
		{
			return Entry.Scene.Get() == Scene;
		});
		bProxiesDirty = true;
	}

	RebuildSceneProxies_GameThread();
}

void FGaussianRenderer::MarkSceneDirty(UGaussianScene* Scene, bool bFlushImmediately)
{
	if (!Scene)
	{
		return;
	}

	{
		FScopeLock Lock(&SceneLock);
		for (FRegisteredScene& Entry : RegisteredScenes)
		{
			if (Entry.Scene.Get() == Scene)
			{
				Entry.bDirty = true;
				bProxiesDirty = true;
				break;
			}
		}
	}

	if (bFlushImmediately)
	{
		FlushDirtySceneProxies();
	}
}

void FGaussianRenderer::FlushDirtySceneProxies()
{
	bool bNeedsRebuild = false;
	{
		FScopeLock Lock(&SceneLock);
		bNeedsRebuild = bProxiesDirty;
	}

	if (bNeedsRebuild)
	{
		RebuildSceneProxies_GameThread();
	}
}

void FGaussianRenderer::PruneInvalidRegisteredScenes()
{
	FScopeLock Lock(&SceneLock);
	const int32 RemovedCount = RegisteredScenes.RemoveAll([](const FRegisteredScene& Entry)
	{
		UGaussianScene* Scene = Entry.Scene.Get();
		if (!Scene || !IsValid(Scene))
		{
			return true;
		}

		if (const UObject* Outer = Scene->GetOuter())
		{
			if (!IsValid(Outer))
			{
				return true;
			}
		}

		return false;
	});

	if (RemovedCount > 0)
	{
		bProxiesDirty = true;
		UE_LOG(LogGaussianSimVerse, Log, TEXT("GaussianSimVerse pruned %d stale scene registration(s)"), RemovedCount);
	}
}

void FGaussianRenderer::RebuildSceneProxies_GameThread()
{
	PruneInvalidRegisteredScenes();
	TArray<FGaussianSceneProxy> NewProxies;
	{
		FScopeLock Lock(&SceneLock);
		NewProxies.Reserve(RegisteredScenes.Num());

		for (FRegisteredScene& Entry : RegisteredScenes)
		{
			UGaussianScene* Scene = Entry.Scene.Get();
			if (!Scene || !Scene->bEnableRendering)
			{
				continue;
			}

			NewProxies.Add(BuildSceneProxy(Scene, Entry.SceneId));
			Entry.bDirty = false;
		}

		GameThreadSceneProxies = MoveTemp(NewProxies);
		bProxiesDirty = false;
	}

	TArray<FGaussianSceneProxy> ProxiesToUpload = GameThreadSceneProxies;
	{
		FScopeLock Lock(&SceneLock);
		PendingRenderThreadProxies = ProxiesToUpload;
		bHasPendingRenderThreadProxies = true;
	}

	ENQUEUE_RENDER_COMMAND(GaussianSimVerse_UpdateSceneProxies)(
		[this, ProxiesToUpload = MoveTemp(ProxiesToUpload)](FRHICommandListImmediate& RHICmdList) mutable
		{
			FScopeLock Lock(&SceneLock);
			RenderThreadSceneProxies = MoveTemp(ProxiesToUpload);
			bHasPendingRenderThreadProxies = false;
		});
}

void FGaussianRenderer::SyncSceneProxies_RenderThread()
{
	FScopeLock Lock(&SceneLock);
	if (bHasPendingRenderThreadProxies)
	{
		RenderThreadSceneProxies = PendingRenderThreadProxies;
		bHasPendingRenderThreadProxies = false;
	}
}

void FGaussianRenderer::CacheView_RenderThread(const FSceneView& View, const FIntRect& ViewportRect)
{
	const uint32 FrameNumber = GFrameNumberRenderThread;
	if (FrameNumber != CachedViewsFrameNumber)
	{
		CachedViewsThisFrame.Reset();
		CachedViewsFrameNumber = FrameNumber;
	}

	const uint32 ViewKey = View.GetViewKey();
	FGaussianCachedViewState* CachedView = CachedViewsThisFrame.FindByPredicate(
		[ViewKey](const FGaussianCachedViewState& Entry)
		{
			return Entry.ViewKey == ViewKey;
		});

	if (!CachedView)
	{
		CachedView = &CachedViewsThisFrame.AddDefaulted_GetRef();
		CachedView->ViewKey = ViewKey;
	}

	CachedView->View = &View;
	CachedView->ViewportRect = ViewportRect;
}

bool FGaussianRenderer::WantsEngineDepthOfField() const
{
	// True if any DOF mode is active (CineCamera or Plugin).
	FScopeLock Lock(&SceneLock);
	const TArray<FGaussianSceneProxy>& Proxies =
		RenderThreadSceneProxies.Num() > 0 ? RenderThreadSceneProxies : GameThreadSceneProxies;
	for (const FGaussianSceneProxy& Proxy : Proxies)
	{
		if (Proxy.bEnableRendering
			&& (Proxy.bUseProxyDepthOfField || Proxy.DofMode != EGaussianProxyDofMode::Off))
		{
			return true;
		}
	}
	return false;
}

bool FGaussianRenderer::WantsCineCameraDepthOfField() const
{
	FScopeLock Lock(&SceneLock);
	const TArray<FGaussianSceneProxy>& Proxies =
		RenderThreadSceneProxies.Num() > 0 ? RenderThreadSceneProxies : GameThreadSceneProxies;
	for (const FGaussianSceneProxy& Proxy : Proxies)
	{
		if (Proxy.bEnableRendering && Proxy.DofMode == EGaussianProxyDofMode::CineCamera)
		{
			return true;
		}
	}
	return false;
}

bool FGaussianRenderer::WantsPluginDepthOfField() const
{
	FScopeLock Lock(&SceneLock);
	const TArray<FGaussianSceneProxy>& Proxies =
		RenderThreadSceneProxies.Num() > 0 ? RenderThreadSceneProxies : GameThreadSceneProxies;
	for (const FGaussianSceneProxy& Proxy : Proxies)
	{
		if (Proxy.bEnableRendering && Proxy.DofMode == EGaussianProxyDofMode::Plugin)
		{
			return true;
		}
	}
	return false;
}

void FGaussianRenderer::SetRelightFrameState_GameThread(const FGaussianRelightFrameState& State)
{
	check(IsInGameThread());
	FScopeLock Lock(&SceneLock);
	RelightFrameState = State;
}

FGaussianRelightFrameState FGaussianRenderer::GetRelightFrameState() const
{
	FScopeLock Lock(&SceneLock);
	return RelightFrameState;
}

bool FGaussianRenderer::HasActiveScenes() const
{
	FScopeLock Lock(&SceneLock);
	const TArray<FGaussianSceneProxy>& Proxies = IsInRenderingThread()
		? RenderThreadSceneProxies
		: GameThreadSceneProxies;

	for (const FGaussianSceneProxy& Proxy : Proxies)
	{
		if (Proxy.bEnableRendering && Proxy.TotalGaussianCount > 0 && Proxy.Chunks.Num() > 0)
		{
			return true;
		}
	}
	return false;
}

FGaussianSceneProxy FGaussianRenderer::BuildSceneProxy(const UGaussianScene* Scene, uint32 SceneId) const
{
	FGaussianSceneProxy Proxy(Scene);
	Proxy.SceneId = SceneId;
	return Proxy;
}

FGaussianFrameResources FGaussianRenderer::BuildFrameResources_RenderThread(const FSceneView& View) const
{
	FGaussianFrameResources FrameResources;

	const int32 MaxScenes = FMath::Max(1, GaussianSimVerse::RenderSettings::CVarMaxScenesPerView.GetValueOnRenderThread());

	FScopeLock Lock(&SceneLock);
	FrameResources.SceneProxies.Reserve(FMath::Min(RenderThreadSceneProxies.Num(), MaxScenes));

	for (const FGaussianSceneProxy& Proxy : RenderThreadSceneProxies)
	{
		if (!Proxy.bEnableRendering)
		{
			continue;
		}

		FrameResources.SceneProxies.Add(Proxy);
		FrameResources.TotalGaussianCount += Proxy.TotalGaussianCount;

		if (FrameResources.SceneProxies.Num() >= MaxScenes)
		{
			break;
		}
	}

	FrameResources.bHasActiveScenes = FrameResources.SceneProxies.Num() > 0;
	return FrameResources;
}

FGaussianViewData FGaussianRenderer::BuildViewData(const FSceneView& InView)
{
	FGaussianViewData ViewData;
	ViewData.ViewMatrix = InView.ViewMatrices.GetViewMatrix();
	ViewData.ProjMatrix = InView.ViewMatrices.GetProjectionMatrix();
	ViewData.ViewProjectionMatrix = InView.ViewMatrices.GetViewProjectionMatrix();
	ViewData.TranslatedViewMatrix = InView.ViewMatrices.GetTranslatedViewMatrix();
	ViewData.TranslatedWorldToClip = InView.ViewMatrices.GetTranslatedViewProjectionMatrix();
	ViewData.PreViewTranslation = InView.ViewMatrices.GetPreViewTranslation();
	ViewData.ViewOrigin = InView.ViewMatrices.GetViewOrigin();
	ViewData.ViewDirection = InView.GetViewDirection();
	ViewData.ViewRect = GetEffectiveViewRect(InView);
	ViewData.RenderTargetSize = FIntPoint(ViewData.ViewRect.Width(), ViewData.ViewRect.Height());
	ViewData.FOV = InView.FOV;
	ViewData.ViewId = static_cast<uint32>(FMath::Max(0, InView.SceneViewInitOptions.StereoViewIndex));
	ViewData.bIsPerspective = true;
	return ViewData;
}

void FGaussianRenderer::ClearDeferredOverlays_RenderThread() const
{
	DeferredOverlays.Reset();
}

FRDGTextureRef FGaussianRenderer::RenderGaussiansForView(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef SceneColorTexture,
	const FIntRect& SceneColorViewRect,
	FRDGTextureRef PreferredOutput,
	bool bAfterTonemap,
	EInjectMode InjectMode) const
{
	if (!GaussianSimVerse::RenderSettings::IsRenderingEnabled()
		|| !GaussianSimVerse::RenderSettings::IsRasterEnabled()
		|| !SceneColorTexture)
	{
		return PreferredOutput ? PreferredOutput : SceneColorTexture;
	}

	// AfterTonemap: composite overlay produced earlier at BeforeDOF (same GraphBuilder frame).
	if (InjectMode == EInjectMode::CompositePending)
	{
		const uint32 ViewKey = View.GetViewKey();
		const FDeferredOverlay* Found = nullptr;
		for (const FDeferredOverlay& Entry : DeferredOverlays)
		{
			if (Entry.ViewKey == ViewKey && Entry.Overlay)
			{
				Found = &Entry;
				break;
			}
		}
		if (!Found)
		{
			return PreferredOutput ? PreferredOutput : SceneColorTexture;
		}

		const FGaussianRelightFrameState Relight = GetRelightFrameState();
		FRDGTextureRef RelightTex = GSystemTextures.GetBlackDummy(GraphBuilder);
		bool bRelightEnabled = false;
		if (Relight.bEnabled)
		{
			if (UTextureRenderTarget2D* RT = Relight.RenderTarget.Get())
			{
				if (FTextureRenderTargetResource* RTRes = RT->GetRenderTargetResource())
				{
					if (FRHITexture* RHI = RTRes->GetRenderTargetTexture())
					{
						RelightTex = GraphBuilder.RegisterExternalTexture(
							CreateRenderTarget(RHI, TEXT("Gaussian.RelightMap")));
						bRelightEnabled = true;
					}
				}
			}
		}

		// Overlay is view-local and sized to the BeforeDOF inject rect. Color composite must use
		// the *current* (AfterTonemap) SceneColor layout for SceneColorOffset — BeforeDOF and
		// AfterTonemap buffers often differ (full buffer vs view slice).
		const FIntPoint OverlayExtent = Found->Overlay->Desc.Extent;
		const FIntRect CompositeRect(
			SceneColorViewRect.Min.X,
			SceneColorViewRect.Min.Y,
			SceneColorViewRect.Min.X + OverlayExtent.X,
			SceneColorViewRect.Min.Y + OverlayExtent.Y);
		const FIntPoint SceneExtent = SceneColorTexture->Desc.Extent;
		const FIntPoint ViewMin = SceneColorViewRect.Min;
		const bool bOffsetFits =
			ViewMin.X >= 0 && ViewMin.Y >= 0
			&& (ViewMin.X + OverlayExtent.X) <= SceneExtent.X
			&& (ViewMin.Y + OverlayExtent.Y) <= SceneExtent.Y;
		const bool bSceneColorIsViewLocal =
			!bOffsetFits
			|| (SceneExtent.X == OverlayExtent.X && SceneExtent.Y == OverlayExtent.Y)
			|| ViewMin == FIntPoint::ZeroValue;
		const FIntPoint CompositeOffset = bSceneColorIsViewLocal ? FIntPoint::ZeroValue : ViewMin;

		const float PreExposure = View.bIsViewInfo
			? static_cast<const FViewInfo&>(View).PreExposure
			: 1.0f;

		// Overlay is linear soft multi-splat. bAfterTonemap selects composite encoding.
		return FGaussianRenderGraph::AddCompositePass(
			GraphBuilder,
			SceneColorTexture,
			Found->Overlay,
			CompositeRect,
			CompositeOffset,
			PreferredOutput,
			bRelightEnabled,
			Relight.bDebug,
			Relight.Blend,
			Relight.Exposure,
			Relight.Brightness,
			Relight.Background,
			RelightTex,
			bAfterTonemap,
			PreExposure);
	}

	const FGaussianFrameResources FrameResources = BuildFrameResources_RenderThread(View);
	if (!FrameResources.bHasActiveScenes)
	{
		static bool bLoggedNoProxiesOnce = false;
		if (!bLoggedNoProxiesOnce)
		{
			bLoggedNoProxiesOnce = true;
			UE_LOG(LogGaussianSimVerse, Warning, TEXT("GaussianSimVerse view %u: no active scene proxies on render thread"), View.GetViewKey());
		}
		return PreferredOutput ? PreferredOutput : SceneColorTexture;
	}

	const FGaussianViewData ViewData = BuildViewData(View);
	FGaussianViewData ViewDataForPass = ViewData;
	// Post-process SceneColor is at render resolution; View.ViewRect may already be UnscaledViewRect.
	if (SceneColorViewRect.Width() > 0 && SceneColorViewRect.Height() > 0)
	{
		ViewDataForPass.ViewRect = SceneColorViewRect;
		ViewDataForPass.RenderTargetSize = SceneColorViewRect.Size();
	}

	FGaussianRDGTransientResources TransientResources = FGaussianRenderGraph::AllocateTransientResources(GraphBuilder);

	FRDGTextureRef SoftDepthBits = nullptr;
	FRDGTextureRef DeferredOverlay = nullptr;
	FIntPoint DeferredSceneColorOffset = FIntPoint::ZeroValue;

	// SceneDepth/CustomStencil use SceneTextures absolute pixels (ViewRect), not always PP SceneColor origin.
	const FIntRect EngineViewRect = GetEffectiveViewRect(View);
	const FIntPoint SceneDepthPixelOffset =
		(EngineViewRect.Width() > 0 && EngineViewRect.Height() > 0)
			? EngineViewRect.Min
			: SceneColorViewRect.Min;

	FGaussianRenderGraph::FPassInputs PassInputs;
	PassInputs.View = &View;
	PassInputs.ViewData = ViewDataForPass;
	PassInputs.FrameResources = FrameResources;
	PassInputs.SceneColorTexture = SceneColorTexture;
	PassInputs.SceneColorViewRect = SceneColorViewRect;
	PassInputs.SceneDepthPixelOffset = SceneDepthPixelOffset;
	// Soft depth only useful before Diaphragm DOF.
	PassInputs.bExportSoftDepthForDof =
		WantsCineCameraDepthOfField()
		&& (InjectMode == EInjectMode::Full || InjectMode == EInjectMode::SoftDepthAndOverlay);
	PassInputs.OutSoftDepthDeviceZ = PassInputs.bExportSoftDepthForDof ? &SoftDepthBits : nullptr;
	PassInputs.InvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;

	FRDGTextureRef CompositedSceneColor = PreferredOutput ? PreferredOutput : SceneColorTexture;
	PassInputs.OutCompositedSceneColor =
		(InjectMode == EInjectMode::SoftDepthAndOverlay) ? nullptr : &CompositedSceneColor;
	PassInputs.PreferredOutputTexture =
		(InjectMode == EInjectMode::SoftDepthAndOverlay) ? nullptr : PreferredOutput;
	PassInputs.bAfterTonemap = bAfterTonemap;
	PassInputs.InjectMode = (InjectMode == EInjectMode::SoftDepthAndOverlay)
		? FGaussianRenderGraph::FPassInputs::EInjectMode::SoftDepthAndOverlay
		: FGaussianRenderGraph::FPassInputs::EInjectMode::Full;
	if (InjectMode == EInjectMode::SoftDepthAndOverlay)
	{
		PassInputs.OutOverlayTexture = &DeferredOverlay;
		PassInputs.OutSceneColorOffset = &DeferredSceneColorOffset;
	}

	// PlayCanvas-style relight: lit proxy RT from SceneCapture, sampled in CompositeCS.
	{
		const FGaussianRelightFrameState Relight = GetRelightFrameState();
		PassInputs.bRelightEnabled = false;
		PassInputs.bRelightDebug = Relight.bDebug;
		PassInputs.RelightBlend = Relight.Blend;
		PassInputs.RelightExposure = Relight.Exposure;
		PassInputs.RelightBrightness = Relight.Brightness;
		PassInputs.RelightBackground = Relight.Background;
		PassInputs.RelightTexture = GSystemTextures.GetBlackDummy(GraphBuilder);

		if (Relight.bEnabled)
		{
			if (UTextureRenderTarget2D* RT = Relight.RenderTarget.Get())
			{
				if (FTextureRenderTargetResource* RTRes = RT->GetRenderTargetResource())
				{
					if (FRHITexture* RHI = RTRes->GetRenderTargetTexture())
					{
						PassInputs.RelightTexture = GraphBuilder.RegisterExternalTexture(
							CreateRenderTarget(RHI, TEXT("Gaussian.RelightMap")));
						PassInputs.bRelightEnabled = true;
					}
				}
			}
		}
	}
	// Always try depth occlusion so regular actors (cubes, etc.) can hide gaussians behind them.
	// Proxy DOF merges CustomDepth into SceneDepth for CoC — exclude those pixels via Custom Stencil
	// so the proxy shell does not self-occlude / black-hole the gaussians.
	PassInputs.bDepthOcclusion = GaussianSimVerse::RenderSettings::IsDepthOcclusionEnabled();
	PassInputs.DepthOcclusionBiasCm = GaussianSimVerse::RenderSettings::GetDepthOcclusionBiasCm();
	PassInputs.bExcludeProxyStencilFromOcclusion = true;
	{
		float F = 0.f, C = 0.f, R = 0.f;
		uint32 Stencil = 1;
		if (GetActiveDofSettings(F, C, R, Stencil))
		{
			PassInputs.ProxyStencilExclude = Stencil;
		}
		else
		{
			PassInputs.ProxyStencilExclude = 1;
		}
	}

	// Opaque actor occlusion: sample engine SceneDepth while rasterizing splats.
	// (After CineCamera late-merge, this includes proxy CoC depth + real mesh depth.)
	if (View.bIsViewInfo)
	{
		const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
		if (const FSceneTextures* SceneTextures = ViewInfo.GetSceneTexturesChecked())
		{
			if (PassInputs.bDepthOcclusion)
			{
				PassInputs.SceneDepthTexture = SceneTextures->Depth.Resolve
					? SceneTextures->Depth.Resolve
					: SceneTextures->Depth.Target;
			}
			// Only use engine CustomDepth.Stencil — never invent PF_X24_G8 views from random textures.
			if (SceneTextures->CustomDepth.IsValid() && SceneTextures->CustomDepth.Stencil)
			{
				PassInputs.CustomStencilSRV = SceneTextures->CustomDepth.Stencil;
			}
		}
	}
	if (!PassInputs.SceneDepthTexture)
	{
		PassInputs.bDepthOcclusion = false;
		PassInputs.SceneDepthTexture = GSystemTextures.GetDepthDummy(GraphBuilder);
	}
	// Without a real custom stencil, cannot safely exclude proxy → keep occlusion for actors only.
	if (!PassInputs.CustomStencilSRV)
	{
		PassInputs.bExcludeProxyStencilFromOcclusion = false;
	}

	FGaussianRenderGraph::AddPasses(GraphBuilder, PassInputs, TransientResources);

	// CineCamera: feed SceneDepth before Diaphragm DOF (must run in the BeforeDOF inject).
	// 1) Proxy CustomDepth → SceneDepth (shell CoC / sky-safe late merge)
	// 2) Nearest-splat soft DeviceZ → SceneDepth (near→far falloff on the cloud itself)
	if (PassInputs.bExportSoftDepthForDof)
	{
		float UnusedF = 0.f, UnusedC = 0.f, UnusedR = 0.f;
		uint32 ProxyStencil = 1u;
		GetActiveDofSettings(UnusedF, UnusedC, UnusedR, ProxyStencil);
		MergeProxyCustomDepthIntoSceneDepth_RenderThread(GraphBuilder, View, ProxyStencil);
	}

	if (SoftDepthBits)
	{
		// Soft buffer is overlay-local (0..size); merge pass viewport must match engine SceneDepth pixels.
		const FIntRect SoftMergeRect(
			SceneDepthPixelOffset.X,
			SceneDepthPixelOffset.Y,
			SceneDepthPixelOffset.X + SceneColorViewRect.Width(),
			SceneDepthPixelOffset.Y + SceneColorViewRect.Height());
		MergeGaussianSoftDepthIntoSceneDepth_RenderThread(
			GraphBuilder, View, SoftDepthBits, SoftMergeRect);

		static bool bLoggedSoftDepthOnce = false;
		if (!bLoggedSoftDepthOnce)
		{
			bLoggedSoftDepthOnce = true;
			UE_LOG(LogGaussianSimVerse, Log,
				TEXT("GaussianSimVerse: SoftDepthMerge→SceneDepth enabled for CineCamera DOF (view %u, rect %dx%d, depthOrigin=%d,%d)"),
				View.GetViewKey(),
				SceneColorViewRect.Width(),
				SceneColorViewRect.Height(),
				SceneDepthPixelOffset.X,
				SceneDepthPixelOffset.Y);
		}
	}

	// Stash overlay (+ soft depth) for AfterDOF color composite / Dual CoC (SceneColor untouched here).
	if (InjectMode == EInjectMode::SoftDepthAndOverlay && DeferredOverlay)
	{
		const uint32 ViewKey = View.GetViewKey();
		bool bReplaced = false;
		for (FDeferredOverlay& Entry : DeferredOverlays)
		{
			if (Entry.ViewKey == ViewKey)
			{
				Entry.Overlay = DeferredOverlay;
				Entry.SoftDepthBits = SoftDepthBits;
				Entry.SceneColorViewRect = SceneColorViewRect;
				Entry.SceneColorOffset = DeferredSceneColorOffset;
				bReplaced = true;
				break;
			}
		}
		if (!bReplaced)
		{
			FDeferredOverlay Entry;
			Entry.ViewKey = ViewKey;
			Entry.Overlay = DeferredOverlay;
			Entry.SoftDepthBits = SoftDepthBits;
			Entry.SceneColorViewRect = SceneColorViewRect;
			Entry.SceneColorOffset = DeferredSceneColorOffset;
			DeferredOverlays.Add(Entry);
		}

		static bool bLoggedSplitInjectOnce = false;
		if (!bLoggedSplitInjectOnce)
		{
			bLoggedSplitInjectOnce = true;
			UE_LOG(LogGaussianSimVerse, Log,
				TEXT("GaussianSimVerse: Dual CineCamera — soft depth @ BeforeDOF, color+relight @ AfterDOF, CoC on gaussians after composite"));
		}

		// Return input SceneColor untouched so AE / tonemap only see meshes + sky until AfterDOF.
		return SceneColorTexture;
	}

	static bool bLoggedFirstRenderOnce = false;
	if (!bLoggedFirstRenderOnce)
	{
		bLoggedFirstRenderOnce = true;
		UE_LOG(LogGaussianSimVerse, Log, TEXT("GaussianSimVerse first render submission: view %u, %d proxies, %u splats, rect %dx%d (post-process injection)"),
			View.GetViewKey(),
			FrameResources.SceneProxies.Num(),
			FrameResources.TotalGaussianCount,
			SceneColorViewRect.Width(),
			SceneColorViewRect.Height());
	}

	if (GaussianSimVerse::RenderSettings::IsRenderDebugEnabled())
	{
		UE_LOG(LogGaussianSimVerse, Log, TEXT("GaussianSimVerse view %u: %d scene proxies, %u total splats, rect %dx%d"),
			View.GetViewKey(),
			FrameResources.SceneProxies.Num(),
			FrameResources.TotalGaussianCount,
			SceneColorViewRect.Width(),
			SceneColorViewRect.Height());
	}

	return CompositedSceneColor;
}

void FGaussianRenderer::MergeProxyCustomDepthIntoSceneDepth_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	uint32 ProxyStencilValue) const
{
	if (!WantsCineCameraDepthOfField() || !View.bIsViewInfo)
	{
		return;
	}

	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	const FSceneTextures* SceneTextures = ViewInfo.GetSceneTexturesChecked();
	if (!SceneTextures || !SceneTextures->CustomDepth.IsValid() || !SceneTextures->CustomDepth.Depth)
	{
		static bool bLogged = false;
		if (!bLogged)
		{
			bLogged = true;
			UE_LOG(LogGaussianSimVerse, Warning,
				TEXT("GaussianSimVerse CineCamera DOF: no CustomDepth. Enable Write Custom Depth + r.CustomDepth 3."));
		}
		return;
	}

	// DiaphragmDOF samples Depth.Resolve — write that resource when possible.
	FRDGTextureRef SceneDepthWrite = SceneTextures->Depth.Resolve
		? SceneTextures->Depth.Resolve
		: SceneTextures->Depth.Target;
	if (!SceneDepthWrite)
	{
		return;
	}

	FRDGTextureRef CustomDepth = SceneTextures->CustomDepth.Depth;
	FRDGTextureSRVRef CustomStencil = SceneTextures->CustomDepth.Stencil;
	uint32 bHasStencil = 0u;
	if (CustomStencil)
	{
		bHasStencil = 1u;
	}
	else if (CustomDepth->Desc.Format == PF_DepthStencil)
	{
		CustomStencil = GraphBuilder.CreateSRV(
			FRDGTextureSRVDesc::CreateWithPixelFormat(CustomDepth, PF_X24_G8));
		bHasStencil = 1u;
	}
	else
	{
		// No stencil isolation — still merge all custom depth (only proxy should write it).
		if (SceneDepthWrite->Desc.Format == PF_DepthStencil)
		{
			CustomStencil = GraphBuilder.CreateSRV(
				FRDGTextureSRVDesc::CreateWithPixelFormat(SceneDepthWrite, PF_X24_G8));
		}
		else
		{
			return;
		}
		bHasStencil = 0u;
	}

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	const TShaderMapRef<FGaussianProxyDepthMergePS> PixelShader(ShaderMap);
	if (!PixelShader.IsValid())
	{
		return;
	}

	// D3D12 rejects depth-only graphics PSOs from FPixelShaderUtils without a color RT.
	// Bind a throwaway color target (no color writes) so the pipeline is valid.
	FRDGTextureDesc DummyColorDesc = FRDGTextureDesc::Create2D(
		SceneDepthWrite->Desc.Extent,
		PF_B8G8R8A8,
		FClearValueBinding::Black,
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef DummyColor = GraphBuilder.CreateTexture(DummyColorDesc, TEXT("Gaussian.DepthMergeColor"));

	FGaussianProxyDepthMergePS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FGaussianProxyDepthMergePS::FParameters>();
	PassParameters->CustomDepthTexture = CustomDepth;
	PassParameters->CustomStencilTexture = CustomStencil;
	PassParameters->ProxyStencil = ProxyStencilValue;
	PassParameters->bHasCustomStencil = bHasStencil;
	PassParameters->RenderTargets[0] = FRenderTargetBinding(DummyColor, ERenderTargetLoadAction::ENoAction);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneDepthWrite,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ELoad,
		FExclusiveDepthStencil::DepthWrite_StencilNop);

	// CF_DepthNearOrEqual: only write proxy depth when it is closer than existing scene depth
	// (preserves cubes/meshes in front of the proxy shell). Reverse-Z aware in UE.
	const FIntRect ViewRect = GetEffectiveViewRect(View);
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		ShaderMap,
		RDG_EVENT_NAME("GaussianSimVerse::ProxyDepthMerge→SceneDepth (CineCamera DOF)"),
		PixelShader,
		PassParameters,
		ViewRect,
		TStaticBlendState<CW_NONE>::GetRHI(),
		TStaticRasterizerState<FM_Solid, CM_None>::GetRHI(),
		TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}

void FGaussianRenderer::MergeGaussianSoftDepthIntoSceneDepth_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef SoftDepthBitsTexture,
	const FIntRect& SoftDepthViewRect) const
{
	if (!SoftDepthBitsTexture || !View.bIsViewInfo || !WantsCineCameraDepthOfField())
	{
		return;
	}

	const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
	const FSceneTextures* SceneTextures = ViewInfo.GetSceneTexturesChecked();
	if (!SceneTextures)
	{
		return;
	}

	FRDGTextureRef SceneDepthWrite = SceneTextures->Depth.Resolve
		? SceneTextures->Depth.Resolve
		: SceneTextures->Depth.Target;
	if (!SceneDepthWrite)
	{
		return;
	}

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	const TShaderMapRef<FGaussianSoftDepthMergePS> PixelShader(ShaderMap);
	if (!PixelShader.IsValid())
	{
		return;
	}

	FRDGTextureDesc DummyColorDesc = FRDGTextureDesc::Create2D(
		SceneDepthWrite->Desc.Extent,
		PF_B8G8R8A8,
		FClearValueBinding::Black,
		TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FRDGTextureRef DummyColor = GraphBuilder.CreateTexture(DummyColorDesc, TEXT("Gaussian.SoftDepthMergeColor"));

	FGaussianSoftDepthMergePS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FGaussianSoftDepthMergePS::FParameters>();
	PassParameters->SoftDepthBitsTexture = SoftDepthBitsTexture;
	PassParameters->SoftDepthOffset = SoftDepthViewRect.Min;
	PassParameters->RenderTargets[0] = FRenderTargetBinding(DummyColor, ERenderTargetLoadAction::ENoAction);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneDepthWrite,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ELoad,
		FExclusiveDepthStencil::DepthWrite_StencilNop);

	// Nearer DeviceZ (reverse-Z) wins over farther background; do not punch through closer cubes.
	const FIntRect ViewRect = SoftDepthViewRect.Width() > 0 ? SoftDepthViewRect : GetEffectiveViewRect(View);
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		ShaderMap,
		RDG_EVENT_NAME("GaussianSimVerse::SoftDepthMerge→SceneDepth"),
		PixelShader,
		PassParameters,
		ViewRect,
		TStaticBlendState<CW_NONE>::GetRHI(),
		TStaticRasterizerState<FM_Solid, CM_None>::GetRHI(),
		TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
}

bool FGaussianRenderer::GetActiveDofSettings(
	float& OutFocalDistanceCm,
	float& OutCocScale,
	float& OutMaxBlurRadiusPx,
	uint32& OutProxyStencil) const
{
	FScopeLock Lock(&SceneLock);
	const TArray<FGaussianSceneProxy>& Proxies =
		RenderThreadSceneProxies.Num() > 0 ? RenderThreadSceneProxies : GameThreadSceneProxies;
	bool bFound = false;
	OutFocalDistanceCm = 500.0f;
	OutCocScale = 0.004f;
	OutMaxBlurRadiusPx = 16.0f;
	OutProxyStencil = 1;
	for (const FGaussianSceneProxy& Proxy : Proxies)
	{
		const bool bDofOn = Proxy.bEnableRendering
			&& (Proxy.DofMode != EGaussianProxyDofMode::Off || Proxy.bUseProxyDepthOfField);
		if (!bDofOn)
		{
			continue;
		}
		if (!bFound)
		{
			OutFocalDistanceCm = Proxy.DofFocalDistanceCm;
			OutCocScale = Proxy.DofCocScale;
			OutMaxBlurRadiusPx = Proxy.DofMaxBlurRadiusPx;
			OutProxyStencil = Proxy.DofProxyStencil;
			bFound = true;
		}
		else
		{
			OutFocalDistanceCm = FMath::Min(OutFocalDistanceCm, Proxy.DofFocalDistanceCm);
			OutCocScale = FMath::Max(OutCocScale, Proxy.DofCocScale);
			OutMaxBlurRadiusPx = FMath::Max(OutMaxBlurRadiusPx, Proxy.DofMaxBlurRadiusPx);
		}
	}
	return bFound;
}

bool FGaussianRenderer::FindDeferredOverlay_RenderThread(
	uint32 ViewKey,
	FRDGTextureRef& OutOverlay,
	FRDGTextureRef& OutSoftDepthBits,
	FIntRect& OutViewRect,
	FIntPoint& OutSceneColorOffset) const
{
	for (const FDeferredOverlay& Entry : DeferredOverlays)
	{
		if (Entry.ViewKey == ViewKey && Entry.Overlay)
		{
			OutOverlay = Entry.Overlay;
			OutSoftDepthBits = Entry.SoftDepthBits;
			OutViewRect = Entry.SceneColorViewRect;
			OutSceneColorOffset = Entry.SceneColorOffset;
			return true;
		}
	}
	return false;
}

void FGaussianRenderer::ApplyProxyDepthOfField_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef SceneColorTexture,
	const FIntRect& SceneColorViewRect,
	FRDGTextureRef SoftDepthBitsTexture,
	FRDGTextureRef OverlayMaskTexture,
	FIntPoint SoftDepthOffset) const
{
	float FocalCm = 500.0f;
	float CocScale = 0.004f;
	float MaxRadius = 16.0f;
	uint32 ProxyStencil = 1;
	if (!GetActiveDofSettings(FocalCm, CocScale, MaxRadius, ProxyStencil)
		|| !SceneColorTexture
		|| SceneColorViewRect.Width() <= 0
		|| SceneColorViewRect.Height() <= 0)
	{
		return;
	}

	// Prefer live CineCamera / PP focal distance when available (cm).
	if (View.FinalPostProcessSettings.DepthOfFieldFocalDistance > 1.0f)
	{
		FocalCm = View.FinalPostProcessSettings.DepthOfFieldFocalDistance;
	}

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	const TShaderMapRef<FGaussianDofCS> DofShader(ShaderMap);
	if (!DofShader.IsValid())
	{
		return;
	}

	FRDGTextureRef SceneDepth = nullptr;
	FRDGTextureRef CustomDepth = nullptr;
	FRDGTextureSRVRef CustomStencil = nullptr;
	if (View.bIsViewInfo)
	{
		const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);
		if (const FSceneTextures* ST = ViewInfo.GetSceneTexturesChecked())
		{
			SceneDepth = ST->Depth.Resolve ? ST->Depth.Resolve : ST->Depth.Target;
			if (ST->CustomDepth.IsValid())
			{
				CustomDepth = ST->CustomDepth.Depth;
				CustomStencil = ST->CustomDepth.Stencil;
			}
		}
	}

	const bool bHasSoftDepth = SoftDepthBitsTexture != nullptr;
	if (!CustomDepth && !bHasSoftDepth && !SceneDepth)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			bLoggedOnce = true;
			UE_LOG(LogGaussianSimVerse, Warning,
				TEXT("GaussianSimVerse DOF skipped: no SoftDepth/CustomDepth/SceneDepth. Enable proxy Custom Depth (r.CustomDepth 3) or CineCamera soft-depth."));
		}
		return;
	}

	FRDGTextureRef SafeSceneDepth = SceneDepth ? SceneDepth : GSystemTextures.GetDepthDummy(GraphBuilder);
	// Bindable stubs when optional resources are missing.
	FRDGTextureRef SafeCustomDepth = CustomDepth ? CustomDepth : SafeSceneDepth;
	FRDGTextureSRVRef SafeStencil = CustomStencil;
	if (!SafeStencil)
	{
		if (SafeCustomDepth->Desc.Format == PF_DepthStencil)
		{
			SafeStencil = GraphBuilder.CreateSRV(
				FRDGTextureSRVDesc::CreateWithPixelFormat(SafeCustomDepth, PF_X24_G8));
		}
		else if (SafeSceneDepth->Desc.Format == PF_DepthStencil)
		{
			SafeStencil = GraphBuilder.CreateSRV(
				FRDGTextureSRVDesc::CreateWithPixelFormat(SafeSceneDepth, PF_X24_G8));
		}
		else
		{
			// 1x1 legal stencil view.
			const FRDGTextureDesc DummyDesc = FRDGTextureDesc::Create2D(
				FIntPoint(1, 1),
				PF_DepthStencil,
				FClearValueBinding::DepthFar,
				TexCreate_ShaderResource | TexCreate_DepthStencilTargetable | TexCreate_InputAttachmentRead);
			FRDGTextureRef DummyDS = GraphBuilder.CreateTexture(DummyDesc, TEXT("Gaussian.DofDummyDS"));
			AddClearDepthStencilPass(GraphBuilder, DummyDS, true, 0.0f, true, 0);
			SafeStencil = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(DummyDS, PF_X24_G8));
		}
	}

	FRDGTextureRef SafeSoftDepth = SoftDepthBitsTexture;
	if (!SafeSoftDepth)
	{
		FRDGTextureDesc SoftDummyDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, 1),
			PF_R32_UINT,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV);
		SafeSoftDepth = GraphBuilder.CreateTexture(SoftDummyDesc, TEXT("Gaussian.DofSoftDummy"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SafeSoftDepth), 0u);
	}

	FRDGTextureRef SafeOverlay = OverlayMaskTexture;
	if (!SafeOverlay)
	{
		FRDGTextureDesc OvDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, 1),
			PF_FloatRGBA,
			FClearValueBinding::White,
			TexCreate_ShaderResource | TexCreate_UAV);
		SafeOverlay = GraphBuilder.CreateTexture(OvDesc, TEXT("Gaussian.DofOverlayDummy"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(SafeOverlay), FVector4f(1, 1, 1, 1));
	}

	const FIntPoint TextureSize = SceneColorTexture->Desc.Extent;
	const FVector4f ViewRect(
		static_cast<float>(SceneColorViewRect.Min.X),
		static_cast<float>(SceneColorViewRect.Min.Y),
		static_cast<float>(SceneColorViewRect.Width()),
		static_cast<float>(SceneColorViewRect.Height()));

	const uint32 bMaskOverlay = OverlayMaskTexture ? 1u : 0u;
	const uint32 bSoft = bHasSoftDepth ? 1u : 0u;

	auto DispatchDofPass = [&](FRDGTextureRef SourceColor, FRDGTextureRef DestColor, FVector2f Direction)
	{
		FGaussianDofCS::FParameters* Params = GraphBuilder.AllocParameters<FGaussianDofCS::FParameters>();
		Params->ViewRect = ViewRect;
		Params->TextureSize = TextureSize;
		Params->BlurDirection = Direction;
		Params->FocalDistanceCm = FocalCm;
		Params->CocScale = CocScale;
		Params->MaxBlurRadiusPx = MaxRadius;
		Params->InvDeviceZToWorldZTransform = View.InvDeviceZToWorldZTransform;
		Params->ProxyStencil = ProxyStencil;
		Params->bHasCustomStencil = CustomStencil ? 1u : 0u;
		Params->bHasCustomDepth = CustomDepth ? 1u : 0u;
		Params->bHasSceneDepth = SceneDepth ? 1u : 0u;
		Params->bHasSoftDepth = bSoft;
		Params->bMaskByOverlayAlpha = bMaskOverlay;
		Params->SoftDepthOffset = SoftDepthOffset;
		Params->SceneColorTexture = SourceColor;
		Params->SceneDepthTexture = SafeSceneDepth;
		Params->CustomDepthTexture = SafeCustomDepth;
		Params->CustomStencilTexture = SafeStencil;
		Params->SoftDepthBitsTexture = SafeSoftDepth;
		Params->OverlayTexture = SafeOverlay;
		Params->RWOutput = GraphBuilder.CreateUAV(DestColor);

		const FIntVector Groups(
			FMath::DivideAndRoundUp(SceneColorViewRect.Width(), 8),
			FMath::DivideAndRoundUp(SceneColorViewRect.Height(), 8),
			1);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GaussianSimVerse::DofBlur"),
			ERDGPassFlags::Compute,
			DofShader,
			Params,
			Groups);
	};

	// Always use FloatRGBA UAV scratch — some SceneColor formats are not UAV-safe and crash.
	FRDGTextureDesc ScratchDesc = FRDGTextureDesc::Create2D(
		SceneColorTexture->Desc.Extent,
		PF_FloatRGBA,
		FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable);
	FRDGTextureRef ScratchA = GraphBuilder.CreateTexture(ScratchDesc, TEXT("Gaussian.DofA"));
	FRDGTextureRef ScratchB = GraphBuilder.CreateTexture(ScratchDesc, TEXT("Gaussian.DofB"));

	// H then V. Input may be HDR SceneColor (SRV); output FloatRGBA then copy back.
	DispatchDofPass(SceneColorTexture, ScratchA, FVector2f(1.0f, 0.0f));
	DispatchDofPass(ScratchA, ScratchB, FVector2f(0.0f, 1.0f));

	// Copy FloatRGBA → SceneColor (engine handles format convert when possible).
	AddCopyTexturePass(GraphBuilder, ScratchB, SceneColorTexture);
}

void FGaussianRenderer::OnPostOpaqueRender(FPostOpaqueRenderParameters& Parameters)
{
	if (!GaussianSimVerse::RenderSettings::IsRenderingEnabled()
		|| !Parameters.View)
	{
		return;
	}

	const FSceneView& View = static_cast<const FSceneView&>(*Parameters.View);
	CacheView_RenderThread(View, Parameters.ViewportRect);
	SyncSceneProxies_RenderThread();
}

void FGaussianRenderer::OnResolvedSceneColor(FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures)
{
	if (!GaussianSimVerse::RenderSettings::IsRenderingEnabled()
		|| !GaussianSimVerse::RenderSettings::IsRasterEnabled()
		|| !GaussianSimVerse::RenderSettings::UseResolvedSceneColorPath()
		|| !SceneTextures.Color.Resolve)
	{
		return;
	}

	SyncSceneProxies_RenderThread();

	auto RenderForView = [&](const FSceneView& View, const FIntRect& ViewportRect)
	{
		RenderGaussiansForView(GraphBuilder, View, SceneTextures.Color.Resolve, ViewportRect);
	};

	if (CachedViewsThisFrame.Num() > 0)
	{
		for (const FGaussianCachedViewState& CachedView : CachedViewsThisFrame)
		{
			if (CachedView.View)
			{
				RenderForView(*CachedView.View, CachedView.ViewportRect);
			}
		}
		return;
	}

	if (SceneTextures.Owner)
	{
		for (const FSceneView* View : SceneTextures.Owner->Views)
		{
			if (View)
			{
				RenderForView(*View, GetEffectiveViewRect(*View));
			}
		}
	}
}
