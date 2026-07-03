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
		ChunkData.GPUBuffer = Asset->GetGPUBuffer();
		ChunkData.LocalToWorld = LocalToWorld;
		ChunkData.Bounds = Chunk->LocalBounds;
		ChunkData.GaussianCount = static_cast<uint32>(Asset->GaussianCount);
		ChunkData.ChunkIndex = ChunkIndex++;
		Chunks.Add(MoveTemp(ChunkData));
	}
}
