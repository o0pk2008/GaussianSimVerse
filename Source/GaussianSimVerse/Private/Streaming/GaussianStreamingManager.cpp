// Copyright Epic Games, Inc. All Rights Reserved.

#include "Streaming/GaussianStreamingManager.h"
#include "Streaming/GaussianStreamingLoadResult.h"
#include "GaussianStreamedSceneActor.h"
#include "GaussianStreamedSceneAsset.h"
#include "GaussianScene.h"
#include "GaussianChunk.h"
#include "GaussianAsset.h"
#include "Rendering/GaussianRenderer.h"
#include "Rendering/GaussianRenderSettings.h"
#include "Rendering/GaussianGPUResources.h"
#include "GaussianSimVerse.h"
#include "Async/Async.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

namespace
{
	// Keep recently-undesired chunks briefly when nothing will replace them (left load radius).
	// Same-source LOD siblings are never co-resident once a replacement is committed (atomic swap).
	constexpr int32 StreamingUnloadGraceFrames = 12;
	constexpr int32 StreamingUnloadGraceFramesMotion = 8;
	// Lean memory path + atomic swap keep FPS stable even at high throughput (validated in-editor).
	// Defaults favor timely LOD; hard caps below still protect against page-file OOM.
	constexpr int32 MaxCompletedLoadsPerUpdateMotion = 3;
	constexpr int32 MaxCompletedLoadsPerUpdateBootstrap = 8;
	constexpr int32 MaxStartsPerUpdateBootstrap = 12;
	constexpr int32 MaxCompletedLoadsPerUpdateIdle = 4;
	constexpr int32 MaxStartsPerUpdateMotion = 8;
	constexpr int32 MaxStartsPerUpdateCatchUp = 12;
	constexpr int32 MaxStartsPerUpdateIdleExtra = 4;
	constexpr int32 MaxCompletedLoadsPerUpdateCatchUp = 8;
	// Short motion window so stop → promote happens quickly after the camera settles.
	constexpr int32 StreamingMotionWindowFrames = 4;
	constexpr float StreamingViewResampleDistanceCm = 12.0f;
	constexpr float StreamingViewResampleAngleDeg = 1.25f;
	// With underfill often 0, prefetch still helps stepwise promotion when levels are missing.
	constexpr int32 PrefetchDepthMotion = 1;
	constexpr int32 PrefetchDepthIdle = 2;

	FGaussianBounds ComputeCenteredBounds(const TArray<FGaussianSplatData>& Splats)
	{
		FGaussianBounds Bounds;
		if (Splats.Num() == 0)
		{
			return Bounds;
		}

		FVector3f Min = Splats[0].Position;
		FVector3f Max = Splats[0].Position;
		for (const FGaussianSplatData& Splat : Splats)
		{
			Min = Min.ComponentMin(Splat.Position);
			Max = Max.ComponentMax(Splat.Position);
		}
		Bounds.Origin = (Min + Max) * 0.5f;
		Bounds.Extent = (Max - Min) * 0.5f;
		return Bounds;
	}

	void PrepareStreamingChunkGpuData(FGaussianStreamingLoadResult& Result)
	{
		if (!Result.bSuccess || Result.Splats.Num() <= 0)
		{
			return;
		}

		Result.PreparedBounds = ComputeCenteredBounds(Result.Splats);
		const FVector3f WorldCenter = Result.PreparedBounds.Origin;
		for (FGaussianSplatData& Splat : Result.Splats)
		{
			Splat.Position -= WorldCenter;
		}

		GaussianGPU::ConvertSplatDataArray(Result.Splats, Result.PreparedGpuSplats);
		GaussianGPU::BuildPositionBuffer(Result.PreparedGpuSplats, Result.PreparedPositions);

		// Free CPU staging as soon as GPU layout exists — pending queues can hold many results.
		Result.Splats.Empty();
	}

	/** Hard caps still protect page file; raised after memory path no longer triples CPU copies. */
	constexpr int32 MaxConcurrentStartedLoads = 12;
	constexpr int32 MaxPendingLoadSlots = 48;
	constexpr int32 MaxFinishedPendingResults = 12;
}

void FGaussianStreamingManager::Initialize(
	AGaussianStreamedSceneActor* InOwner,
	UGaussianStreamedSceneAsset* InAsset,
	UGaussianScene* InScene)
{
	Owner = InOwner;
	StreamedAsset = InAsset;
	Scene = InScene;
	ResidentChunks.Reset();
	PendingLoads.Reset();
	DesiredKeys.Reset();
	PrefetchKeys.Reset();
	LastSampledViewOrigin = FVector::ZeroVector;
	LastSampledViewDirection = FVector::ForwardVector;
	PriorityViewOrigin = FVector::ZeroVector;
	PriorityViewDirection = FVector::ForwardVector;
	bHasSampledViewOrigin = false;
	bHasSampledViewDirection = false;
	bHasPriorityView = false;
	LastResampleFrame = 0;
	bWasCameraInMotion = false;
	bBootstrapActive = true;

	if (StreamedAsset)
	{
		FString LoadError;
		if (!StreamedAsset->EnsureLodTreeLoaded(LoadError))
		{
			UE_LOG(LogGaussianSimVerse, Warning, TEXT("Failed to load streamed LOD tree: %s"), *LoadError);
		}
	}
}

