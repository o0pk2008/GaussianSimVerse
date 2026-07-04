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

	RebuildSceneProxies_GameThread();
	FlushRenderingCommands();
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

void FGaussianRenderer::MarkSceneDirty(UGaussianScene* Scene)
{
	if (!Scene)
	{
		return;
	}

	bool bNeedsRebuild = false;
	{
		FScopeLock Lock(&SceneLock);
		for (FRegisteredScene& Entry : RegisteredScenes)
		{
			if (Entry.Scene.Get() == Scene)
			{
				Entry.bDirty = true;
				bProxiesDirty = true;
				bNeedsRebuild = true;
				break;
			}
		}
	}

	if (bNeedsRebuild)
	{
		RebuildSceneProxies_GameThread();
		FlushRenderingCommands();
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

void FGaussianRenderer::RenderGaussiansForView(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	FRDGTextureRef SceneColorTexture,
	const FIntRect& SceneColorViewRect) const
{
	if (!GaussianSimVerse::RenderSettings::IsRenderingEnabled()
		|| !GaussianSimVerse::RenderSettings::IsRasterEnabled()
		|| !SceneColorTexture)
	{
		return;
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
		return;
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

	FGaussianRenderGraph::FPassInputs PassInputs;
	PassInputs.View = &View;
	PassInputs.ViewData = ViewDataForPass;
	PassInputs.FrameResources = FrameResources;
	PassInputs.SceneColorTexture = SceneColorTexture;
	PassInputs.SceneColorViewRect = SceneColorViewRect;

	FGaussianRenderGraph::AddPasses(GraphBuilder, PassInputs, TransientResources);

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
