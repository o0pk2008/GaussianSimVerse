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
	// Same-source LOD upgrades keep the old slice until the new one is resident (no holes).
	constexpr int32 StreamingUnloadGraceFrames = 12;
	constexpr int32 StreamingUnloadGraceFramesMotion = 8;
	// Motion: few commits/frame to avoid GT/upload spikes; idle/bootstrap can catch up.
	constexpr int32 MaxCompletedLoadsPerUpdateSteady = 4;
	constexpr int32 MaxCompletedLoadsPerUpdateBootstrap = 8;
	constexpr int32 MaxStartsPerUpdateBootstrap = 12;
	constexpr int32 MaxCompletedLoadsPerUpdateIdle = 4;
	constexpr int32 MaxStartsPerUpdateMotion = 8;
	constexpr int32 MaxStartsPerUpdateIdleExtra = 4;
	constexpr int32 MaxCompletedLoadsPerUpdateCatchUp = 3;
	constexpr int32 StreamingMotionWindowFrames = 12;
	constexpr float StreamingViewResampleDistanceCm = 10.0f;
	constexpr float StreamingViewResampleAngleDeg = 1.0f;

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
	}
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
	LastSampledViewOrigin = FVector::ZeroVector;
	LastSampledViewDirection = FVector::ForwardVector;
	bHasSampledViewOrigin = false;
	bHasSampledViewDirection = false;
	LastResampleFrame = 0;
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
	LastSampledViewOrigin = FVector::ZeroVector;
	LastSampledViewDirection = FVector::ForwardVector;
	bHasSampledViewOrigin = false;
	bHasSampledViewDirection = false;
	LastResampleFrame = 0;
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

