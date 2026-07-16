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
		/** Lower = more important (center of view / closer). Used to order starts & commits. */
		float ViewPriority = TNumericLimits<float>::Max();
	};

	void GatherDesiredChunks(const FVector& ViewOrigin, TSet<FGaussianStreamChunkKey>& OutDesired, TSet<FGaussianStreamChunkKey>& OutPrefetch);
	void TraverseNode(
		const FGaussianLodTreeNode& Node,
		const FVector& ViewOrigin,
		int32 LodBias,
		int32 UnderfillLimit,
		bool bPreferFastPromote,
		int32 PrefetchDepth,
		TSet<FGaussianStreamChunkKey>& OutDesired,
		TSet<FGaussianStreamChunkKey>& OutPrefetch) const;
	int32 SelectLodLevel(const FGaussianLodTreeNode& Node, const FVector& ViewOrigin, int32 LodBias) const;
	/**
	 * PlayCanvas-style underfill: pick a display slice (prefer resident; motion loads coarse-first,
	 * idle targets optimal sooner) and progressive prefetch toward optimal.
	 */
	void SelectLeafDesiredAndPrefetch(
		const FGaussianLodTreeNode& Node,
		int32 OptimalLod,
		int32 UnderfillLimit,
		bool bPreferFastPromote,
		int32 PrefetchDepth,
		TSet<FGaussianStreamChunkKey>& OutDesired,
		TSet<FGaussianStreamChunkKey>& OutPrefetch) const;
	bool IsSliceResident(const FGaussianLodSlice& Slice, int32 LeafId) const;
	static int64 SumDesiredSplatCount(const TSet<FGaussianStreamChunkKey>& Desired);
	bool IsNodeRelevant(const FGaussianBounds& Bounds, const FVector& ViewOrigin) const;
	void SyncDesiredChunks(const TSet<FGaussianStreamChunkKey>& Desired);
	void EnqueuePrefetchLoads(const TSet<FGaussianStreamChunkKey>& PrefetchKeys);
	void StartPendingLoads();
	/** Drop excess/stale pending results so finished multi-MB payloads cannot pile up in RAM. */
	void TrimPendingLoadQueue();
	int32 CountStartedPendingLoads() const;
	int32 CountFinishedPendingLoads() const;
	/** Lower score = higher priority (looking at + nearby). */
	float ComputeViewPriority(const FGaussianBounds& Bounds) const;
	void RefreshPendingViewPriorities();
	/** @return Number of successful commits this update. */
	int32 ProcessCompletedLoads();
	void EvictExcessChunks();
	void UnloadSupersededChunks();
	/**
	 * Keep only KeepKey resident for a spatial source (leaf / environment).
	 * Removes sibling LOD slices so old+new never render together (atomic LOD swap).
	 * @return True if any sibling was removed.
	 */
	bool RemoveSiblingResidents(
		const FGaussianStreamChunkKey& KeepKey,
		TArray<TObjectPtr<UGaussianAsset>>& OutAssetsPendingRelease);
	/** Global pass: at most one resident slice per leaf (prefer Desired, else finest). */
	bool EnforceSingleResidentPerLeaf(TArray<TObjectPtr<UGaussianAsset>>& OutAssetsPendingRelease);
	void RemoveResident(const FGaussianStreamChunkKey& Key, TArray<TObjectPtr<UGaussianAsset>>& OutAssetsPendingRelease);
	void ReleaseDeferredAssets(const TArray<TObjectPtr<UGaussianAsset>>& Assets);
	FString MakeChunkDirectoryForKey(const FGaussianStreamChunkKey& Key) const;
	FGaussianBounds BoundsForKey(const FGaussianStreamChunkKey& Key) const;
	FGaussianSogChunkLoader::FLoadRange RangeForKey(const FGaussianStreamChunkKey& Key) const;
	bool ShouldResampleDesired(const FVector& ViewOrigin, const FVector& ViewDirection) const;
	int32 GetMaxStartsPerUpdate() const;
	int32 GetMaxCompletedLoadsPerUpdate() const;
	/** Adaptive splat commit budget: lower in motion, higher when idle/catching up. */
	int32 GetMaxCommitSplatsThisUpdate() const;
	bool NeedsDetailCatchUp() const;
	void MaybeEndBootstrap();
	int32 CountMissingDesiredChunks() const;
	bool IsCameraInMotionInternal() const;
	static bool IsSameSpatialSource(const FGaussianStreamChunkKey& A, const FGaussianStreamChunkKey& B);
	bool HasResidentReplacementFor(const FGaussianStreamChunkKey& Key, const TSet<FGaussianStreamChunkKey>& Desired) const;
	bool IsAwaitingLodReplacement(const FGaussianStreamChunkKey& Key, const TSet<FGaussianStreamChunkKey>& Desired) const;
	void ApplyDesiredAndPrefetch(const FVector& ViewOrigin, bool bUpdateSampledView, const FVector& ViewDirection);

	AGaussianStreamedSceneActor* Owner = nullptr;
	TObjectPtr<UGaussianStreamedSceneAsset> StreamedAsset;
	TObjectPtr<UGaussianScene> Scene;

	TMap<FGaussianStreamChunkKey, FResidentChunk> ResidentChunks;
	TArray<FPendingLoad> PendingLoads;
	TSet<FGaussianStreamChunkKey> DesiredKeys;
	TSet<FGaussianStreamChunkKey> PrefetchKeys;
	FVector LastSampledViewOrigin = FVector::ZeroVector;
	FVector LastSampledViewDirection = FVector::ForwardVector;
	/** Updated every frame for load priority (even when desired set is not resampled). */
	FVector PriorityViewOrigin = FVector::ZeroVector;
	FVector PriorityViewDirection = FVector::ForwardVector;
	bool bHasSampledViewOrigin = false;
	bool bHasSampledViewDirection = false;
	bool bHasPriorityView = false;
	int32 LastResampleFrame = 0;
	/** Tracks motion window for bias transitions (re-gather when motion ends). */
	bool bWasCameraInMotion = false;
	/** First fill prefers throughput; steady streaming prefers smoother FPS. */
	bool bBootstrapActive = true;
};
