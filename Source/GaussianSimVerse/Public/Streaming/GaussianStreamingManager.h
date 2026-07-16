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
	void UpdateStreaming(const FVector& ViewOrigin, const FVector& ViewDirection);

	int32 GetLoadedChunkCount() const { return ResidentChunks.Num(); }
	int32 GetLoadedSplatCount() const;
	int32 GetPendingLoadCount() const { return PendingLoads.Num(); }
	int32 GetDesiredChunkCount() const { return DesiredKeys.Num(); }
	/** True while filling the first desired set; higher load concurrency for cold start. */
	bool IsBootstrapActive() const { return bBootstrapActive; }
	/** True while camera is actively moving/turning; prefer responsive LOD updates. */
	bool IsCameraInMotion() const;

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
	void TraverseNode(const FGaussianLodTreeNode& Node, const FVector& ViewOrigin, int32 LodBias, TSet<FGaussianStreamChunkKey>& OutDesired) const;
	int32 SelectLodLevel(const FGaussianLodTreeNode& Node, const FVector& ViewOrigin, int32 LodBias) const;
	static int64 SumDesiredSplatCount(const TSet<FGaussianStreamChunkKey>& Desired);
	bool IsNodeRelevant(const FGaussianBounds& Bounds, const FVector& ViewOrigin) const;
	void SyncDesiredChunks(const TSet<FGaussianStreamChunkKey>& Desired);
	void StartPendingLoads();
	void ProcessCompletedLoads();
	void EvictExcessChunks();
	void UnloadSupersededChunks();
	void RemoveResident(const FGaussianStreamChunkKey& Key, TArray<TObjectPtr<UGaussianAsset>>& OutAssetsPendingRelease);
	void ReleaseDeferredAssets(const TArray<TObjectPtr<UGaussianAsset>>& Assets);
	FString MakeChunkDirectoryForKey(const FGaussianStreamChunkKey& Key) const;
	FGaussianBounds BoundsForKey(const FGaussianStreamChunkKey& Key) const;
	FGaussianSogChunkLoader::FLoadRange RangeForKey(const FGaussianStreamChunkKey& Key) const;
	bool ShouldResampleDesired(const FVector& ViewOrigin, const FVector& ViewDirection) const;
	int32 GetMaxStartsPerUpdate() const;
	int32 GetMaxCompletedLoadsPerUpdate() const;
	void MaybeEndBootstrap();
	int32 CountMissingDesiredChunks() const;
	bool IsCameraInMotionInternal() const;
	static bool IsSameSpatialSource(const FGaussianStreamChunkKey& A, const FGaussianStreamChunkKey& B);
	bool HasResidentReplacementFor(const FGaussianStreamChunkKey& Key, const TSet<FGaussianStreamChunkKey>& Desired) const;
	bool IsAwaitingLodReplacement(const FGaussianStreamChunkKey& Key, const TSet<FGaussianStreamChunkKey>& Desired) const;

	AGaussianStreamedSceneActor* Owner = nullptr;
	TObjectPtr<UGaussianStreamedSceneAsset> StreamedAsset;
	TObjectPtr<UGaussianScene> Scene;

	TMap<FGaussianStreamChunkKey, FResidentChunk> ResidentChunks;
	TArray<FPendingLoad> PendingLoads;
	TSet<FGaussianStreamChunkKey> DesiredKeys;
	FVector LastSampledViewOrigin = FVector::ZeroVector;
	FVector LastSampledViewDirection = FVector::ForwardVector;
	bool bHasSampledViewOrigin = false;
	bool bHasSampledViewDirection = false;
	int32 LastResampleFrame = 0;
	/** First fill prefers throughput; steady streaming prefers smoother FPS. */
	bool bBootstrapActive = true;
};
