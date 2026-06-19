// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSimVerseEditor.h"
#include "GaussianImporter.h"
#include "GaussianAsset.h"
#include "GaussianAssetTypeActions.h"
#include "Import/GaussianPlyReader.h"
#include "Import/GaussianSplatReader.h"
#include "Import/GaussianSogReader.h"
#include "Import/GaussianSogTextureImporter.h"
#include "Import/GaussianImportUtils.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FGaussianSimVerseEditorModule"

static TSharedPtr<FAssetTypeActions_GaussianAsset> GaussianAssetTypeActions;

void FGaussianSimVerseEditorModule::StartupModule()
{
	GaussianAssetTypeActions = MakeShared<FAssetTypeActions_GaussianAsset>();
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	AssetTools.RegisterAssetTypeActions(GaussianAssetTypeActions.ToSharedRef());

	UE_LOG(LogTemp, Log, TEXT("GaussianSimVerseEditor module started"));
}

void FGaussianSimVerseEditorModule::ShutdownModule()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetTools")) && GaussianAssetTypeActions.IsValid())
	{
		IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		AssetTools.UnregisterAssetTypeActions(GaussianAssetTypeActions.ToSharedRef());
		GaussianAssetTypeActions.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSimVerseEditorModule, GaussianSimVerseEditor)

// ---------------------------------------------------------------------------
// FGaussianImporter
// ---------------------------------------------------------------------------

bool FGaussianImporter::CanImportFile(const FString& FilePath)
{
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	if (Extension == TEXT("ply") || Extension == TEXT("splat") || Extension == TEXT("sog"))
	{
		return true;
	}

	if (Extension == TEXT("json"))
	{
		return FPaths::GetCleanFilename(FilePath).Equals(TEXT("meta.json"), ESearchCase::IgnoreCase);
	}

	return false;
}

bool FGaussianImporter::ImportFile(const FString& FilePath, UGaussianAsset* OutAsset, FString& OutError, UPackage* DestinationPackage)
{
	if (!OutAsset || !CanImportFile(FilePath))
	{
		OutError = TEXT("Invalid import file or asset");
		return false;
	}

	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	TArray<FGaussianSplatData> Splats;
	bool bSuccess = false;
	FString ExtractedDirectory;

	if (Extension == TEXT("ply"))
	{
		bSuccess = FGaussianPlyReader::ReadFile(FilePath, Splats, OutError);
		OutAsset->SourceFormat = EGaussianSourceFormat::PLY;
	}
	else if (Extension == TEXT("splat"))
	{
		bSuccess = FGaussianSplatReader::ReadFile(FilePath, Splats, OutError);
		OutAsset->SourceFormat = EGaussianSourceFormat::Splat;
	}
	else if (Extension == TEXT("sog") || Extension == TEXT("json"))
	{
		bSuccess = FGaussianSogReader::ReadFile(FilePath, Splats, OutError, &ExtractedDirectory);
		OutAsset->SourceFormat = EGaussianSourceFormat::SOG;
	}
	else
	{
		OutError = FString::Printf(TEXT("Unsupported extension: %s"), *Extension);
		return false;
	}

	if (!bSuccess)
	{
		return false;
	}

	OutAsset->ImportSourcePath = FilePath;
	OutAsset->SetStagingData(Splats);

	if (OutAsset->SourceFormat == EGaussianSourceFormat::SOG && !ExtractedDirectory.IsEmpty())
	{
		FGaussianSogTextureImporter::ImportCompanionTextures(
			ExtractedDirectory,
			OutAsset,
			DestinationPackage ? DestinationPackage : OutAsset->GetOutermost());
	}

	UE_LOG(LogTemp, Log, TEXT("Imported %d Gaussians from %s (PlayCanvas Y-up -> UE Z-up conversion applied)"), Splats.Num(), *FilePath);
	return true;
}
