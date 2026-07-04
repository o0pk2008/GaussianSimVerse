// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianViewExtension.h"
#include "Rendering/GaussianRenderer.h"
#include "Rendering/GaussianRenderSettings.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "ScreenPass.h"

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
}

void FGaussianViewExtension::PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	Renderer.SyncSceneProxies_RenderThread();
}

void FGaussianViewExtension::PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView)
{
	Renderer.CacheView_RenderThread(InView, FGaussianRenderer::GetEffectiveViewRect(InView));
}

void FGaussianViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass PassId,
	const FSceneView& InView,
	FAfterPassCallbackDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	if (!bIsPassEnabled
		|| !GaussianSimVerse::RenderSettings::IsRenderingEnabled()
		|| !GaussianSimVerse::RenderSettings::IsRasterEnabled())
	{
		return;
	}

	const int32 DesiredPass = GaussianSimVerse::RenderSettings::GetPostProcessPass();
	const bool bBeforeDOF = (DesiredPass == 0 && PassId == EPostProcessingPass::BeforeDOF);
	const bool bAfterDOF = (DesiredPass == 1 && PassId == EPostProcessingPass::AfterDOF);
	if (!bBeforeDOF && !bAfterDOF)
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