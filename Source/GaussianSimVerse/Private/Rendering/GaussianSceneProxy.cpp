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

		// GPU splat positions are centered at 0 around Asset->Bounds.Origin (dataset-space center).
		// Non-streamed assets: LocalBounds.Origin is usually zero (whole model under actor).
		// Streamed assets: convert dataset-space center into actor-local offset via DatasetPivot so
		// moving/rotating the actor correctly moves the cloud (old formula locked chunks in world space).
		const bool bIsStreamedChunk = !Chunk->StreamingKey.KeyString.IsEmpty();
		FVector ChunkOffset = FVector(Chunk->LocalBounds.Origin);
		if (bIsStreamedChunk)
		{
			const FVector DatasetCenter = FVector(Asset->Bounds.Origin);
			if (InScene->bHasDatasetPivot)
			{
				ChunkOffset = DatasetCenter - InScene->DatasetPivot;
			}
			else
			{
				// Fallback: treat LocalBounds as dataset absolute if pivot missing.
				ChunkOffset = FVector(Chunk->LocalBounds.Origin) - InScene->DatasetPivot;
			}
		}
		ChunkData.LocalToWorld = LocalToWorld * FTranslationMatrix(ChunkOffset);

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
