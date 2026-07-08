// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianChunk.h"
#include "GaussianAsset.h"

bool UGaussianChunk::IsLoaded() const
{
	return LoadState == EGaussianChunkLoadState::Loaded
		&& Asset != nullptr
		&& Asset->IsValidForRendering();
}