void FGaussianStreamingManager::Shutdown()
{
	TArray<TObjectPtr<UGaussianAsset>> AssetsToRelease;
	AssetsToRelease.Reserve(ResidentChunks.Num());

	if (Scene)
	{
		Scene->Chunks.Reset();
	}

	for (TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
	{
		if (Pair.Value.Asset)
		{
			AssetsToRelease.Add(Pair.Value.Asset);
		}
	}

	ResidentChunks.Reset();
	PendingLoads.Reset();
	DesiredKeys.Reset();
	PrefetchKeys.Reset();
	LastSampledViewOrigin = FVector::ZeroVector;
	LastSampledViewDirection = FVector::ForwardVector;
	PriorityViewOrigin = FVector::ZeroVector;
	PriorityViewDirection = FVector::ForwardVector;
	bHasSampledViewOrigin = false;
	bHasSampledViewDirection = false;
	bHasPriorityView = false;
	LastResampleFrame = 0;
	bWasCameraInMotion = false;
	bBootstrapActive = true;

	if (Scene && Scene->IsRegisteredWithRenderer())
	{
		FGaussianRenderer::Get().MarkSceneDirty(Scene, false);
		FGaussianRenderer::Get().FlushDirtySceneProxies();
	}

	ReleaseDeferredAssets(AssetsToRelease);

	Owner = nullptr;
	StreamedAsset = nullptr;
	Scene = nullptr;
}

int32 FGaussianStreamingManager::GetLoadedSplatCount() const
{
	int32 Total = 0;
	for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
	{
		if (Pair.Value.Asset)
		{
			Total += Pair.Value.Asset->GaussianCount;
		}
	}
	return Total;
}

bool FGaussianStreamingManager::IsNodeRelevant(const FGaussianBounds& Bounds, const FVector& ViewOrigin) const
{
	const float LoadRadius = GaussianSimVerse::RenderSettings::GetStreamingLoadRadius();
	const FVector Center(Bounds.Origin);
	const float DistSq = FVector::DistSquared(ViewOrigin, Center);
	const float Radius = Bounds.Extent.Size();
	const float MaxDist = LoadRadius + Radius;
	return DistSq <= MaxDist * MaxDist;
}

int32 FGaussianStreamingManager::SelectLodLevel(const FGaussianLodTreeNode& Node, const FVector& ViewOrigin, int32 LodBias) const
{
	if (!StreamedAsset || StreamedAsset->LodMeta.LodLevels <= 0)
	{
		return 0;
	}

	const FVector Center(Node.Bounds.Origin);
	const float Distance = FVector::Dist(ViewOrigin, Center);
	const float NodeRadius = FMath::Max(Node.Bounds.Extent.Size(), 1.0f);
	const float LodBaseDistance = GaussianSimVerse::RenderSettings::GetStreamingLodBaseDistance();
	const float LodMultiplier = GaussianSimVerse::RenderSettings::GetStreamingLodMultiplier();
	const float Metric = (Distance / NodeRadius) / FMath::Max(LodBaseDistance, 0.01f);
	const float ScaledMetric = FMath::Max(Metric, 1.0f) * FMath::Max(LodMultiplier, 0.01f);
	int32 LodLevel = FMath::FloorToInt(FMath::Log2(ScaledMetric));
	// LodBias shifts every node coarser (higher level) so the whole desired set fits the splat budget.
	LodLevel += FMath::Max(LodBias, 0);
	return FMath::Clamp(LodLevel, 0, StreamedAsset->LodMeta.LodLevels - 1);
}

bool FGaussianStreamingManager::IsSliceResident(const FGaussianLodSlice& Slice, int32 LeafId) const
{
	if (Slice.Count <= 0)
	{
		return false;
	}

	const FGaussianStreamChunkKey Key = FGaussianStreamChunkKey::MakeSlice(
		Slice.FileIndex,
		Slice.Offset,
		Slice.Count,
		Slice.LodLevel,
		LeafId);
	return ResidentChunks.Contains(Key);
}

void FGaussianStreamingManager::SelectLeafDesiredAndPrefetch(
	const FGaussianLodTreeNode& Node,
	int32 OptimalLod,
	int32 UnderfillLimit,
	bool bPreferFastPromote,
	int32 PrefetchDepth,
	TSet<FGaussianStreamChunkKey>& OutDesired,
	TSet<FGaussianStreamChunkKey>& OutPrefetch) const
{
	if (Node.LodSlices.Num() == 0 || !StreamedAsset)
	{
		return;
	}

	const int32 MaxLod = StreamedAsset->LodMeta.LodLevels - 1;
	const int32 ClampedOptimal = FMath::Clamp(OptimalLod, 0, MaxLod);
	const int32 Underfill = FMath::Max(UnderfillLimit, 0);
	const int32 CoarseCap = FMath::Min(MaxLod, ClampedOptimal + Underfill);

	// Index valid slices by LOD level (a leaf may skip some levels).
	TMap<int32, const FGaussianLodSlice*> SlicesByLod;
	for (const FGaussianLodSlice& Candidate : Node.LodSlices)
	{
		if (Candidate.Count <= 0 || !StreamedAsset->LodMeta.Filenames.IsValidIndex(Candidate.FileIndex))
		{
			continue;
		}
		// Prefer first occurrence if duplicates exist.
		if (!SlicesByLod.Contains(Candidate.LodLevel))
		{
			SlicesByLod.Add(Candidate.LodLevel, &Candidate);
		}
	}

	if (SlicesByLod.Num() == 0)
	{
		return;
	}

	const FGaussianLodSlice* DisplaySlice = nullptr;

	if (Underfill > 0)
	{
		// Prefer highest quality already-resident within [optimal, optimal+underfill].
		for (int32 Lod = ClampedOptimal; Lod <= CoarseCap; ++Lod)
		{
			if (const FGaussianLodSlice* const* Found = SlicesByLod.Find(Lod))
			{
				if (IsSliceResident(**Found, Node.LeafId))
				{
					DisplaySlice = *Found;
					break;
				}
			}
		}

		// Nothing resident yet:
		// - Motion: coarsest-in-range first (cheap paint, SuperSplat-like underfill).
		// - Idle/fast promote: target optimal (or nearest) so detail converges like SuperSplat viewer.
		if (!DisplaySlice)
		{
			if (bPreferFastPromote)
			{
				int32 BestDelta = MAX_int32;
				for (int32 Lod = ClampedOptimal; Lod <= CoarseCap; ++Lod)
				{
					if (const FGaussianLodSlice* const* Found = SlicesByLod.Find(Lod))
					{
						const int32 Delta = FMath::Abs(Lod - ClampedOptimal);
						if (Delta < BestDelta)
						{
							DisplaySlice = *Found;
							BestDelta = Delta;
						}
					}
				}
			}
			else
			{
				for (int32 Lod = CoarseCap; Lod >= ClampedOptimal; --Lod)
				{
					if (const FGaussianLodSlice* const* Found = SlicesByLod.Find(Lod))
					{
						DisplaySlice = *Found;
						break;
					}
				}
			}
		}
	}

	// Fallback: nearest available level to optimal (legacy / underfill disabled).
	if (!DisplaySlice)
	{
		int32 BestDelta = MAX_int32;
		for (const TPair<int32, const FGaussianLodSlice*>& Pair : SlicesByLod)
		{
			const int32 Delta = FMath::Abs(Pair.Key - ClampedOptimal);
			// Strict '<' keeps the higher-detail (lower) level on ties.
			if (Delta < BestDelta)
			{
				DisplaySlice = Pair.Value;
				BestDelta = Delta;
			}
		}
	}

	if (!DisplaySlice)
	{
		return;
	}

	const FGaussianStreamChunkKey DisplayKey = FGaussianStreamChunkKey::MakeSlice(
		DisplaySlice->FileIndex,
		DisplaySlice->Offset,
		DisplaySlice->Count,
		DisplaySlice->LodLevel,
		Node.LeafId);
	OutDesired.Add(DisplayKey);

	// Progressive prefetch toward optimal (depth 1 in motion, deeper when idle for faster catch-up).
	const int32 Depth = FMath::Max(PrefetchDepth, 1);
	if (DisplaySlice->LodLevel > ClampedOptimal)
	{
		int32 Prefetched = 0;
		for (int32 Lod = DisplaySlice->LodLevel - 1; Lod >= ClampedOptimal && Prefetched < Depth; --Lod)
		{
			if (const FGaussianLodSlice* const* Found = SlicesByLod.Find(Lod))
			{
				const FGaussianStreamChunkKey PrefetchKey = FGaussianStreamChunkKey::MakeSlice(
					(*Found)->FileIndex,
					(*Found)->Offset,
					(*Found)->Count,
					(*Found)->LodLevel,
					Node.LeafId);
				if (PrefetchKey != DisplayKey && !ResidentChunks.Contains(PrefetchKey))
				{
					OutPrefetch.Add(PrefetchKey);
					++Prefetched;
				}
			}
		}
	}
}

void FGaussianStreamingManager::TraverseNode(
	const FGaussianLodTreeNode& Node,
	const FVector& ViewOrigin,
	int32 LodBias,
	int32 UnderfillLimit,
	bool bPreferFastPromote,
	int32 PrefetchDepth,
	TSet<FGaussianStreamChunkKey>& OutDesired,
	TSet<FGaussianStreamChunkKey>& OutPrefetch) const
{
	if (!IsNodeRelevant(Node.Bounds, ViewOrigin))
	{
		return;
	}

	if (Node.IsLeaf())
	{
		if (Node.LodSlices.Num() == 0)
		{
			return;
		}

		const int32 OptimalLod = SelectLodLevel(Node, ViewOrigin, LodBias);
		SelectLeafDesiredAndPrefetch(
			Node,
			OptimalLod,
			UnderfillLimit,
			bPreferFastPromote,
			PrefetchDepth,
			OutDesired,
			OutPrefetch);
		return;
	}

	for (const FGaussianLodTreeNode& Child : Node.Children)
	{
		TraverseNode(Child, ViewOrigin, LodBias, UnderfillLimit, bPreferFastPromote, PrefetchDepth, OutDesired, OutPrefetch);
	}
}

int64 FGaussianStreamingManager::SumDesiredSplatCount(const TSet<FGaussianStreamChunkKey>& Desired)
{
	int64 Total = 0;
	for (const FGaussianStreamChunkKey& Key : Desired)
	{
		if (!Key.bEnvironment)
		{
			Total += FMath::Max(Key.Count, 0);
		}
	}
	return Total;
}

void FGaussianStreamingManager::GatherDesiredChunks(
	const FVector& ViewOrigin,
	TSet<FGaussianStreamChunkKey>& OutDesired,
	TSet<FGaussianStreamChunkKey>& OutPrefetch)
{
	OutDesired.Reset();
	OutPrefetch.Reset();
	if (!StreamedAsset)
	{
		return;
	}

	const bool bHasEnvironment = !StreamedAsset->LodMeta.EnvironmentRelativePath.IsEmpty();
	const int32 MaxSplats = GaussianSimVerse::RenderSettings::GetStreamingMaxLoadedSplats();
	const int32 MaxLodBias = FMath::Max(StreamedAsset->LodMeta.LodLevels - 1, 0);
	const int32 ConfiguredUnderfill = GaussianSimVerse::RenderSettings::GetStreamingLodUnderfillLimit();
	const bool bInMotion = IsCameraInMotionInternal();

	// Motion: full underfill + motion bias (smooth). Idle: tighter underfill + deeper prefetch (faster detail).
	const int32 UnderfillLimit = bInMotion
		? ConfiguredUnderfill
		: FMath::Min(ConfiguredUnderfill, 1);
	const bool bPreferFastPromote = !bInMotion || bBootstrapActive;
	const int32 PrefetchDepth = bInMotion ? PrefetchDepthMotion : PrefetchDepthIdle;

	// Motion bias: force coarser optimal LODs while the camera is moving (fewer fine loads / uploads).
	const int32 MotionBias = bInMotion
		? GaussianSimVerse::RenderSettings::GetStreamingMotionLodBias()
		: 0;

	// Raise LOD globally until the desired set fits the splat budget. This keeps the whole model
	// visible (uniformly coarser) instead of letting eviction punch holes when zoomed in, where the
	// distance-based per-leaf selection would otherwise request far more splats than the budget allows.
	// Start from MotionBias so motion-time coarseness is always applied before budget pressure.
	for (int32 LodBias = MotionBias; ; ++LodBias)
	{
		OutDesired.Reset();
		OutPrefetch.Reset();
		if (bHasEnvironment)
		{
			OutDesired.Add(FGaussianStreamChunkKey::MakeEnvironment());
		}
		TraverseNode(
			StreamedAsset->LodTree,
			ViewOrigin,
			LodBias,
			UnderfillLimit,
			bPreferFastPromote,
			PrefetchDepth,
			OutDesired,
			OutPrefetch);

		if (MaxSplats <= 0 || LodBias >= MaxLodBias)
		{
			break;
		}
		if (SumDesiredSplatCount(OutDesired) <= static_cast<int64>(MaxSplats))
		{
			break;
		}
	}

	// Prefetch keys must not also be desired (desired already loads via Sync).
	for (const FGaussianStreamChunkKey& Key : OutDesired)
	{
		OutPrefetch.Remove(Key);
	}
}

FGaussianBounds FGaussianStreamingManager::BoundsForKey(const FGaussianStreamChunkKey& Key) const
{
	if (Key.bEnvironment)
	{
		FString EnvDir;
		if (StreamedAsset && StreamedAsset->ResolveEnvironmentDirectory(EnvDir))
		{
			return StreamedAsset->LodMeta.SceneBounds;
		}
	}

	// Search tree for matching slice bounds by scanning leaves - expensive but only for debug/fallback.
	if (StreamedAsset)
	{
		TArray<const FGaussianLodTreeNode*> Stack;
		Stack.Add(&StreamedAsset->LodTree);
		while (Stack.Num() > 0)
		{
			const FGaussianLodTreeNode* Node = Stack.Pop(EAllowShrinking::No);
			if (Node->IsLeaf())
			{
				for (const FGaussianLodSlice& Slice : Node->LodSlices)
				{
					if (Slice.FileIndex == Key.FileIndex && Slice.Offset == Key.Offset && Slice.Count == Key.Count && Slice.LodLevel == Key.LodLevel)
					{
						return Node->Bounds;
					}
				}
			}
			else
			{
				for (const FGaussianLodTreeNode& Child : Node->Children)
				{
					Stack.Add(&Child);
				}
			}
		}
	}

	return StreamedAsset ? StreamedAsset->LodMeta.SceneBounds : FGaussianBounds();
}

FGaussianSogChunkLoader::FLoadRange FGaussianStreamingManager::RangeForKey(const FGaussianStreamChunkKey& Key) const
{
	FGaussianSogChunkLoader::FLoadRange Range;
	if (!Key.bEnvironment)
	{
		Range.Offset = Key.Offset;
		Range.Count = Key.Count;
	}
	return Range;
}

FString FGaussianStreamingManager::MakeChunkDirectoryForKey(const FGaussianStreamChunkKey& Key) const
{
	FString Directory;
	if (!StreamedAsset)
	{
		return Directory;
	}

	if (Key.bEnvironment)
	{
		StreamedAsset->ResolveEnvironmentDirectory(Directory);
	}
	else
	{
		StreamedAsset->ResolveChunkDirectory(Key.FileIndex, Directory);
	}
	return Directory;
}

void FGaussianStreamingManager::RemoveResident(
	const FGaussianStreamChunkKey& Key,
	TArray<TObjectPtr<UGaussianAsset>>& OutAssetsPendingRelease)
{
	if (FResidentChunk* Resident = ResidentChunks.Find(Key))
	{
		if (Scene && Resident->Chunk)
		{
			Scene->Chunks.Remove(Resident->Chunk);
		}
		if (Resident->Asset)
		{
			OutAssetsPendingRelease.Add(Resident->Asset);
		}
		ResidentChunks.Remove(Key);
	}
}

bool FGaussianStreamingManager::RemoveSiblingResidents(
	const FGaussianStreamChunkKey& KeepKey,
	TArray<TObjectPtr<UGaussianAsset>>& OutAssetsPendingRelease)
{
	TArray<FGaussianStreamChunkKey> ToRemove;
	for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
	{
		if (Pair.Key == KeepKey)
		{
			continue;
		}
		if (IsSameSpatialSource(Pair.Key, KeepKey))
		{
			ToRemove.Add(Pair.Key);
		}
	}

	for (const FGaussianStreamChunkKey& Key : ToRemove)
	{
		RemoveResident(Key, OutAssetsPendingRelease);
	}
	return ToRemove.Num() > 0;
}

bool FGaussianStreamingManager::EnforceSingleResidentPerLeaf(
	TArray<TObjectPtr<UGaussianAsset>>& OutAssetsPendingRelease)
{
	// Group resident keys by spatial identity.
	TMap<int32, TArray<FGaussianStreamChunkKey>> ByLeaf;
	TArray<FGaussianStreamChunkKey> EnvironmentKeys;
	ByLeaf.Reserve(ResidentChunks.Num());

	for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
	{
		if (Pair.Key.bEnvironment)
		{
			EnvironmentKeys.Add(Pair.Key);
			continue;
		}
		const int32 GroupId = (Pair.Key.LeafId != INDEX_NONE)
			? Pair.Key.LeafId
			: (Pair.Key.FileIndex * 73856093) ^ (Pair.Key.Offset * 19349663) ^ Pair.Key.Count;
		ByLeaf.FindOrAdd(GroupId).Add(Pair.Key);
	}

	bool bRemovedAny = false;
	auto KeepPreferred = [this, &OutAssetsPendingRelease, &bRemovedAny](const TArray<FGaussianStreamChunkKey>& Keys)
	{
		if (Keys.Num() <= 1)
		{
			return;
		}

		// Prefer desired (finest desired if multiple); else finest loaded (lowest LodLevel).
		FGaussianStreamChunkKey Keep = Keys[0];
		bool bFoundDesired = false;
		for (const FGaussianStreamChunkKey& Key : Keys)
		{
			if (!DesiredKeys.Contains(Key))
			{
				continue;
			}
			if (!bFoundDesired || Key.LodLevel < Keep.LodLevel)
			{
				Keep = Key;
				bFoundDesired = true;
			}
		}
		if (!bFoundDesired)
		{
			for (const FGaussianStreamChunkKey& Key : Keys)
			{
				if (Key.LodLevel < Keep.LodLevel)
				{
					Keep = Key;
				}
			}
		}

		if (RemoveSiblingResidents(Keep, OutAssetsPendingRelease))
		{
			bRemovedAny = true;
		}
	};

	for (const TPair<int32, TArray<FGaussianStreamChunkKey>>& Pair : ByLeaf)
	{
		KeepPreferred(Pair.Value);
	}
	KeepPreferred(EnvironmentKeys);
	return bRemovedAny;
}

void FGaussianStreamingManager::ReleaseDeferredAssets(const TArray<TObjectPtr<UGaussianAsset>>& Assets)
{
	for (UGaussianAsset* Asset : Assets)
	{
		if (Asset)
		{
			Asset->ReleaseGPUResources();
		}
	}
}

void FGaussianStreamingManager::SyncDesiredChunks(const TSet<FGaussianStreamChunkKey>& Desired)
{
	TArray<FGaussianStreamChunkKey> ToRemove;
	const int32 UnloadGrace = IsCameraInMotionInternal()
		? StreamingUnloadGraceFramesMotion
		: StreamingUnloadGraceFrames;
	for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
	{
		if (Desired.Contains(Pair.Key))
		{
			continue;
		}

		const bool bAwaitingSameSourceLod = IsAwaitingLodReplacement(Pair.Key, Desired);
		const bool bAlreadyHaveReplacement = HasResidentReplacementFor(Pair.Key, Desired);

		// LOD upgrade path: keep old slice until replacement is resident (prevents pop-out holes).
		if (bAwaitingSameSourceLod && !bAlreadyHaveReplacement)
		{
			continue;
		}

		if (bAlreadyHaveReplacement
			|| static_cast<int32>(GFrameNumber - Pair.Value.LastTouchedFrame) >= UnloadGrace)
		{
			ToRemove.Add(Pair.Key);
		}
	}

	TArray<TObjectPtr<UGaussianAsset>> AssetsToRelease;
	AssetsToRelease.Reserve(ToRemove.Num());
	for (const FGaussianStreamChunkKey& Key : ToRemove)
	{
		RemoveResident(Key, AssetsToRelease);
	}

	if (AssetsToRelease.Num() > 0 && Scene)
	{
		FGaussianRenderer::Get().MarkSceneDirty(Scene, false);
		ReleaseDeferredAssets(AssetsToRelease);
	}

	PendingLoads.RemoveAll([&Desired, UnloadGrace](const FPendingLoad& Load)
	{
		if (Desired.Contains(Load.Key) || Load.bStarted)
		{
			return false;
		}
		return static_cast<int32>(GFrameNumber - Load.LastRequestedFrame) >= UnloadGrace;
	});

	for (const FGaussianStreamChunkKey& Key : Desired)
	{
		if (ResidentChunks.Contains(Key))
		{
			ResidentChunks[Key].LastTouchedFrame = GFrameNumber;
			continue;
		}

		const bool bAlreadyPending = PendingLoads.ContainsByPredicate([&Key](const FPendingLoad& Load)
		{
			return Load.Key == Key;
		});
		if (bAlreadyPending)
		{
			for (FPendingLoad& Load : PendingLoads)
			{
				if (Load.Key == Key)
				{
					Load.LastRequestedFrame = GFrameNumber;
					Load.ViewPriority = ComputeViewPriority(Load.LocalBounds);
					break;
				}
			}
			continue;
		}

		const FString ChunkDirectory = MakeChunkDirectoryForKey(Key);
		if (ChunkDirectory.IsEmpty())
		{
			continue;
		}

		// Make room for desired loads by dropping unstarted prefetch/stale slots first.
		if (PendingLoads.Num() >= MaxPendingLoadSlots)
		{
			for (int32 Index = PendingLoads.Num() - 1; Index >= 0 && PendingLoads.Num() >= MaxPendingLoadSlots; --Index)
			{
				if (!PendingLoads[Index].bStarted && !Desired.Contains(PendingLoads[Index].Key))
				{
					PendingLoads.RemoveAtSwap(Index);
				}
			}
		}
		if (PendingLoads.Num() >= MaxPendingLoadSlots)
		{
			continue;
		}

		FPendingLoad Pending;
		Pending.Key = Key;
		Pending.ChunkDirectory = ChunkDirectory;
		Pending.LocalBounds = BoundsForKey(Key);
		Pending.Range = RangeForKey(Key);
		Pending.Result = MakeShared<FGaussianStreamingLoadResult>();
		Pending.Result->Key = Key;
		Pending.LastRequestedFrame = GFrameNumber;
		Pending.ViewPriority = ComputeViewPriority(Pending.LocalBounds);
		PendingLoads.Add(MoveTemp(Pending));
	}
}

void FGaussianStreamingManager::EnqueuePrefetchLoads(const TSet<FGaussianStreamChunkKey>& Prefetch)
{
	for (const FGaussianStreamChunkKey& Key : Prefetch)
	{
		if (PendingLoads.Num() >= MaxPendingLoadSlots)
		{
			break;
		}

		if (ResidentChunks.Contains(Key))
		{
			continue;
		}

		const bool bAlreadyPending = PendingLoads.ContainsByPredicate([&Key](const FPendingLoad& Load)
		{
			return Load.Key == Key;
		});
		if (bAlreadyPending)
		{
			for (FPendingLoad& Load : PendingLoads)
			{
				if (Load.Key == Key)
				{
					Load.LastRequestedFrame = GFrameNumber;
					Load.ViewPriority = ComputeViewPriority(Load.LocalBounds);
					break;
				}
			}
			continue;
		}

		const FString ChunkDirectory = MakeChunkDirectoryForKey(Key);
		if (ChunkDirectory.IsEmpty())
		{
			continue;
		}

		FPendingLoad Pending;
		Pending.Key = Key;
		Pending.ChunkDirectory = ChunkDirectory;
		Pending.LocalBounds = BoundsForKey(Key);
		Pending.Range = RangeForKey(Key);
		Pending.Result = MakeShared<FGaussianStreamingLoadResult>();
		Pending.Result->Key = Key;
		Pending.LastRequestedFrame = GFrameNumber;
		Pending.ViewPriority = ComputeViewPriority(Pending.LocalBounds);
		PendingLoads.Add(MoveTemp(Pending));
	}
}

void FGaussianStreamingManager::ApplyDesiredAndPrefetch(
	const FVector& ViewOrigin,
	bool bUpdateSampledView,
	const FVector& ViewDirection)
{
	TSet<FGaussianStreamChunkKey> Desired;
	TSet<FGaussianStreamChunkKey> Prefetch;
	GatherDesiredChunks(ViewOrigin, Desired, Prefetch);
	DesiredKeys = MoveTemp(Desired);
	PrefetchKeys = MoveTemp(Prefetch);

	if (bUpdateSampledView)
	{
		LastSampledViewOrigin = ViewOrigin;
		LastSampledViewDirection = ViewDirection.GetSafeNormal();
		bHasSampledViewOrigin = true;
		bHasSampledViewDirection = true;
		LastResampleFrame = GFrameNumber;
	}

	SyncDesiredChunks(DesiredKeys);
	EnqueuePrefetchLoads(PrefetchKeys);
}

int32 FGaussianStreamingManager::CountMissingDesiredChunks() const
{
	int32 Missing = 0;
	for (const FGaussianStreamChunkKey& Key : DesiredKeys)
	{
		if (!ResidentChunks.Contains(Key))
		{
			++Missing;
		}
	}
	return Missing;
}

bool FGaussianStreamingManager::IsSameSpatialSource(const FGaussianStreamChunkKey& A, const FGaussianStreamChunkKey& B)
{
	if (A.bEnvironment || B.bEnvironment)
	{
		return A.bEnvironment && B.bEnvironment;
	}
	// Different LOD slices of the same leaf have different Offset/Count — match by LeafId.
	if (A.LeafId != INDEX_NONE && B.LeafId != INDEX_NONE)
	{
		return A.LeafId == B.LeafId;
	}
	return A.FileIndex == B.FileIndex && A.Offset == B.Offset && A.Count == B.Count;
}

bool FGaussianStreamingManager::HasResidentReplacementFor(
	const FGaussianStreamChunkKey& Key,
	const TSet<FGaussianStreamChunkKey>& Desired) const
{
	for (const FGaussianStreamChunkKey& DesiredKey : Desired)
	{
		if (DesiredKey == Key || !IsSameSpatialSource(Key, DesiredKey))
		{
			continue;
		}
		if (ResidentChunks.Contains(DesiredKey))
		{
			return true;
		}
	}
	return false;
}

bool FGaussianStreamingManager::IsAwaitingLodReplacement(
	const FGaussianStreamChunkKey& Key,
	const TSet<FGaussianStreamChunkKey>& Desired) const
{
	for (const FGaussianStreamChunkKey& DesiredKey : Desired)
	{
		if (DesiredKey == Key)
		{
			return false;
		}
		if (IsSameSpatialSource(Key, DesiredKey))
		{
			return true;
		}
	}
	return false;
}

void FGaussianStreamingManager::MaybeEndBootstrap()
{
	if (!bBootstrapActive)
	{
		return;
	}

	// Stay in bootstrap until the first desired set is fully resident (or empty).
	if (DesiredKeys.Num() > 0 && CountMissingDesiredChunks() == 0 && PendingLoads.Num() == 0)
	{
		bBootstrapActive = false;
	}
}

bool FGaussianStreamingManager::NeedsDetailCatchUp() const
{
	return CountMissingDesiredChunks() > 0
		|| PrefetchKeys.Num() > 0
		|| PendingLoads.Num() > 0;
}

int32 FGaussianStreamingManager::CountStartedPendingLoads() const
{
	int32 Count = 0;
	for (const FPendingLoad& Load : PendingLoads)
	{
		if (Load.bStarted && Load.Result.IsValid() && !Load.Result->IsFinished())
		{
			++Count;
		}
	}
	return Count;
}

int32 FGaussianStreamingManager::CountFinishedPendingLoads() const
{
	int32 Count = 0;
	for (const FPendingLoad& Load : PendingLoads)
	{
		if (Load.bStarted && Load.Result.IsValid() && Load.Result->IsFinished() && Load.Result->bSuccess)
		{
			++Count;
		}
	}
	return Count;
}

float FGaussianStreamingManager::ComputeViewPriority(const FGaussianBounds& Bounds) const
{
	if (!bHasPriorityView)
	{
		// Fallback: closer to last sample / origin first.
		const FVector Origin = bHasSampledViewOrigin ? LastSampledViewOrigin : FVector::ZeroVector;
		return FVector::Dist(Origin, FVector(Bounds.Origin));
	}

	const FVector Center(Bounds.Origin);
	const FVector ToCenter = Center - PriorityViewOrigin;
	const float Dist = FMath::Max(static_cast<float>(ToCenter.Size()), 1.0f);
	const FVector Dir = PriorityViewDirection.GetSafeNormal();
	float CosFacing = 0.0f;
	if (!Dir.IsNearlyZero())
	{
		CosFacing = static_cast<float>(FVector::DotProduct(ToCenter / Dist, Dir));
	}

	// 0 when looking straight at the chunk, grows when off-axis / behind the camera.
	// Behind (cos < 0) is heavily deprioritized so side/back upgrades wait.
	const float FacingPenalty = (CosFacing > 0.0f)
		? (1.0f - CosFacing)           // 0..1 in front hemisphere
		: (2.0f - CosFacing);          // 2..3 behind camera

	// Prefer near + on-axis. Distance in meters-ish scale keeps numbers stable.
	return (Dist * 0.01f) * (0.2f + FacingPenalty) + FacingPenalty * 50.0f;
}

void FGaussianStreamingManager::RefreshPendingViewPriorities()
{
	for (FPendingLoad& Load : PendingLoads)
	{
		if (!Load.bStarted || (Load.Result.IsValid() && !Load.Result->IsFinished()))
		{
			Load.ViewPriority = ComputeViewPriority(Load.LocalBounds);
		}
		else if (Load.Result.IsValid() && Load.Result->IsFinished())
		{
			// Keep finished jobs prioritised for commit order as well.
			Load.ViewPriority = ComputeViewPriority(Load.LocalBounds);
		}
	}
}

void FGaussianStreamingManager::TrimPendingLoadQueue()
{
	// 1) Drop unstarted non-desired with worst view priority when queue is too large.
	while (PendingLoads.Num() > MaxPendingLoadSlots)
	{
		int32 WorstIndex = INDEX_NONE;
		float WorstScore = -1.0f;
		for (int32 Index = 0; Index < PendingLoads.Num(); ++Index)
		{
			const FPendingLoad& Load = PendingLoads[Index];
			if (Load.bStarted || DesiredKeys.Contains(Load.Key))
			{
				continue;
			}
			if (Load.ViewPriority > WorstScore)
			{
				WorstScore = Load.ViewPriority;
				WorstIndex = Index;
			}
		}
		if (WorstIndex == INDEX_NONE)
		{
			break;
		}
		PendingLoads.RemoveAtSwap(WorstIndex);
	}

	// 2) Drop finished non-desired results waiting for commit — these hold multi-MB GPU staging arrays.
	// Prefer dropping off-axis / behind-camera first so in-view upgrades keep their slots.
	int32 FinishedSuccess = CountFinishedPendingLoads();
	while (FinishedSuccess > MaxFinishedPendingResults)
	{
		int32 WorstIndex = INDEX_NONE;
		float WorstScore = -1.0f;
		for (int32 Index = 0; Index < PendingLoads.Num(); ++Index)
		{
			FPendingLoad& Load = PendingLoads[Index];
			if (!Load.bStarted || !Load.Result.IsValid() || !Load.Result->IsFinished() || !Load.Result->bSuccess)
			{
				continue;
			}
			if (DesiredKeys.Contains(Load.Key))
			{
				continue;
			}
			if (Load.ViewPriority > WorstScore)
			{
				WorstScore = Load.ViewPriority;
				WorstIndex = Index;
			}
		}
		if (WorstIndex == INDEX_NONE)
		{
			break;
		}
		FPendingLoad& Load = PendingLoads[WorstIndex];
		Load.Result->Splats.Empty();
		Load.Result->PreparedGpuSplats.Empty();
		Load.Result->PreparedPositions.Empty();
		Load.Result->ShCoefficients.Empty();
		PendingLoads.RemoveAtSwap(WorstIndex);
		--FinishedSuccess;
	}
}

int32 FGaussianStreamingManager::GetMaxStartsPerUpdate() const
{
	const int32 Steady = GaussianSimVerse::RenderSettings::GetStreamingMaxLoadsPerFrame();
	const int32 InFlight = CountStartedPendingLoads();
	const int32 Room = FMath::Max(0, MaxConcurrentStartedLoads - InFlight);
	if (Room <= 0)
	{
		return 0;
	}

	int32 Wanted = Steady;
	if (bBootstrapActive)
	{
		Wanted = FMath::Max(Steady, MaxStartsPerUpdateBootstrap);
	}
	else if (IsCameraInMotionInternal())
	{
		Wanted = FMath::Min(Steady, MaxStartsPerUpdateMotion);
	}
	else if (NeedsDetailCatchUp())
	{
		Wanted = FMath::Max(Steady, MaxStartsPerUpdateCatchUp);
	}
	else
	{
		Wanted = Steady + MaxStartsPerUpdateIdleExtra;
	}

	// Also stop starting when too many finished results are already waiting to commit (RAM).
	if (CountFinishedPendingLoads() >= MaxFinishedPendingResults)
	{
		return 0;
	}

	return FMath::Clamp(FMath::Min(Wanted, Room), 0, MaxConcurrentStartedLoads);
}

int32 FGaussianStreamingManager::GetMaxCompletedLoadsPerUpdate() const
{
	if (bBootstrapActive)
	{
		return MaxCompletedLoadsPerUpdateBootstrap;
	}
	// While the camera is moving, keep commits low even if desired is incomplete.
	if (IsCameraInMotionInternal())
	{
		return MaxCompletedLoadsPerUpdateMotion;
	}
	if (NeedsDetailCatchUp())
	{
		return MaxCompletedLoadsPerUpdateCatchUp;
	}
	return MaxCompletedLoadsPerUpdateIdle;
}

int32 FGaussianStreamingManager::GetMaxCommitSplatsThisUpdate() const
{
	const int32 Base = GaussianSimVerse::RenderSettings::GetStreamingMaxCommitSplatsPerFrame();
	if (Base <= 0)
	{
		return 0; // unlimited
	}
	if (bBootstrapActive)
	{
		return Base * 2;
	}
	if (IsCameraInMotionInternal())
	{
		// FPS is stable with lean commit path — allow full base budget while moving.
		return Base;
	}
	if (NeedsDetailCatchUp())
	{
		// Settled / incomplete desired: push commits harder (FPS remains stable with lean path).
		return static_cast<int32>(Base * 2.0f);
	}
	return Base;
}

void FGaussianStreamingManager::StartPendingLoads()
{
	const int32 MaxStarts = GetMaxStartsPerUpdate();
	if (MaxStarts <= 0)
	{
		return;
	}

	// Sort unstarted jobs: Desired first, then best view priority (looking-at / near).
	TArray<int32> UnstartedOrder;
	UnstartedOrder.Reserve(PendingLoads.Num());
	for (int32 Index = 0; Index < PendingLoads.Num(); ++Index)
	{
		if (!PendingLoads[Index].bStarted)
		{
			UnstartedOrder.Add(Index);
		}
	}
	UnstartedOrder.Sort([this](const int32 A, const int32 B)
	{
		const FPendingLoad& LA = PendingLoads[A];
		const FPendingLoad& LB = PendingLoads[B];
		const bool bDesiredA = DesiredKeys.Contains(LA.Key);
		const bool bDesiredB = DesiredKeys.Contains(LB.Key);
		if (bDesiredA != bDesiredB)
		{
			return bDesiredA;
		}
		if (LA.ViewPriority != LB.ViewPriority)
		{
			return LA.ViewPriority < LB.ViewPriority;
		}
		// Prefer finer LOD upgrades slightly when priorities tie (lower LodLevel first).
		return LA.Key.LodLevel < LB.Key.LodLevel;
	});

	int32 Started = 0;
	const int32 MaxSplats = GaussianSimVerse::RenderSettings::GetStreamingMaxLoadedSplats();

	for (const int32 Index : UnstartedOrder)
	{
		if (Started >= MaxStarts || !PendingLoads.IsValidIndex(Index))
		{
			break;
		}

		FPendingLoad& Pending = PendingLoads[Index];
		if (Pending.bStarted)
		{
			continue;
		}

		const bool bIsDesired = DesiredKeys.Contains(Pending.Key);
		if (MaxSplats > 0 && !bIsDesired && GetLoadedSplatCount() >= MaxSplats)
		{
			continue;
		}

		Pending.bStarted = true;
		++Started;

		const FString Directory = Pending.ChunkDirectory;
		const FGaussianSogChunkLoader::FLoadRange Range = Pending.Range;
		const TSharedPtr<FGaussianStreamingLoadResult> Result = Pending.Result;

		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Directory, Range, Result]()
		{
			Result->bSuccess = FGaussianSogChunkLoader::LoadDirectory(
				Directory,
				Result->Splats,
				Result->Error,
				&Result->ShCoefficients,
				&Result->ImportedShDegree,
				&Range);
			if (Result->bSuccess)
			{
				PrepareStreamingChunkGpuData(*Result);
			}
			else
			{
				Result->Splats.Empty();
				Result->ShCoefficients.Empty();
			}
			Result->MarkFinished();
		});
	}
}

