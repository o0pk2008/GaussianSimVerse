// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"

class UGaussianScene;
class FGaussianGPUBuffer;

/** Render-thread chunk binding with GPU buffer reference. */
struct GAUSSIANSIMVERSE_API FGaussianChunkRenderData
{
	FGaussianGPUBuffer* GPUBuffer = nullptr;
	FMatrix LocalToWorld = FMatrix::Identity;
	FGaussianBounds Bounds;
	uint32 GaussianCount = 0;
	uint32 ChunkIndex = INDEX_NONE;
};

/** Render-thread mirror of a registered Gaussian scene. */
struct GAUSSIANSIMVERSE_API FGaussianSceneProxy
{
	FGaussianSceneProxy() = default;
	explicit FGaussianSceneProxy(const UGaussianScene* InScene);

	uint32 SceneId = INDEX_NONE;
	FMatrix WorldToLocal = FMatrix::Identity;
	FMatrix LocalToWorld = FMatrix::Identity;
	FGaussianBounds Bounds;
	uint32 TotalGaussianCount = 0;
	bool bEnableRendering = true;
	float SplatScale = 1.5f;
	float AlphaCullThreshold = 0.007843137f;
	float CutoffK = 7.0f;
	float CovarianceDilation = 0.3f;
	EGaussianSHBand ShBand = EGaussianSHBand::SH3;
	TArray<FGaussianChunkRenderData> Chunks;
};

/** Per-view inputs assembled on the render thread for RDG pass submission. */
struct GAUSSIANSIMVERSE_API FGaussianViewData
{
	FMatrix ViewMatrix = FMatrix::Identity;
	FMatrix ProjMatrix = FMatrix::Identity;
	FMatrix ViewProjectionMatrix = FMatrix::Identity;
	FMatrix TranslatedViewMatrix = FMatrix::Identity;
	FMatrix TranslatedWorldToClip = FMatrix::Identity;
	FVector PreViewTranslation = FVector::ZeroVector;
	FVector ViewOrigin = FVector::ZeroVector;
	FVector ViewDirection = FVector::ForwardVector;
	FIntRect ViewRect;
	FIntPoint RenderTargetSize = FIntPoint::ZeroValue;
	float FOV = 90.0f;
	uint32 ViewId = 0;
	bool bIsPerspective = true;
};

/** Aggregated frame state passed into the RDG graph each view. */
struct GAUSSIANSIMVERSE_API FGaussianFrameResources
{
	TArray<FGaussianSceneProxy> SceneProxies;
	uint32 TotalGaussianCount = 0;
	bool bHasActiveScenes = false;
};
