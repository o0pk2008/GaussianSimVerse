// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianViewExtension.h"
#include "Rendering/GaussianRenderer.h"
#include "Rendering/GaussianRenderSettings.h"
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
	// When gaussians are registered, optionally force non-temporal AA for this view only.
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
	/** Mirror engine MotionBlur gate enough to choose AfterDOF fallback when MB will not run. */
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

	// Injection order:
	// 0 BeforeDOF — early; can ghost under TSR/TAA
	// 1 AfterDOF — still goes through Motion Blur (camera smear on splats)
	// 2 AfterMotionBlur preferred — but when MB is OFF (common in editor), that pass never
	//   enables callbacks, so we MUST fall back to AfterDOF or gaussians disappear entirely.
	const int32 DesiredPass = GaussianSimVerse::RenderSettings::GetPostProcessPass();
	bool bSubscribe = false;

	if (DesiredPass == 0)
	{
		bSubscribe = bIsPassEnabled && PassId == EPostProcessingPass::BeforeDOF;
	}
	else if (DesiredPass == 1)
	{
		bSubscribe = bIsPassEnabled && PassId == EPostProcessingPass::AfterDOF;
	}
	else // DesiredPass == 2
	{
		const bool bMotionBlurWillRun = GaussianViewExtensionPrivate::WillMotionBlurPassRun(InView);
		if (bMotionBlurWillRun)
		{
			bSubscribe = bIsPassEnabled && PassId == EPostProcessingPass::MotionBlur;
		}
		else
		{
			// Editor / MB disabled: inject after DOF so splats still appear.
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

	Renderer.RenderGaussiansForView(GraphBuilder, View, SceneColor.Texture, SceneColor.ViewRect);
	return SceneColor;
}

bool FGaussianViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return GaussianSimVerse::RenderSettings::IsRenderingEnabled() && Renderer.HasActiveScenes();
}