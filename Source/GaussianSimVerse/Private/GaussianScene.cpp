// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianScene.h"
#include "GaussianAsset.h"
#include "Rendering/GaussianRenderer.h"

UGaussianScene::UGaussianScene()
{
}

void UGaussianScene::RegisterWithRenderer()
{
	if (!bRegisteredWithRenderer)
	{
		FGaussianRenderer::Get().RegisterScene(this);
		bRegisteredWithRenderer = true;
	}
	else
	{
		FGaussianRenderer::Get().MarkSceneDirty(this);
	}
}

void UGaussianScene::UnregisterFromRenderer()
{
	if (bRegisteredWithRenderer)
	{
		FGaussianRenderer::Get().UnregisterScene(this);
		bRegisteredWithRenderer = false;
	}
}

uint32 UGaussianScene::GetTotalGaussianCount() const
{
	uint32 Total = 0;
	for (const UGaussianChunk* Chunk : Chunks)
	{
		if (Chunk && Chunk->IsLoaded())
		{
			Total += static_cast<uint32>(Chunk->Asset->GaussianCount);
		}
	}
	return Total;
}

FGaussianBounds UGaussianScene::GetCombinedBounds() const
{
	FGaussianBounds Combined;
	bool bHasBounds = false;

	for (const UGaussianChunk* Chunk : Chunks)
	{
		if (!Chunk || !Chunk->IsLoaded())
		{
			continue;
		}

		const FGaussianBounds ChunkBounds = Chunk->LocalBounds;
		if (!bHasBounds)
		{
			Combined = ChunkBounds;
			bHasBounds = true;
		}
		else
		{
			const FBox Box = Combined.GetBox() + ChunkBounds.GetBox();
			Combined.Origin = FVector3f(Box.GetCenter());
			Combined.Extent = FVector3f(Box.GetExtent());
		}
	}

	return Combined;
}
