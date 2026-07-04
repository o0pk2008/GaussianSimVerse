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
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FGaussianSimVerseEditorModule"

static TSharedPtr<FAssetTypeActions_GaussianAsset> GaussianAssetTypeActions;

namespace
{
	struct FGaussianImportStats
	{
		FVector3f MinScale = FVector3f(FLT_MAX, FLT_MAX, FLT_MAX);
		FVector3f MaxScale = FVector3f(-FLT_MAX, -FLT_MAX, -FLT_MAX);
		float MinAlpha = FLT_MAX;
		float MaxAlpha = -FLT_MAX;
		int32 LowAlphaCount = 0;
	};

	FGaussianImportStats BuildImportStats(const TArray<FGaussianSplatData>& Splats)
	{
		FGaussianImportStats Stats;
		for (const FGaussianSplatData& Splat : Splats)
		{
			Stats.MinScale.X = FMath::Min(Stats.MinScale.X, Splat.Scale.X);
			Stats.MinScale.Y = FMath::Min(Stats.MinScale.Y, Splat.Scale.Y);
			Stats.MinScale.Z = FMath::Min(Stats.MinScale.Z, Splat.Scale.Z);
			Stats.MaxScale.X = FMath::Max(Stats.MaxScale.X, Splat.Scale.X);
			Stats.MaxScale.Y = FMath::Max(Stats.MaxScale.Y, Splat.Scale.Y);
			Stats.MaxScale.Z = FMath::Max(Stats.MaxScale.Z, Splat.Scale.Z);
			Stats.MinAlpha = FMath::Min(Stats.MinAlpha, Splat.Color.W);
			Stats.MaxAlpha = FMath::Max(Stats.MaxAlpha, Splat.Color.W);
			if (Splat.Color.W <= (1.0f / 255.0f))
			{
				++Stats.LowAlphaCount;
			}
		}

		if (Splats.Num() == 0)
		{
			Stats.MinScale = FVector3f::ZeroVector;
			Stats.MaxScale = FVector3f::ZeroVector;
			Stats.MinAlpha = 0.0f;
			Stats.MaxAlpha = 0.0f;
		}

		return Stats;
	}
}

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
	TArray<float> ShCoefficients;
	int32 ImportedShDegree = 0;
	bool bSuccess = false;
	FString ExtractedDirectory;

	if (Extension == TEXT("ply"))
	{
		bSuccess = FGaussianPlyReader::ReadFile(FilePath, Splats, OutError, &ShCoefficients, &ImportedShDegree);
		OutAsset->SourceFormat = EGaussianSourceFormat::PLY;
	}
	else if (Extension == TEXT("splat"))
	{
		bSuccess = FGaussianSplatReader::ReadFile(FilePath, Splats, OutError);
		OutAsset->SourceFormat = EGaussianSourceFormat::Splat;
	}
	else if (Extension == TEXT("sog") || Extension == TEXT("json"))
	{
		bSuccess = FGaussianSogReader::ReadFile(
			FilePath, Splats, OutError, &ExtractedDirectory, &ShCoefficients, &ImportedShDegree);
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
	const int32 ShCoefficientCount = ShCoefficients.Num();
	OutAsset->SetStagingData(Splats, MoveTemp(ShCoefficients), ImportedShDegree);

	const FGaussianImportStats ImportStats = BuildImportStats(Splats);

	if (OutAsset->SourceFormat == EGaussianSourceFormat::SOG && !ExtractedDirectory.IsEmpty())
	{
		FGaussianSogTextureImporter::ImportCompanionTextures(
			ExtractedDirectory,
			OutAsset,
			DestinationPackage ? DestinationPackage : OutAsset->GetOutermost());
	}

	UE_LOG(
		LogTemp,
		Log,
		TEXT("Imported %d Gaussians from %s | Format=%d | SH degree=%d (coeffs=%d) | Bounds Origin=(%.2f, %.2f, %.2f) Extent=(%.2f, %.2f, %.2f) | Alpha=[%.4f, %.4f] LowAlpha=%d | ScaleMin=(%.4f, %.4f, %.4f) ScaleMax=(%.4f, %.4f, %.4f)"),
		Splats.Num(),
		*FilePath,
		static_cast<int32>(OutAsset->SourceFormat),
		OutAsset->ImportedShDegree,
		ShCoefficientCount,
		OutAsset->Bounds.Origin.X, OutAsset->Bounds.Origin.Y, OutAsset->Bounds.Origin.Z,
		OutAsset->Bounds.Extent.X, OutAsset->Bounds.Extent.Y, OutAsset->Bounds.Extent.Z,
		ImportStats.MinAlpha, ImportStats.MaxAlpha, ImportStats.LowAlphaCount,
		ImportStats.MinScale.X, ImportStats.MinScale.Y, ImportStats.MinScale.Z,
		ImportStats.MaxScale.X, ImportStats.MaxScale.Y, ImportStats.MaxScale.Z);
	return true;
}
