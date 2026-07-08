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
#include "GaussianSimVerse.h"
#include "Async/Async.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

namespace
{
	// Keep recently-undesired chunks for a few frames to avoid visible holes during
	// LOD switching when camera moves across thresholds.
	constexpr int32 StreamingUnloadGraceFrames = 20;
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

	if (Scene && Scene->IsRegisteredWithRenderer())
	{
		FGaussianRenderer::Get().MarkSceneDirty(Scene);
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

int32 FGaussianStreamingManager::SelectLodLevel(const FGaussianLodTreeNode& Node, const FVector& ViewOrigin) const
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
	return FMath::Clamp(LodLevel, 0, StreamedAsset->LodMeta.LodLevels - 1);
}

void FGaussianStreamingManager::TraverseNode(
	const FGaussianLodTreeNode& Node,
	const FVector& ViewOrigin,
	TSet<FGaussianStreamChunkKey>& OutDesired) const
{
	if (!IsNodeRelevant(Node.Bounds, ViewOrigin))
	{
		return;
	}

	if (Node.IsLeaf())
	{
		const int32 LodLevel = SelectLodLevel(Node, ViewOrigin);
		const FGaussianLodSlice* Slice = Node.LodSlices.FindByPredicate(
			[LodLevel](const FGaussianLodSlice& Candidate)
			{
				return Candidate.LodLevel == LodLevel;
			});

		if (!Slice || Slice->Count <= 0 || !StreamedAsset->LodMeta.Filenames.IsValidIndex(Slice->FileIndex))
		{
			return;
		}

		OutDesired.Add(FGaussianStreamChunkKey::MakeSlice(Slice->FileIndex, Slice->Offset, Slice->Count, Slice->LodLevel));
		return;
	}

	for (const FGaussianLodTreeNode& Child : Node.Children)
	{
		TraverseNode(Child, ViewOrigin, OutDesired);
	}
}

void FGaussianStreamingManager::GatherDesiredChunks(const FVector& ViewOrigin, TSet<FGaussianStreamChunkKey>& OutDesired)
{
	OutDesired.Reset();
	if (!StreamedAsset)
	{
		return;
	}

	if (!StreamedAsset->LodMeta.EnvironmentRelativePath.IsEmpty())
	{
		OutDesired.Add(FGaussianStreamChunkKey::MakeEnvironment());
	}

	TraverseNode(StreamedAsset->LodTree, ViewOrigin, OutDesired);
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
	const bool bHasPendingLoads = PendingLoads.Num() > 0;
	for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
	{
		if (!bHasPendingLoads
			&& !Desired.Contains(Pair.Key)
			&& (GFrameNumber - Pair.Value.LastTouchedFrame) >= StreamingUnloadGraceFrames)
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
		FGaussianRenderer::Get().MarkSceneDirty(Scene);
		ReleaseDeferredAssets(AssetsToRelease);
	}

	PendingLoads.RemoveAll([&Desired](const FPendingLoad& Load)
	{
		if (Desired.Contains(Load.Key) || Load.bStarted)
		{
			return false;
		}
		return (GFrameNumber - Load.LastRequestedFrame) >= StreamingUnloadGraceFrames;
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

void FGaussianStreamingManager::StartPendingLoads()
{
	const int32 MaxStarts = GaussianSimVerse::RenderSettings::GetStreamingMaxLoadsPerFrame();
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
			Result->MarkFinished();
		});
	}
}

void FGaussianStreamingManager::ProcessCompletedLoads()
{
	if (!Owner || !Scene || !StreamedAsset)
	{
		return;
	}

	bool bSceneDirty = false;

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

		UGaussianAsset* Asset = NewObject<UGaussianAsset>(Owner, NAME_None, RF_Transient);
		Asset->SetStagingData(Pending.Result->Splats, MoveTemp(Pending.Result->ShCoefficients), Pending.Result->ImportedShDegree);

		UGaussianChunk* Chunk = NewObject<UGaussianChunk>(Scene, NAME_None, RF_Transient);
		Chunk->Asset = Asset;
		Chunk->LocalBounds = Pending.LocalBounds;
		Chunk->ActiveLOD = Pending.Key.LodLevel;
		Chunk->LoadState = EGaussianChunkLoadState::Loaded;
		Chunk->StreamingKey = Pending.Key;

		Scene->Chunks.Add(Chunk);
		Asset->InitGPUResources();

		FResidentChunk Resident;
		Resident.Chunk = Chunk;
		Resident.Asset = Asset;
		Resident.Key = Pending.Key;
		Resident.LastTouchedFrame = GFrameNumber;
		ResidentChunks.Add(Pending.Key, MoveTemp(Resident));

		PendingLoads.RemoveAtSwap(Index);
		bSceneDirty = true;
	}

	if (bSceneDirty)
	{
		if (Owner)
		{
			Owner->NotifyStreamingChunkLoaded();
		}
		FGaussianRenderer::Get().MarkSceneDirty(Scene);
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
		FGaussianStreamChunkKey OldestKey;
		int32 OldestFrame = MAX_int32;
		for (const TPair<FGaussianStreamChunkKey, FResidentChunk>& Pair : ResidentChunks)
		{
			if (!Pair.Key.bEnvironment && Pair.Value.LastTouchedFrame < OldestFrame)
			{
				OldestFrame = Pair.Value.LastTouchedFrame;
				OldestKey = Pair.Key;
			}
		}

		if (!OldestKey.KeyString.IsEmpty() && ResidentChunks.Contains(OldestKey))
		{
			RemoveResident(OldestKey, AssetsToRelease);
		}
		else
		{
			break;
		}
	}

	if (AssetsToRelease.Num() > 0 && Scene)
	{
		FGaussianRenderer::Get().MarkSceneDirty(Scene);
		ReleaseDeferredAssets(AssetsToRelease);
	}
}

void FGaussianStreamingManager::UpdateStreaming(const FVector& ViewOrigin)
{
	if (!StreamedAsset || !Scene)
	{
		return;
	}

	TSet<FGaussianStreamChunkKey> Desired;
	GatherDesiredChunks(ViewOrigin, Desired);
	DesiredKeys = Desired;
	SyncDesiredChunks(Desired);
	StartPendingLoads();
	ProcessCompletedLoads();
	EvictExcessChunks();

	if (GaussianSimVerse::RenderSettings::IsStreamingDebugEnabled() && Owner && Owner->GetWorld())
	{
		const float Duration = 0.0f;
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
