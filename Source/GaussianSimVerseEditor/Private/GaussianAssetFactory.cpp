// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianAssetFactory.h"
#include "GaussianImporter.h"
#include "GaussianAsset.h"
#include "Misc/Paths.h"

UGaussianAssetFactory::UGaussianAssetFactory()
{
	SupportedClass = UGaussianAsset::StaticClass();
	bCreateNew = false;
	bEditAfterNew = false;
	bEditorImport = true;

	Formats.Add(TEXT("ply;Gaussian PLY"));
	Formats.Add(TEXT("splat;Gaussian Splat"));
	Formats.Add(TEXT("sog;Gaussian SOG Archive"));
	Formats.Add(TEXT("json;Gaussian SOG Metadata"));
}

bool UGaussianAssetFactory::FactoryCanImport(const FString& Filename)
{
	if (!FGaussianImporter::CanImportFile(Filename))
	{
		return false;
	}

	const FString Extension = FPaths::GetExtension(Filename).ToLower();
	if (Extension == TEXT("json"))
	{
		return FPaths::GetCleanFilename(Filename).Equals(TEXT("meta.json"), ESearchCase::IgnoreCase);
	}

	return true;
}

UObject* UGaussianAssetFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	UGaussianAsset* Asset = NewObject<UGaussianAsset>(InParent, InClass, InName, Flags);
	if (!Asset)
	{
		return nullptr;
	}

	FString ImportError;
	if (!FGaussianImporter::ImportFile(Filename, Asset, ImportError, InParent->GetOutermost()))
	{
		if (Warn && !ImportError.IsEmpty())
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("%s"), *ImportError);
		}
		return nullptr;
	}

	// Defer GPU upload until PIE / scene actor registration — importing can be millions of splats.
	UE_LOG(LogTemp, Log, TEXT("Gaussian import complete: %d splats (%.1f MB payload)"),
		Asset->GaussianCount, Asset->GetPayloadSizeMB());

	return Asset;
}
