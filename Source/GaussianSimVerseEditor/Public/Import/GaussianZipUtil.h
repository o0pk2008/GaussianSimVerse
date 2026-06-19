// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class GAUSSIANSIMVERSEEDITOR_API FGaussianZipUtil
{
public:
	/** Extract a .sog/.zip archive to OutExtractedDir (created if needed). */
	static bool ExtractArchive(const FString& ArchivePath, FString& OutExtractedDir, FString& OutError);
};
