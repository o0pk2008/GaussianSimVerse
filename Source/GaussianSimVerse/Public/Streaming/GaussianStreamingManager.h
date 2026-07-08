// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Streaming/GaussianLodTypes.h"
#include "Import/GaussianSogChunkLoader.h"

class UGaussianScene;
class UGaussianStreamedSceneAsset;
class UGaussianChunk;
class UGaussianAsset;
class AGaussianStreamedSceneActor;

/** Runtime LOD streaming controller for Streamed SOG datasets. */
class GAUSSIANSIMVERSE_API FGaussianStreamingManager
{
public:
	void Initialize(AGaussianStreamedSceneActor* InOwner, UGaussianStreamedSceneAsset* InAsset, UGaussianScene* InScene);
	void Shutdown();
	void UpdateStreaming(const FVector& ViewOrigin);

	int32 GetLoadedChunkCount() const { return ResidentChunks.Num(); }
	int32 GetLoadedSplatCount() const;
	int32 GetPendingLoadCount() const { return PendingLoads.Num(); }
	int32 GetDesiredChunkCount() const { return DesiredKeys.Num(); }

private:
	struct FResidentChunk
	{
		TObjectPtr<UGaussianChunk> Chunk;
		TObjectPtr<UGaussianAsset> Asset;
		FGaussianStreamChunkKey Key;
		int32 LastTouchedFrame = 0;
	};

	struct FPendingLoad
	{
		FGaussianStreamChunkKey Key;
		FString ChunkDirectory;
		FGaussianBounds LocalBounds;
		FGaussianSogChunkLoader::FLoadRange Range;
		TSharedPtr<struct FGaussianStreamingLoadResult> Result;
		bool bStarted = false;
		int32 LastRequestedFrame = 0;
	};

	void GatherDesiredChunks(const FVector& ViewOrigin, TSet<FGaussianStreamChunkKey>& OutDesired);
	void TraverseNode(const FGaussianLodTreeNode& Node, const FVector& ViewOrigin, TSet<FGaussianStreamChunkKey>& OutDesired) const;
	int32 SelectLodLevel(const FGaussianLodTreeNode& Node, const FVector& ViewOrigin) const;
	bool IsNodeRelevant(const FGaussianBounds& Bounds, const FVector& ViewOrigin) const;
	void SyncDesiredChunks(const TSet<FGaussianStreamChunkKey>& Desired);
	void StartPendingLoads();
	void ProcessCompletedLoads();
	void EvictExcessChunks();
	void RemoveResident(const FGaussianStreamChunkKey& Key, TArray<TObjectPtr<UGaussianAsset>>& OutAssetsPendingRelease);
	void ReleaseDeferredAssets(const TArray<TObjectPtr<UGaussianAsset>>& Assets);
	FString MakeChunkDirectoryForKey(const FGaussianStreamChunkKey& Key) const;
	FGaussianBounds BoundsForKey(const FGaussianStreamChunkKey& Key) const;
	FGaussianSogChunkLoader::FLoadRange RangeForKey(const FGaussianStreamChunkKey& Key) const;

	AGaussianStreamedSceneActor* Owner = nullptr;
	TObjectPtr<UGaussianStreamedSceneAsset> StreamedAsset;
	TObjectPtr<UGaussianScene> Scene;

	TMap<FGaussianStreamChunkKey, FResidentChunk> ResidentChunks;
	TArray<FPendingLoad> PendingLoads;
	TSet<FGaussianStreamChunkKey> DesiredKeys;
};