bool FGaussianStreamingManager::ShouldResampleDesired(const FVector& ViewOrigin, const FVector& ViewDirection) const
{
	if (!bHasSampledViewOrigin)
	{
		return true;
	}

	const bool bMovedFarEnough = FVector::DistSquared(ViewOrigin, LastSampledViewOrigin)
		>= FMath::Square(StreamingViewResampleDistanceCm);
	if (bMovedFarEnough)
	{
		return true;
	}

	if (!bHasSampledViewDirection)
	{
		return true;
	}

	const FVector CurrentDirection = ViewDirection.GetSafeNormal();
	const FVector LastDirection = LastSampledViewDirection.GetSafeNormal();
	if (CurrentDirection.IsNearlyZero() || LastDirection.IsNearlyZero())
	{
		return false;
	}

	const float Dot = FMath::Clamp(FVector::DotProduct(CurrentDirection, LastDirection), -1.0f, 1.0f);
	const float DeltaDeg = FMath::RadiansToDegrees(FMath::Acos(Dot));
	return DeltaDeg >= StreamingViewResampleAngleDeg;
}

bool FGaussianStreamingManager::IsCameraInMotionInternal() const
{
	return (GFrameNumber - LastResampleFrame) <= StreamingMotionWindowFrames;
}

bool FGaussianStreamingManager::IsCameraInMotion() const
{
	return IsCameraInMotionInternal();
}

