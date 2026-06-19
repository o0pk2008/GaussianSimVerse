// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class FGaussianRenderer;
struct FPostProcessMaterialInputs;
struct FScreenPassTexture;

/** Injects Gaussian compositing via post-processing and caches views for the resolved-scene-color path. */
class FGaussianViewExtension : public FSceneViewExtensionBase
{
public:
	FGaussianViewExtension(const FAutoRegister& AutoRegister, FGaussianRenderer& InRenderer);

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override;
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
	virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;
	virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override;
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass PassId,
		const FSceneView& InView,
		FAfterPassCallbackDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	FScreenPassTexture InjectGaussiansPostProcess_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	FGaussianRenderer& Renderer;
};