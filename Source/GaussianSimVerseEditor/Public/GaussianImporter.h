// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UGaussianAsset;
class UPackage;

/**
 * Imports .ply / .splat / .sog (and meta.json SOG folders) into UGaussianAsset.
 */
class GAUSSIANSIMVERSEEDITOR_API FGaussianImporter
{
public:
	static bool CanImportFile(const FString& FilePath);
	static bool ImportFile(
		const FString& FilePath,
		UGaussianAsset* OutAsset,
		FString& OutError,
		UPackage* DestinationPackage = nullptr);
};
