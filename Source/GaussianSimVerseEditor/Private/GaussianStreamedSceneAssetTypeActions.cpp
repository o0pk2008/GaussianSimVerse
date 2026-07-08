// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianStreamedSceneAssetTypeActions.h"
#include "GaussianStreamedSceneAsset.h"
#include "AssetTypeCategories.h"

#define LOCTEXT_NAMESPACE "GaussianStreamedSceneAssetTypeActions"

FText FAssetTypeActions_GaussianStreamedSceneAsset::GetName() const
{
	return LOCTEXT("AssetName", "Gaussian Streamed Scene");
}

FColor FAssetTypeActions_GaussianStreamedSceneAsset::GetTypeColor() const
{
	return FColor(80, 200, 255);
}

UClass* FAssetTypeActions_GaussianStreamedSceneAsset::GetSupportedClass() const
{
	return UGaussianStreamedSceneAsset::StaticClass();
}

uint32 FAssetTypeActions_GaussianStreamedSceneAsset::GetCategories()
{
	return EAssetTypeCategories::Misc;
}

const TArray<FText>& FAssetTypeActions_GaussianStreamedSceneAsset::GetSubMenus() const
{
	static const TArray<FText> SubMenus;
	return SubMenus;
}

#undef LOCTEXT_NAMESPACE
