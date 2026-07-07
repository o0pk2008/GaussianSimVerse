// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianAssetFactory.h"
#include "GaussianImporter.h"
#include "GaussianAsset.h"
#include "GaussianSceneActor.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace
{
	void CreateGaussianSceneActorBlueprintAsset(UGaussianAsset* Asset, FFeedbackContext* Warn)
	{
		if (!Asset)
		{
			return;
		}

		const FString AssetPackageName = Asset->GetOutermost()->GetName();
		const FString AssetFolderPath = FPackageName::GetLongPackagePath(AssetPackageName);
		const FString BlueprintName = FString::Printf(TEXT("%s_SceneActor"), *Asset->GetName());
		const FString BlueprintPackagePath = AssetFolderPath / BlueprintName;

		if (FindObject<UBlueprint>(nullptr, *BlueprintPackagePath) != nullptr)
		{
			return;
		}

		UPackage* BlueprintPackage = CreatePackage(*BlueprintPackagePath);
		if (!BlueprintPackage)
		{
			if (Warn)
			{
				Warn->Logf(ELogVerbosity::Warning, TEXT("Failed to create package for GaussianSceneActor blueprint: %s"), *BlueprintPackagePath);
			}
			return;
		}

		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AGaussianSceneActor::StaticClass(),
			BlueprintPackage,
			*BlueprintName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!Blueprint)
		{
			return;
		}

		// Ensure generated class/CDO exist before writing defaults.
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		if (AGaussianSceneActor* CDO = Cast<AGaussianSceneActor>(Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr))
		{
			CDO->Modify();
			CDO->GaussianAsset = Asset;
			CDO->PostEditChange();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Blueprint->MarkPackageDirty();
		BlueprintPackage->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Blueprint);
	}
}

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
	CreateGaussianSceneActorBlueprintAsset(Asset, Warn);

	return Asset;
}
