// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UGaussianAsset;
class UPackage;
class UTexture2D;

class GAUSSIANSIMVERSEEDITOR_API FGaussianSogTextureImporter
{
public:
	/** Import WebP companion files from an extracted SOG directory into the asset package. */
	static void ImportCompanionTextures(const FString& ExtractedDirectory, UGaussianAsset* Asset, UPackage* DestinationPackage);
};
