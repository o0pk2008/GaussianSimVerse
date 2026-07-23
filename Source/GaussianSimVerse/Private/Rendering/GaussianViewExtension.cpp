// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianViewExtension.h"
#include "Rendering/GaussianRenderer.h"
#include "Rendering/GaussianRenderSettings.h"
#include "GaussianTypes.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "SceneUtils.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "HAL/IConsoleManager.h"

FGaussianViewExtension::FGaussianViewExtension(const FAutoRegister& AutoRegister, FGaussianRenderer& InRenderer)
	: FSceneViewExtensionBase(AutoRegister)
	, Renderer(InRenderer)
{
}

void FGaussianViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
}

void FGaussianViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	// TAA/TSR reprojects scene color without Gaussian velocity → smear while moving.
	const int32 PreferMode = GaussianSimVerse::RenderSettings::GetPreferNonTemporalAAMode();
	if (PreferMode <= 0
		|| !GaussianSimVerse::RenderSettings::IsRenderingEnabled()
		|| !Renderer.HasActiveScenes())
	{
		return;
	}

	if (!IsTemporalAccumulationBasedMethod(InView.AntiAliasingMethod))
	{
		return;
	}

	InView.AntiAliasingMethod = (PreferMode >= 2) ? AAM_None : AAM_FXAA;
}

void FGaussianViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	Renderer.SyncSceneProxies_RenderThread();
	// RDG textures from the previous frame are invalid — drop deferred overlays.
	Renderer.ClearDeferredOverlays_RenderThread();
}

void FGaussianViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
	Renderer.CacheView_RenderThread(InView, FGaussianRenderer::GetEffectiveViewRect(InView));
}

namespace GaussianViewExtensionPrivate
{
	static bool WillMotionBlurPassRun(const FSceneView& View)
	{
		if (!View.Family)
		{
			return false;
		}
		static const auto* CVarMBQ = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MotionBlurQuality"));
		const int32 MotionBlurQuality = CVarMBQ ? CVarMBQ->GetValueOnRenderThread() : 0;
		return View.Family->EngineShowFlags.PostProcessing
			&& View.Family->EngineShowFlags.MotionBlur
			&& View.FinalPostProcessSettings.MotionBlurAmount > 0.001f
			&& View.FinalPostProcessSettings.MotionBlurMax > 0.001f
			&& View.Family->bRealtimeUpdate
			&& MotionBlurQuality > 0;
	}
}

void FGaussianViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass PassId,
	const FSceneView& InView,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	if (!GaussianSimVerse::RenderSettings::IsRenderingEnabled()
		|| !GaussianSimVerse::RenderSettings::IsRasterEnabled())
	{
		return;
	}

	const bool bCineCameraDof = Renderer.WantsCineCameraDepthOfField()
		&& GaussianSimVerse::RenderSettings::IsAutoBeforeDofForProxyDofEnabled();

	// CineCamera DOF:
	//   Mode 0 EngineFull (default): Full inject at BeforeDOF — soft-depth→SceneDepth + color into
	//     SceneColor so engine Diaphragm DOF blurs gaussians (natural previous look).
	//   Mode 1 Dual: soft-depth BeforeDOF, color+relight AfterDOF, then approx CoC on gaussians only
	//     (better lighting isolation; DOF is less natural than engine Diaphragm).
	if (bCineCameraDof)
	{
		const int32 DofMode = GaussianSimVerse::RenderSettings::GetCineCameraDofMode();
		if (DofMode <= 0)
		{
			// EngineFull — BeforeDOF can run even when bIsPassEnabled is false on some builds.
			if (PassId == EPostProcessingPass::BeforeDOF)
			{
				InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateLambda(
					[this](FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
					{
						return InjectGaussiansPostProcess_RenderThread(
							GraphBuilder, View, Inputs,
							FGaussianRenderer::EInjectMode::Full,
							/*bAfterTonemap=*/false);
					}));
			}
			return;
		}

		// Dual (optional)
		if (PassId == EPostProcessingPass::BeforeDOF)
		{
			InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateLambda(
				[this](FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
				{
					return InjectGaussiansPostProcess_RenderThread(
						GraphBuilder, View, Inputs,
						FGaussianRenderer::EInjectMode::SoftDepthAndOverlay,
						/*bAfterTonemap=*/false);
				}));
		}
		if (bIsPassEnabled && PassId == EPostProcessingPass::AfterDOF)
		{
			InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateLambda(
				[this](FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
				{
					return InjectGaussiansPostProcess_RenderThread(
						GraphBuilder, View, Inputs,
						FGaussianRenderer::EInjectMode::CompositePending,
						/*bAfterTonemap=*/false);
				}));
		}
		return;
	}

	int32 DesiredPass = GaussianSimVerse::RenderSettings::GetPostProcessPass();

	bool bSubscribe = false;
	if (DesiredPass == 0)
	{
		// BeforeDOF chain can run even when bIsPassEnabled is false on some builds.
		bSubscribe = (PassId == EPostProcessingPass::BeforeDOF);
	}
	else if (DesiredPass == 1)
	{
		bSubscribe = bIsPassEnabled && PassId == EPostProcessingPass::AfterDOF;
	}
	else if (DesiredPass == 3)
	{
		const bool bMotionBlurWillRun = GaussianViewExtensionPrivate::WillMotionBlurPassRun(InView);
		if (bMotionBlurWillRun)
		{
			bSubscribe = bIsPassEnabled && PassId == EPostProcessingPass::MotionBlur;
		}
		else
		{
			bSubscribe = bIsPassEnabled && PassId == EPostProcessingPass::AfterDOF;
		}
	}
	else // DesiredPass == 2: after tonemap (default)
	{
		// BL_SceneColorAfterTonemapping — scene already exposed correctly for cubes/sky.
		bSubscribe = bIsPassEnabled && PassId == EPostProcessingPass::Tonemap;
	}

	if (!bSubscribe)
	{
		return;
	}

	const bool bAfterTonemap = (DesiredPass == 2);
	InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateLambda(
		[this, bAfterTonemap](FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
		{
			return InjectGaussiansPostProcess_RenderThread(
				GraphBuilder, View, Inputs,
				FGaussianRenderer::EInjectMode::Full,
				bAfterTonemap);
		}));
}