void FGaussianStreamingManager::TraverseNode(
	const FGaussianLodTreeNode& Node,
	const FVector& ViewOrigin,
	int32 LodBias,
	TSet<FGaussianStreamChunkKey>& OutDesired) const
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

		const int32 DesiredLod = SelectLodLevel(Node, ViewOrigin, LodBias);

		// A leaf may not publish every LOD level (common for object datasets, where a block
		// only exists at a subset of levels). Snap to the nearest available slice instead of
		// dropping the block entirely, which previously made blocks vanish at some distances/angles.
		const FGaussianLodSlice* Best = nullptr;
		int32 BestDelta = MAX_int32;
		for (const FGaussianLodSlice& Candidate : Node.LodSlices)
		{
			if (Candidate.Count <= 0 || !StreamedAsset->LodMeta.Filenames.IsValidIndex(Candidate.FileIndex))
			{
				continue;
			}

			const int32 Delta = FMath::Abs(Candidate.LodLevel - DesiredLod);
			// Slices are sorted ascending by level; strict '<' keeps the higher-detail (lower) level on ties.
			if (Delta < BestDelta)
			{
				Best = &Candidate;
				BestDelta = Delta;
			}
		}

		if (!Best)
		{
			return;
		}

		OutDesired.Add(FGaussianStreamChunkKey::MakeSlice(
			Best->FileIndex,
			Best->Offset,
			Best->Count,
			Best->LodLevel,
			Node.LeafId));
		return;
	}

	for (const FGaussianLodTreeNode& Child : Node.Children)
	{
		TraverseNode(Child, ViewOrigin, LodBias, OutDesired);
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

void FGaussianStreamingManager::GatherDesiredChunks(const FVector& ViewOrigin, TSet<FGaussianStreamChunkKey>& OutDesired)
{
	OutDesired.Reset();
	if (!StreamedAsset)
	{
		return;
	}

	const bool bHasEnvironment = !StreamedAsset->LodMeta.EnvironmentRelativePath.IsEmpty();
	const int32 MaxSplats = GaussianSimVerse::RenderSettings::GetStreamingMaxLoadedSplats();
	const int32 MaxLodBias = FMath::Max(StreamedAsset->LodMeta.LodLevels - 1, 0);

	// Raise LOD globally until the desired set fits the splat budget. This keeps the whole model
	// visible (uniformly coarser) instead of letting eviction punch holes when zoomed in, where the
	// distance-based per-leaf selection would otherwise request far more splats than the budget allows.
	for (int32 LodBias = 0; ; ++LodBias)
	{
		OutDesired.Reset();
		if (bHasEnvironment)
		{
			OutDesired.Add(FGaussianStreamChunkKey::MakeEnvironment());
		}
		TraverseNode(StreamedAsset->LodTree, ViewOrigin, LodBias, OutDesired);

		if (MaxSplats <= 0 || LodBias >= MaxLodBias)
		{
			break;
		}
		if (SumDesiredSplatCount(OutDesired) <= static_cast<int64>(MaxSplats))
		{
			break;
		}
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
		PendingLoads.Add(MoveTemp(Pending));
	}
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

int32 FGaussianStreamingManager::GetMaxStartsPerUpdate() const
{
	const int32 Steady = GaussianSimVerse::RenderSettings::GetStreamingMaxLoadsPerFrame();
	if (bBootstrapActive)
	{
		return FMath::Clamp(FMath::Max(Steady, MaxStartsPerUpdateBootstrap), 1, 16);
	}
	// Catch-up while desired set is incomplete: keep async workers fed so LOD swaps finish sooner.
	if (CountMissingDesiredChunks() > 0)
	{
		return FMath::Clamp(FMath::Max(Steady, MaxStartsPerUpdateMotion), 1, 16);
	}
	if (IsCameraInMotionInternal())
	{
		return FMath::Clamp(FMath::Max(Steady, MaxStartsPerUpdateMotion), 1, 16);
	}
	return FMath::Clamp(Steady + MaxStartsPerUpdateIdleExtra, 1, 16);
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
		return MaxCompletedLoadsPerUpdateSteady;
	}
	if (CountMissingDesiredChunks() > 0)
	{
		return MaxCompletedLoadsPerUpdateCatchUp;
	}
	return MaxCompletedLoadsPerUpdateIdle;
}

void FGaussianStreamingManager::StartPendingLoads()
{
	const int32 MaxStarts = GetMaxStartsPerUpdate();
	int32 Started = 0;

	for (FPendingLoad& Pending : PendingLoads)
	{
		if (Pending.bStarted || Started >= MaxStarts)
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

void FGaussianStreamingManager::ProcessCompletedLoads()
{
	if (!Owner || !Scene || !StreamedAsset)
	{
		return;
	}

	bool bSceneDirty = false;
	int32 CompletedThisUpdate = 0;
	const int32 MaxCompletedThisUpdate = GetMaxCompletedLoadsPerUpdate();

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

		if (CompletedThisUpdate >= MaxCompletedThisUpdate)
		{
			break;
		}

		UGaussianAsset* Asset = NewObject<UGaussianAsset>(Owner, NAME_None, RF_Transient);
		if (Pending.Result->PreparedGpuSplats.Num() > 0)
		{
			Asset->SetPreparedStreamingData(
				MoveTemp(Pending.Result->Splats),
				MoveTemp(Pending.Result->ShCoefficients),
				Pending.Result->ImportedShDegree,
				Pending.Result->PreparedBounds,
				MoveTemp(Pending.Result->PreparedGpuSplats),
				MoveTemp(Pending.Result->PreparedPositions));
		}
		else
		{
			Asset->SetStagingData(Pending.Result->Splats, MoveTemp(Pending.Result->ShCoefficients), Pending.Result->ImportedShDegree);
			Asset->InitGPUResources();
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
		Resident.Key = Pending.Key;
		Resident.LastTouchedFrame = GFrameNumber;
		ResidentChunks.Add(Pending.Key, MoveTemp(Resident));

		PendingLoads.RemoveAtSwap(Index);
		bSceneDirty = true;
		++CompletedThisUpdate;
	}

	if (bSceneDirty)
	{
		if (Owner)
		{
			Owner->NotifyStreamingChunkLoaded();
		}
		FGaussianRenderer::Get().MarkSceneDirty(Scene, false);
	}
}

void FGaussianStreamingManager::UnloadSupersededChunks()
{
	if (!Scene || DesiredKeys.Num() == 0)
	{
		return;
	}

	TArray<FGaussianStreamChunkKey> ToRemove;
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

	if (ToRemove.Num() == 0)
	{
		return;
	}

	TArray<TObjectPtr<UGaussianAsset>> AssetsToRelease;
	AssetsToRelease.Reserve(ToRemove.Num());
	for (const FGaussianStreamChunkKey& Key : ToRemove)
	{
		RemoveResident(Key, AssetsToRelease);
	}

	FGaussianRenderer::Get().MarkSceneDirty(Scene, false);
	ReleaseDeferredAssets(AssetsToRelease);
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

	if (ShouldResampleDesired(ViewOrigin, ViewDirection))
	{
		TSet<FGaussianStreamChunkKey> Desired;
		GatherDesiredChunks(ViewOrigin, Desired);
		DesiredKeys = Desired;
		LastSampledViewOrigin = ViewOrigin;
		LastSampledViewDirection = ViewDirection.GetSafeNormal();
		bHasSampledViewOrigin = true;
		bHasSampledViewDirection = true;
		LastResampleFrame = GFrameNumber;
		SyncDesiredChunks(DesiredKeys);
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
	}

	StartPendingLoads();
	ProcessCompletedLoads();
	UnloadSupersededChunks();
	EvictExcessChunks();
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
