// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"
#include "RenderResource.h"

/** CPU/GPU layout for a single Gaussian splat. Must match GaussianCommon.ush FGaussianSplatGPU. */
struct GAUSSIANSIMVERSE_API FGaussianSplatGPU
{
	FVector3f Position = FVector3f::ZeroVector;
	float Opacity = 1.0f;
	FVector4f Rotation = FVector4f(0.f, 0.f, 0.f, 1.f);
	FVector3f Scale = FVector3f(0.01f, 0.01f, 0.01f);
	float Padding = 0.0f;
	FVector4f Color = FVector4f(1.f, 1.f, 1.f, 1.f);
};
static_assert(sizeof(FGaussianSplatGPU) == 64, "FGaussianSplatGPU must match HLSL layout");

namespace GaussianGPU
{
	GAUSSIANSIMVERSE_API FGaussianSplatGPU ConvertSplatData(const FGaussianSplatData& Splat);
	GAUSSIANSIMVERSE_API void ConvertSplatDataArray(const TArray<FGaussianSplatData>& Source, TArray<FGaussianSplatGPU>& OutGPU);
	GAUSSIANSIMVERSE_API void BuildPositionBuffer(const TArray<FGaussianSplatGPU>& GPUData, TArray<FVector4f>& OutPositions);
}
