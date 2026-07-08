// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Streaming/GaussianLodTypes.h"

class GAUSSIANSIMVERSE_API FGaussianLodMetaReader
{
public:
	static bool ParseFile(const FString& LodMetaPath, FGaussianLodMetaData& OutMeta, FGaussianLodTreeNode& OutTree, FString& OutError);
};
