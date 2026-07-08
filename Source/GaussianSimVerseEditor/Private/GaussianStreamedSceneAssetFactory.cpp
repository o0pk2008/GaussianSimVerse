// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianStreamedSceneAssetFactory.h"
#include "GaussianStreamedSceneAsset.h"
#include "GaussianStreamedSceneActor.h"
#include "Streaming/GaussianLodMetaReader.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace
{
	void CreateStreamedSceneActorBlueprint(UGaussianStreamedSceneAsset* Asset, FFeedbackContext* Warn)
	{
		if (!Asset)
		{
			return;
		}

		const FString AssetPackageName = Asset->GetOutermost()->GetName();
		const FString AssetFolderPath = FPackageName::GetLongPackagePath(AssetPackageName);
		const FString BlueprintName = FString::Printf(TEXT("%s_StreamedSceneActor"), *Asset->GetName());
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
				Warn->Logf(ELogVerbosity::Warning, TEXT("Failed to create package for streamed scene actor blueprint: %s"), *BlueprintPackagePath);
			}
			return;
		}

		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AGaussianStreamedSceneActor::StaticClass(),
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

		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		if (AGaussianStreamedSceneActor* CDO = Cast<AGaussianStreamedSceneActor>(Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr))
		{
			CDO->Modify();
			CDO->StreamedSceneAsset = Asset;
			CDO->PostEditChange();
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		}

		Blueprint->MarkPackageDirty();
		BlueprintPackage->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Blueprint);
	}
}

UGaussianStreamedSceneAssetFactory::UGaussianStreamedSceneAssetFactory()
{
	SupportedClass = UGaussianStreamedSceneAsset::StaticClass();
	bCreateNew = false;
	bEditAfterNew = false;
	bEditorImport = true;
	Formats.Add(TEXT("json;Gaussian LOD Metadata"));
}

bool UGaussianStreamedSceneAssetFactory::FactoryCanImport(const FString& Filename)
{
	return FPaths::GetCleanFilename(Filename).Equals(TEXT("lod-meta.json"), ESearchCase::IgnoreCase);
}

UObject* UGaussianStreamedSceneAssetFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	UGaussianStreamedSceneAsset* Asset = NewObject<UGaussianStreamedSceneAsset>(InParent, InClass, InName, Flags);
	if (!Asset)
	{
		return nullptr;
	}

	FString ParseError;
	FGaussianLodTreeNode ImportedTree;
	if (!FGaussianLodMetaReader::ParseFile(Filename, Asset->LodMeta, ImportedTree, ParseError))
	{
		if (Warn && !ParseError.IsEmpty())
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("%s"), *ParseError);
		}
		return nullptr;
	}

	Asset->LodMetaPath = FPaths::ConvertRelativePathToFull(Filename);
	Asset->DatasetRoot = FPaths::GetPath(Asset->LodMetaPath);
	Asset->LodTree = MoveTemp(ImportedTree);
#if WITH_EDITORONLY_DATA
	Asset->ImportSourcePath = Filename;
#endif

	UE_LOG(LogTemp, Log, TEXT("Imported streamed Gaussian dataset: %d LOD levels, %d chunk files, root=%s"),
		Asset->LodMeta.LodLevels,
		Asset->LodMeta.Filenames.Num(),
		*Asset->DatasetRoot);

	CreateStreamedSceneActorBlueprint(Asset, Warn);
	return Asset;
}
