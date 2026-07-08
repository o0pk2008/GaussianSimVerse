// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianStreamedSceneAsset.h"
#include "Streaming/GaussianLodMetaReader.h"
#include "Misc/Paths.h"

bool UGaussianStreamedSceneAsset::EnsureLodTreeLoaded(FString& OutError) const
{
	UGaussianStreamedSceneAsset* MutableThis = const_cast<UGaussianStreamedSceneAsset*>(this);
	if (LodTree.IsLeaf() || LodTree.Children.Num() > 0)
	{
		return true;
	}

	if (LodMetaPath.IsEmpty())
	{
		OutError = TEXT("Streamed scene asset has no lod-meta.json path");
		return false;
	}

	FGaussianLodMetaData ParsedMeta = LodMeta;
	if (!FGaussianLodMetaReader::ParseFile(LodMetaPath, ParsedMeta, MutableThis->LodTree, OutError))
	{
		return false;
	}

	MutableThis->LodMeta = ParsedMeta;
	return true;
}

bool UGaussianStreamedSceneAsset::ResolveChunkDirectory(int32 FileIndex, FString& OutDirectory) const
{
	if (!LodMeta.Filenames.IsValidIndex(FileIndex))
	{
		return false;
	}

	const FString RelativeMeta = LodMeta.Filenames[FileIndex];
	OutDirectory = FPaths::Combine(DatasetRoot, FPaths::GetPath(RelativeMeta));
	return IFileManager::Get().DirectoryExists(*OutDirectory);
}

bool UGaussianStreamedSceneAsset::ResolveEnvironmentDirectory(FString& OutDirectory) const
{
	if (LodMeta.EnvironmentRelativePath.IsEmpty())
	{
		return false;
	}

	OutDirectory = FPaths::Combine(DatasetRoot, FPaths::GetPath(LodMeta.EnvironmentRelativePath));
	return IFileManager::Get().DirectoryExists(*OutDirectory);
}
