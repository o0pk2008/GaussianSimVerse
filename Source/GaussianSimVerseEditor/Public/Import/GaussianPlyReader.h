// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"

class GAUSSIANSIMVERSEEDITOR_API FGaussianPlyReader
{
public:
	static bool ReadFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError);
};