int32 FGaussianStreamingManager::ProcessCompletedLoads()
{
	if (!Owner || !Scene || !StreamedAsset)
	{
		return 0;
	}

	auto EstimateIncomingSplats = [](const FPendingLoad& Pending) -> int32
	{
		if (!Pending.Result.IsValid())
		{
			return FMath::Max(Pending.Key.Count, 0);
		}
		if (Pending.Result->PreparedGpuSplats.Num() > 0)
		{
			return Pending.Result->PreparedGpuSplats.Num();
		}
		if (Pending.Result->Splats.Num() > 0)
		{
			return Pending.Result->Splats.Num();
		}
		return FMath::Max(Pending.Key.Count, 0);
	};

	// Drop failed jobs; collect successful ready keys for budgeted commit.
	TArray<FGaussianStreamChunkKey> ReadyKeys;
	TArray<int32> ReadySplatCounts;
	ReadyKeys.Reserve(PendingLoads.Num());
	ReadySplatCounts.Reserve(PendingLoads.Num());

	for (int32 Index = PendingLoads.Num() - 1; Index >= 0; --Index)
	{
		FPendingLoad& Pending = PendingLoads[Index];
		if (!Pending.bStarted || !Pending.Result.IsValid() || !Pending.Result->IsFinished())
		{
			continue;
		}

		if (!Pending.Result->bSuccess)
		{
			if (!Pending.Result->Error.IsEmpty())
			{
				UE_LOG(LogGaussianSimVerse, Warning, TEXT("Streamed chunk load failed (%s): %s"), *Pending.Key.KeyString, *Pending.Result->Error);
			}
			PendingLoads.RemoveAtSwap(Index);
			continue;
		}

		ReadyKeys.Add(Pending.Key);
		ReadySplatCounts.Add(EstimateIncomingSplats(Pending));
	}

	if (ReadyKeys.Num() == 0)
	{
		return 0;
	}

	// Desired first, then best view priority (center of screen / near), then smallest-first.
	TArray<float> ReadyPriorities;
	ReadyPriorities.Reserve(ReadyKeys.Num());
	for (const FGaussianStreamChunkKey& Key : ReadyKeys)
	{
		float Priority = TNumericLimits<float>::Max();
		for (const FPendingLoad& Load : PendingLoads)
		{
			if (Load.Key == Key)
			{
				Priority = Load.ViewPriority;
				break;
			}
		}
		ReadyPriorities.Add(Priority);
	}

	TArray<int32> Order;
	Order.Reserve(ReadyKeys.Num());
	for (int32 i = 0; i < ReadyKeys.Num(); ++i)
	{
		Order.Add(i);
	}
	Order.Sort([this, &ReadyKeys, &ReadySplatCounts, &ReadyPriorities](const int32 A, const int32 B)
	{
		const bool bDesiredA = DesiredKeys.Contains(ReadyKeys[A]);
		const bool bDesiredB = DesiredKeys.Contains(ReadyKeys[B]);
		if (bDesiredA != bDesiredB)
		{
			return bDesiredA; // desired before prefetch
		}
		if (ReadyPriorities[A] != ReadyPriorities[B])
		{
			return ReadyPriorities[A] < ReadyPriorities[B]; // looking-at first
		}
		return ReadySplatCounts[A] < ReadySplatCounts[B];
	});

	bool bSceneDirty = false;
	int32 CompletedThisUpdate = 0;
	int32 SplatsCommittedThisUpdate = 0;
	const int32 MaxCompletedThisUpdate = GetMaxCompletedLoadsPerUpdate();
	const int32 MaxCommitSplats = GetMaxCommitSplatsThisUpdate();

	for (const int32 OrderIndex : Order)
	{
		if (CompletedThisUpdate >= MaxCompletedThisUpdate)
		{
			break;
		}

		const FGaussianStreamChunkKey TargetKey = ReadyKeys[OrderIndex];
		const int32 IncomingSplats = ReadySplatCounts[OrderIndex];

		// Soft splat budget: always allow the first commit this update so a single huge chunk
		// larger than the budget cannot stall streaming forever.
		if (MaxCommitSplats > 0
			&& CompletedThisUpdate > 0
			&& (SplatsCommittedThisUpdate + IncomingSplats) > MaxCommitSplats)
		{
			break;
		}

		const int32 PendingIndex = PendingLoads.IndexOfByPredicate([&TargetKey](const FPendingLoad& Load)
		{
			return Load.Key == TargetKey
				&& Load.bStarted
				&& Load.Result.IsValid()
				&& Load.Result->IsFinished()
				&& Load.Result->bSuccess;
		});
		if (PendingIndex == INDEX_NONE)
		{
			continue;
		}

		// Already resident (duplicate finish) — drop the pending slot.
		if (ResidentChunks.Contains(TargetKey))
		{
			if (PendingLoads[PendingIndex].Result.IsValid())
			{
				PendingLoads[PendingIndex].Result->Splats.Empty();
				PendingLoads[PendingIndex].Result->PreparedGpuSplats.Empty();
				PendingLoads[PendingIndex].Result->PreparedPositions.Empty();
				PendingLoads[PendingIndex].Result->ShCoefficients.Empty();
			}
			PendingLoads.RemoveAtSwap(PendingIndex);
			continue;
		}

		// Hard resident budget: refuse commit that would exceed MaxLoadedSplats (except empty scene).
		const int32 MaxResidentSplats = GaussianSimVerse::RenderSettings::GetStreamingMaxLoadedSplats();
		const int32 LoadedNow = GetLoadedSplatCount();
		if (MaxResidentSplats > 0
			&& LoadedNow > 0
			&& (LoadedNow + IncomingSplats) > MaxResidentSplats)
		{
			// Estimate splat count freed by replacing same-leaf siblings (do not free until we know it helps).
			int32 SiblingSplatCount = 0;
			for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
			{
				if (Pair.Key != TargetKey && IsSameSpatialSource(Pair.Key, TargetKey) && Pair.Value.Asset)
				{
					SiblingSplatCount += FMath::Max(Pair.Value.Asset->GaussianCount, 0);
				}
			}

			const int32 AfterSwap = LoadedNow - SiblingSplatCount + IncomingSplats;
			if (SiblingSplatCount > 0 && AfterSwap <= MaxResidentSplats)
			{
				TArray<TObjectPtr<UGaussianAsset>> Freed;
				if (RemoveSiblingResidents(TargetKey, Freed))
				{
					ReleaseDeferredAssets(Freed);
					bSceneDirty = true;
				}
			}
			else if (!DesiredKeys.Contains(TargetKey))
			{
				// Prefetch that cannot fit: drop to free RAM.
				if (PendingLoads[PendingIndex].Result.IsValid())
				{
					PendingLoads[PendingIndex].Result->Splats.Empty();
					PendingLoads[PendingIndex].Result->PreparedGpuSplats.Empty();
					PendingLoads[PendingIndex].Result->PreparedPositions.Empty();
					PendingLoads[PendingIndex].Result->ShCoefficients.Empty();
				}
				PendingLoads.RemoveAtSwap(PendingIndex);
				continue;
			}
			else if ((GetLoadedSplatCount() + IncomingSplats) > MaxResidentSplats)
			{
				// Desired but still over budget even after a potential sibling swap — wait (no hole).
				continue;
			}
		}

		FPendingLoad& Pending = PendingLoads[PendingIndex];

		// Detach payload from the pending result first so we never hold two full copies
		// (Result + Asset) during commit — critical under low page-file conditions.
		const int32 ImportedShDegree = Pending.Result->ImportedShDegree;
		const FGaussianBounds PreparedBounds = Pending.Result->PreparedBounds;
		TArray<FGaussianSplatGPU> PreparedGpuSplats = MoveTemp(Pending.Result->PreparedGpuSplats);
		TArray<FVector4f> PreparedPositions = MoveTemp(Pending.Result->PreparedPositions);
		TArray<float> PreparedSh = MoveTemp(Pending.Result->ShCoefficients);
		TArray<FGaussianSplatData> PreparedStaging = MoveTemp(Pending.Result->Splats);
		Pending.Result->PreparedGpuSplats.Empty();
		Pending.Result->PreparedPositions.Empty();
		Pending.Result->ShCoefficients.Empty();
		Pending.Result->Splats.Empty();

		UGaussianAsset* Asset = NewObject<UGaussianAsset>(Owner, NAME_None, RF_Transient);
		if (PreparedGpuSplats.Num() > 0)
		{
			// Staging already consumed on worker; pass empty staging to avoid CPU duplicates.
			PreparedStaging.Empty();
			Asset->SetPreparedStreamingData(
				MoveTemp(PreparedStaging),
				MoveTemp(PreparedSh),
				ImportedShDegree,
				PreparedBounds,
				MoveTemp(PreparedGpuSplats),
				MoveTemp(PreparedPositions));
		}
		else
		{
			Asset->SetStagingData(PreparedStaging, MoveTemp(PreparedSh), ImportedShDegree);
			Asset->InitGPUResources();
			PreparedStaging.Empty();
		}

		UGaussianChunk* Chunk = NewObject<UGaussianChunk>(Scene, NAME_None, RF_Transient);
		Chunk->Asset = Asset;
		Chunk->LocalBounds = Pending.LocalBounds;
		Chunk->ActiveLOD = Pending.Key.LodLevel;
		Chunk->LoadState = EGaussianChunkLoadState::Loaded;
		Chunk->StreamingKey = Pending.Key;

		Scene->Chunks.Add(Chunk);

		FResidentChunk Resident;
		Resident.Chunk = Chunk;
		Resident.Asset = Asset;
		Resident.Key = TargetKey;
		Resident.LastTouchedFrame = GFrameNumber;
		ResidentChunks.Add(TargetKey, MoveTemp(Resident));

		PendingLoads.RemoveAtSwap(PendingIndex);
		bSceneDirty = true;
		++CompletedThisUpdate;
		SplatsCommittedThisUpdate += IncomingSplats;
	}

	// Sibling drop is deferred until after ApplyDesiredAndPrefetch in UpdateStreaming so underfill
	// can promote Desired to the newly resident finer slice, then EnforceSingleResidentPerLeaf
	// keeps only one slice per leaf before FlushDirtySceneProxies (no double-draw frame).
	if (bSceneDirty)
	{
		if (Owner)
		{
			Owner->NotifyStreamingChunkLoaded();
		}
		FGaussianRenderer::Get().MarkSceneDirty(Scene, false);
	}

	return CompletedThisUpdate;
}

