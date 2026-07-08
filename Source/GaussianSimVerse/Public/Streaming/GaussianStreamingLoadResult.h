// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Streaming/GaussianLodTypes.h"
#include "GaussianTypes.h"
#include "Rendering/GaussianGPUResources.h"
#include "HAL/ThreadSafeCounter.h"

struct FGaussianStreamingLoadResult
{
	FThreadSafeCounter FinishedFlag;
	bool bSuccess = false;
	FGaussianStreamChunkKey Key;
	TArray<FGaussianSplatData> Splats;
	TArray<float> ShCoefficients;
	int32 ImportedShDegree = 0;
	/** Precomputed on the async worker so the game thread can skip ConvertSplatDataArray. */
	TArray<FGaussianSplatGPU> PreparedGpuSplats;
	TArray<FVector4f> PreparedPositions;
	FGaussianBounds PreparedBounds;
	FString Error;

	bool IsFinished() const { return FinishedFlag.GetValue() != 0; }
	void MarkFinished() { FinishedFlag.Set(1); }
};