FScreenPassTexture FGaussianViewExtension::InjectGaussiansPostProcess_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs,
	FGaussianRenderer::EInjectMode InjectMode,
	bool bAfterTonemap)
{
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
		GraphBuilder,
		Inputs.GetInput(EPostProcessMaterialInput::SceneColor));

	if (!SceneColor.IsValid())
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	// Soft-depth @ BeforeDOF: raster + merge depth only. Never replace SceneColor (AE must not see gaussians).
	if (InjectMode == FGaussianRenderer::EInjectMode::SoftDepthAndOverlay)
	{
		Renderer.RenderGaussiansForView(
			GraphBuilder, View, SceneColor.Texture, SceneColor.ViewRect,
			/*PreferredOutput=*/nullptr, bAfterTonemap, InjectMode);
		// Correctly honor OverrideOutput (copy-through) so the PP chain never gets an uncleared target.
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	const bool bPlugin = Renderer.WantsPluginDepthOfField();
	const bool bCineDualAfterComposite =
		InjectMode == FGaussianRenderer::EInjectMode::CompositePending
		&& Renderer.WantsCineCameraDepthOfField()
		&& GaussianSimVerse::RenderSettings::IsAutoBeforeDofForProxyDofEnabled()
		&& GaussianSimVerse::RenderSettings::GetCineCameraDofMode() > 0;

	FRDGTextureRef PreferredOutput = Inputs.OverrideOutput.IsValid()
		? Inputs.OverrideOutput.Texture
		: nullptr;

	FRDGTextureRef OutputTexture = Renderer.RenderGaussiansForView(
		GraphBuilder, View, SceneColor.Texture, SceneColor.ViewRect,
		PreferredOutput, bAfterTonemap, InjectMode);

	FScreenPassTexture Result(
		OutputTexture ? OutputTexture : SceneColor.Texture,
		SceneColor.ViewRect);

	if (bPlugin || bCineDualAfterComposite)
	{
		FRDGTextureRef SoftDepthBits = nullptr;
		FRDGTextureRef OverlayMask = nullptr;
		FIntPoint SoftOffset = FIntPoint::ZeroValue;
		if (bCineDualAfterComposite)
		{
			FRDGTextureRef UnusedOverlay = nullptr;
			FIntRect DeferredRect;
			FIntPoint DeferredOffset;
			if (Renderer.FindDeferredOverlay_RenderThread(
				View.GetViewKey(), UnusedOverlay, SoftDepthBits, DeferredRect, DeferredOffset))
			{
				OverlayMask = UnusedOverlay;
				SoftOffset = FIntPoint::ZeroValue; // SoftDepthBits is view-local (0-based)
			}
		}

		Renderer.ApplyProxyDepthOfField_RenderThread(
			GraphBuilder, View, Result.Texture, Result.ViewRect,
			SoftDepthBits, OverlayMask, SoftOffset);
	}

	return Result;
}

bool FGaussianViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return GaussianSimVerse::RenderSettings::IsRenderingEnabled() && Renderer.HasActiveScenes();
}