void FGaussianStreamingManager::UnloadSupersededChunks()
{
	if (!Scene)
	{
		return;
	}

	TArray<TObjectPtr<UGaussianAsset>> AssetsToRelease;

	// Hard rule: never keep two LOD slices of the same leaf resident (atomic display).
	// Prefer DesiredKeys member when present, else the finest loaded slice.
	EnforceSingleResidentPerLeaf(AssetsToRelease);

	TArray<FGaussianStreamChunkKey> ToRemove;
	if (DesiredKeys.Num() > 0)
	{
		for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
		{
			if (DesiredKeys.Contains(Pair.Key))
			{
				continue;
			}
			if (HasResidentReplacementFor(Pair.Key, DesiredKeys))
			{
				ToRemove.Add(Pair.Key);
			}
		}
	}

	for (const FGaussianStreamChunkKey& Key : ToRemove)
	{
		RemoveResident(Key, AssetsToRelease);
	}

	if (AssetsToRelease.Num() > 0)
	{
		FGaussianRenderer::Get().MarkSceneDirty(Scene, false);
		ReleaseDeferredAssets(AssetsToRelease);
	}
}

void FGaussianStreamingManager::EvictExcessChunks()
{
	const int32 MaxSplats = GaussianSimVerse::RenderSettings::GetStreamingMaxLoadedSplats();
	if (MaxSplats <= 0)
	{
		return;
	}

	TArray<TObjectPtr<UGaussianAsset>> AssetsToRelease;
	while (GetLoadedSplatCount() > MaxSplats && ResidentChunks.Num() > 0)
	{
		// Only evict chunks that are NOT currently desired. Evicting a desired/visible chunk causes
		// the "holes while zoomed in" thrash: it gets re-requested and reloaded next frame. The LOD
		// bias in GatherDesiredChunks keeps the desired total within budget, so the excess here is
		// always stale (superseded / left-radius) chunks that are safe to drop first.
		FGaussianStreamChunkKey OldestKey;
		int32 OldestFrame = MAX_int32;
		bool bFound = false;
		for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
		{
			if (Pair.Key.bEnvironment || DesiredKeys.Contains(Pair.Key))
			{
				continue;
			}
			if (!bFound || Pair.Value.LastTouchedFrame < OldestFrame)
			{
				OldestFrame = Pair.Value.LastTouchedFrame;
				OldestKey = Pair.Key;
				bFound = true;
			}
		}

		if (bFound && ResidentChunks.Contains(OldestKey))
		{
			RemoveResident(OldestKey, AssetsToRelease);
		}
		else
		{
			// Everything left is desired (or environment): stop rather than evict visible geometry.
			break;
		}
	}

	if (AssetsToRelease.Num() > 0 && Scene)
	{
		FGaussianRenderer::Get().MarkSceneDirty(Scene, false);
		ReleaseDeferredAssets(AssetsToRelease);
	}
}

