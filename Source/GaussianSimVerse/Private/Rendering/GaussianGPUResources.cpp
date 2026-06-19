// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianGPUResources.h"

namespace GaussianGPU
{
	FGaussianSplatGPU ConvertSplatData(const FGaussianSplatData& Splat)
	{
		FGaussianSplatGPU GPU;
		GPU.Position = Splat.Position;
		GPU.Opacity = Splat.Color.W;
		GPU.Rotation = Splat.Rotation;
		GPU.Scale = Splat.Scale;
		GPU.Color = Splat.Color;
		return GPU;
	}

	void ConvertSplatDataArray(const TArray<FGaussianSplatData>& Source, TArray<FGaussianSplatGPU>& OutGPU)
	{
		OutGPU.Reset();
		OutGPU.Reserve(Source.Num());
		for (const FGaussianSplatData& Splat : Source)
		{
			OutGPU.Add(ConvertSplatData(Splat));
		}
	}

	void BuildPositionBuffer(const TArray<FGaussianSplatGPU>& GPUData, TArray<FVector4f>& OutPositions)
	{
		OutPositions.Reset();
		OutPositions.Reserve(GPUData.Num());
		for (const FGaussianSplatGPU& Splat : GPUData)
		{
			OutPositions.Emplace(Splat.Position, 1.0f);
		}
	}
}
