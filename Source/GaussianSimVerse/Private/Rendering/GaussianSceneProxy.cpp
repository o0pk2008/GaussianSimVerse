// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianRenderResources.h"
#include "GaussianScene.h"
#include "GaussianChunk.h"
#include "GaussianAsset.h"

FGaussianSceneProxy::FGaussianSceneProxy(const UGaussianScene* InScene)
{
	if (!InScene)
	{
		return;
	}

	LocalToWorld = InScene->WorldTransform.ToMatrixWithScale();
	WorldToLocal = LocalToWorld.Inverse();
	Bounds = InScene->GetCombinedBounds();
	TotalGaussianCount = InScene->GetTotalGaussianCount();
	bEnableRendering = InScene->bEnableRendering;
	DofMode = InScene->DofMode;
	bUseProxyDepthOfField = InScene->bUseProxyDepthOfField || (InScene->DofMode != EGaussianProxyDofMode::Off);
	DofFocalDistanceCm = InScene->DofFocalDistanceCm;
	DofCocScale = InScene->DofCocScale;
	DofMaxBlurRadiusPx = InScene->DofMaxBlurRadiusPx;
	DofProxyStencil = InScene->DofProxyStencil;
	SplatScale = InScene->SplatScale;
	AlphaCullThreshold = InScene->AlphaCullThreshold;
	CutoffK = InScene->CutoffK;
	CovarianceDilation = InScene->CovarianceDilation;
	ShBand = InScene->ShBand;
	ColorGrade = FGaussianColorGradeGPU::FromAdjustment(InScene->Colors);

	Chunks.Reserve(InScene->Chunks.Num());
	uint32 ChunkIndex = 0;
	for (const UGaussianChunk* Chunk : InScene->Chunks)
	{
		if (!Chunk || !Chunk->IsLoaded())
		{
			continue;
		}

		UGaussianAsset* Asset = Chunk->Asset;
		if (!Asset || !Asset->IsValidForRendering())
		{
			continue;
		}

		FGaussianChunkRenderData ChunkData;
		ChunkData.GPUBufferShared = Asset->GetGPUBufferShared();

		// Streamed GPU positions are centered at 0 (PrepareStreamingChunkGpuData subtracts center).
		// Place each chunk at (Asset.Bounds.Origin - DatasetPivot) in *actor local* space, then
		// apply the actor transform (scale/rot/trans). Matrix order matters for UE row-vectors:
		//   V' = V * T(offset) * Actor  →  Actor.TransformPosition(V + offset)  [offset is scaled]
		// Old order Actor * T(offset) applied offset AFTER scale → chunks scaled in place but
		// centers stayed put when the actor was scaled.
		const bool bIsStreamedChunk = !Chunk->StreamingKey.KeyString.IsEmpty();
		FVector ChunkOffset = FVector(Chunk->LocalBounds.Origin);
		if (bIsStreamedChunk)
		{
			const FVector DatasetCenter = FVector(Asset->Bounds.Origin);
			const FVector Pivot = InScene->bHasDatasetPivot ? InScene->DatasetPivot : FVector::ZeroVector;
			ChunkOffset = DatasetCenter - Pivot;
		}
		else if (Asset->bUsesDatasetCoordinates)
		{
			// Absolute dataset positions already; no extra local offset.
			ChunkOffset = FVector::ZeroVector;
		}
		ChunkData.LocalToWorld = FTranslationMatrix(ChunkOffset) * LocalToWorld;

		FGaussianBounds ChunkLocalBounds;
		ChunkLocalBounds.Origin = FVector3f::ZeroVector;
		ChunkLocalBounds.Extent = Asset->Bounds.Extent;
		ChunkData.Bounds = ChunkLocalBounds;
		if (ChunkData.Bounds.Extent.IsNearlyZero())
		{
			ChunkData.Bounds.Extent = Chunk->LocalBounds.Extent;
		}
		ChunkData.GaussianCount = static_cast<uint32>(Asset->GaussianCount);
		ChunkData.ChunkIndex = ChunkIndex++;
		ChunkData.ChunkLodLevel = Chunk->ActiveLOD;
		Chunks.Add(MoveTemp(ChunkData));
	}
}
