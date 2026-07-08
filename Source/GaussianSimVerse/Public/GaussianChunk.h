// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"
#include "Streaming/GaussianLodTypes.h"
#include "GaussianChunk.generated.h"

class UGaussianAsset;

/**
 * A spatially-addressable chunk of Gaussians for LOD and streaming.
 * Phase 5 (GaussianStreaming) will drive load/unload of chunks.
 */
UCLASS(BlueprintType)
class GAUSSIANSIMVERSE_API UGaussianChunk : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	TObjectPtr<UGaussianAsset> Asset;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FIntVector ChunkCoord = FIntVector::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	FGaussianBounds LocalBounds;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Gaussian")
	int32 ActiveLOD = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Streaming")
	EGaussianChunkLoadState LoadState = EGaussianChunkLoadState::Unloaded;

	FGaussianStreamChunkKey StreamingKey;

	bool IsLoaded() const;
};
