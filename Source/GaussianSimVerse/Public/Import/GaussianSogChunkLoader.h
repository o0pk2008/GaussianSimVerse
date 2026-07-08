// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"

/** Runtime SOG chunk loader (directory meta.json + WebP). Supports splat subsets for streamed LOD. */
class GAUSSIANSIMVERSE_API FGaussianSogChunkLoader
{
public:
	struct FLoadRange
	{
		int32 Offset = 0;
		int32 Count = INDEX_NONE;
	};

	static bool LoadDirectory(
		const FString& DirectoryPath,
		TArray<FGaussianSplatData>& OutSplats,
		FString& OutError,
		TArray<float>* OutShCoefficients = nullptr,
		int32* OutImportedShDegree = nullptr,
		const FLoadRange* Range = nullptr);
};
