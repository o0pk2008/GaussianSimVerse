// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianTypes.h"

class GAUSSIANSIMVERSEEDITOR_API FGaussianSogReader
{
public:
	/** Load SOG from a directory containing meta.json and WebP images. */
	static bool ReadDirectory(
		const FString& DirectoryPath,
		TArray<FGaussianSplatData>& OutSplats,
		FString& OutError,
		TArray<float>* OutShCoefficients = nullptr,
		int32* OutImportedShDegree = nullptr);

	/** Load bundled .sog (ZIP) or directory; extracts archives to a temp folder when needed. */
	static bool ReadFile(
		const FString& FilePath,
		TArray<FGaussianSplatData>& OutSplats,
		FString& OutError,
		FString* OutExtractedDirectory = nullptr,
		TArray<float>* OutShCoefficients = nullptr,
		int32* OutImportedShDegree = nullptr);
};