void FGaussianStreamingManager::UpdateStreaming(const FVector& ViewOrigin, const FVector& ViewDirection)
{
	if (!StreamedAsset || !Scene)
	{
		return;
	}

	// Always refresh view for load priority (even when desired set is not resampled this frame).
	PriorityViewOrigin = ViewOrigin;
	PriorityViewDirection = ViewDirection.GetSafeNormal();
	bHasPriorityView = true;
	RefreshPendingViewPriorities();

	// View-driven resample (camera moved/turned) vs motion-window settling (bias drop).
	const bool bViewChanged = ShouldResampleDesired(ViewOrigin, ViewDirection);
	// Treat this frame as in-motion if the view crossed the resample threshold, so MotionLodBias
	// applies on the first moving frame (before LastResampleFrame is refreshed).
	const bool bInMotion = IsCameraInMotionInternal() || bViewChanged;
	const bool bMotionEnded = bWasCameraInMotion && !bInMotion;
	bWasCameraInMotion = bInMotion;

	if (bViewChanged || bMotionEnded)
	{
		if (bViewChanged)
		{
			// Stamp motion window before gather so GetStreamingMotionLodBias applies this frame.
			LastResampleFrame = GFrameNumber;
		}
		ApplyDesiredAndPrefetch(ViewOrigin, /*bUpdateSampledView=*/bViewChanged, ViewDirection);
		bWasCameraInMotion = IsCameraInMotionInternal();
	}
	else
	{
		for (const FGaussianStreamChunkKey& Key : DesiredKeys)
		{
			if (FResidentChunk* Resident = ResidentChunks.Find(Key))
			{
				Resident->LastTouchedFrame = GFrameNumber;
			}
		}
		// Keep prefetch requests alive while waiting for progressive promotions.
		EnqueuePrefetchLoads(PrefetchKeys);
	}

	TrimPendingLoadQueue();
	StartPendingLoads();
	const int32 CompletedCount = ProcessCompletedLoads();
	// Underfill depends on residency: re-evaluate so newly loaded finer slices become desired
	// and superseded coarser siblings can unload without waiting for another camera move.
	if (CompletedCount > 0)
	{
		ApplyDesiredAndPrefetch(ViewOrigin, /*bUpdateSampledView=*/false, ViewDirection);
		TrimPendingLoadQueue();
		StartPendingLoads();
	}
	UnloadSupersededChunks();
	EvictExcessChunks();
	TrimPendingLoadQueue();
	MaybeEndBootstrap();
	FGaussianRenderer::Get().FlushDirtySceneProxies();

	if (GaussianSimVerse::RenderSettings::IsStreamingDebugEnabled() && Owner && Owner->GetWorld())
	{
		const float Duration = 0.05f;
		const FColor LeafColor(80, 200, 255);
		const FColor ActiveColor(255, 180, 40);
		TArray<const FGaussianLodTreeNode*> Stack;
		Stack.Add(&StreamedAsset->LodTree);
		while (Stack.Num() > 0)
		{
			const FGaussianLodTreeNode* Node = Stack.Pop(EAllowShrinking::No);
			const FBox Box = Node->Bounds.GetBox();
			const bool bRelevant = IsNodeRelevant(Node->Bounds, ViewOrigin);
			if (bRelevant)
			{
				DrawDebugBox(Owner->GetWorld(), Box.GetCenter(), Box.GetExtent(), Node->IsLeaf() ? ActiveColor : LeafColor, false, Duration, 0, 1.0f);
			}
			if (!Node->IsLeaf())
			{
				for (const FGaussianLodTreeNode& Child : Node->Children)
				{
					Stack.Add(&Child);
				}
			}
		}
	}

	if (GaussianSimVerse::RenderSettings::IsStreamingDebugOverlayEnabled() && Owner)
	{
		Owner->DrawStreamingDebugOverlay(*this);
	}
}
