// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianViewExtension.h"
#include "Rendering/GaussianRenderer.h"
#include "Rendering/GaussianRenderSettings.h"
#include "GaussianTypes.h"
#include "PostProcess/PostProcessMaterialInputs.h"
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

	// CineCamera DOF: must inject at BeforeDOF (extension runs immediately before DiaphragmDOF).
	// Plugin DOF: inject AfterDOF/MB then compute blur (default pass is fine).
	int32 DesiredPass = GaussianSimVerse::RenderSettings::GetPostProcessPass();
	if (Renderer.WantsCineCameraDepthOfField()
		&& GaussianSimVerse::RenderSettings::IsAutoBeforeDofForProxyDofEnabled())
	{
		DesiredPass = 0;
	}

	bool bSubscribe = false;
	if (DesiredPass == 0)
	{
		// BeforeDOF extension chain runs even when bIsPassEnabled is false on some builds.
		bSubscribe = (PassId == EPostProcessingPass::BeforeDOF);
	}
	else if (DesiredPass == 1)
	{
		bSubscribe = bIsPassEnabled && PassId == EPostProcessingPass::AfterDOF;
	}
	else
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

	if (!bSubscribe)
	{
		return;
	}

	InOutPassCallbacks.Add(
		FAfterPassCallbackDelegate::CreateRaw(this, &FGaussianViewExtension::InjectGaussiansPostProcess_RenderThread));
}

FScreenPassTexture FGaussianViewExtension::InjectGaussiansPostProcess_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
		GraphBuilder,
		Inputs.GetInput(EPostProcessMaterialInput::SceneColor));

	if (!SceneColor.IsValid())
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	const bool bPlugin = Renderer.WantsPluginDepthOfField();

	// CineCamera: inject before DiaphragmDOF (AutoBeforeDof). Progressive CoC uses soft nearest-
	// splat DeviceZ merged into SceneDepth after raster — not proxy CustomDepth (hull flattens CoC).
	// Focus / aperture: CineCamera actor, not plugin sliders.
	Renderer.RenderGaussiansForView(GraphBuilder, View, SceneColor.Texture, SceneColor.ViewRect);

	// Plugin path: custom CoC blur after inject; not driven by CineCamera focus.
	if (bPlugin)
	{
		Renderer.ApplyProxyDepthOfField_RenderThread(
			GraphBuilder, View, SceneColor.Texture, SceneColor.ViewRect);
	}

	return SceneColor;
}

bool FGaussianViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return GaussianSimVerse::RenderSettings::IsRenderingEnabled() && Renderer.HasActiveScenes();
}
