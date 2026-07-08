// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Streaming/GaussianLodTypes.h"
#include "GaussianStreamedSceneAsset.generated.h"

/**
 * Lightweight asset referencing a Streamed SOG dataset (lod-meta.json + chunk folders).
 * Splats are loaded at runtime; nothing is baked into UGaussianAsset at import time.
 */
UCLASS(BlueprintType)
class GAUSSIANSIMVERSE_API UGaussianStreamedSceneAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Absolute path to the folder containing lod-meta.json. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Streaming")
	FString DatasetRoot;

	/** Absolute path to lod-meta.json on disk. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Streaming")
	FString LodMetaPath;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian|Streaming")
	FGaussianLodMetaData LodMeta;

	/** Parsed octree from lod-meta.json (runtime; reloaded from disk after asset load). */
	FGaussianLodTreeNode LodTree;

	UFUNCTION(BlueprintCallable, Category = "Gaussian|Streaming")
	bool EnsureLodTreeLoaded(FString& OutError) const;

	UFUNCTION(BlueprintCallable, Category = "Gaussian|Streaming")
	bool ResolveChunkDirectory(int32 FileIndex, FString& OutDirectory) const;

	UFUNCTION(BlueprintCallable, Category = "Gaussian|Streaming")
	bool ResolveEnvironmentDirectory(FString& OutDirectory) const;

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Category = "Gaussian|Streaming")
	FString ImportSourcePath;
#endif
};
