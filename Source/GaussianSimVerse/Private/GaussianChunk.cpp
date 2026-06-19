// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianChunk.h"
#include "GaussianAsset.h"

bool UGaussianChunk::IsLoaded() const
{
	return Asset != nullptr && Asset->IsValidForRendering();
}
