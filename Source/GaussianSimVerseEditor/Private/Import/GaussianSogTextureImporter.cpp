// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianSogTextureImporter.h"
#include "Import/GaussianWebPLoader.h"
#include "GaussianAsset.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"

namespace GaussianSogTextureImporterPrivate
{
	static void FindWebpFiles(const FString& Directory, TArray<FString>& OutFiles)
	{
		IFileManager::Get().FindFiles(OutFiles, *Directory, TEXT("webp"));

		if (OutFiles.Num() == 0)
		{
			IFileManager::Get().FindFilesRecursive(OutFiles, *Directory, TEXT("*.webp"), true, false);
		}
	}

	static UTexture2D* CreateTextureAsset(
		const FGaussianImageRGBA8& Image,
		const FString& PackageName,
		const FString& AssetName,
		FString& OutError)
	{
		if (!Image.IsValid())
		{
			OutError = TEXT("Invalid decoded WebP image");
			return nullptr;
		}

		UPackage* TexturePackage = CreatePackage(*PackageName);
		if (!TexturePackage)
		{
			OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
			return nullptr;
		}

		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Image.Width * Image.Height);
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			const int32 ByteIndex = Index * 4;
			Pixels[Index] = FColor(
				Image.Pixels[ByteIndex + 0],
				Image.Pixels[ByteIndex + 1],
				Image.Pixels[ByteIndex + 2],
				Image.Pixels[ByteIndex + 3]);
		}

		FCreateTexture2DParameters Params;
		Params.bUseAlpha = true;
		Params.bSRGB = false;
		Params.CompressionSettings = TC_VectorDisplacementmap;
		Params.bDeferCompression = true;
		Params.MipGenSettings = TMGS_NoMipmaps;

		UTexture2D* Texture = FImageUtils::CreateTexture2D(
			Image.Width,
			Image.Height,
			Pixels,
			TexturePackage,
			*AssetName,
			RF_Public | RF_Standalone,
			Params);

		if (!Texture)
		{
			OutError = FString::Printf(TEXT("Failed to create texture asset: %s"), *AssetName);
			return nullptr;
		}

		Texture->NeverStream = true;
		TexturePackage->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Texture);

		return Texture;
	}
}

void FGaussianSogTextureImporter::ImportCompanionTextures(
	const FString& ExtractedDirectory,
	UGaussianAsset* Asset,
	UPackage* DestinationPackage)
{
	if (!Asset || !DestinationPackage || ExtractedDirectory.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("SOG texture import skipped: invalid inputs (Dir=%s Asset=%s)"),
			*ExtractedDirectory, Asset ? *Asset->GetName() : TEXT("null"));
		return;
	}

	TArray<FString> WebpFiles;
	GaussianSogTextureImporterPrivate::FindWebpFiles(ExtractedDirectory, WebpFiles);

	if (WebpFiles.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SOG texture import found no WebP files in %s"), *ExtractedDirectory);
		return;
	}

	WebpFiles.Sort();

	FScopedSlowTask SlowTask(static_cast<float>(WebpFiles.Num()), NSLOCTEXT("GaussianSimVerse", "ImportSogTextures", "Importing SOG WebP textures..."));
	SlowTask.MakeDialog();

	TArray<UTexture2D*> ImportedTextures;
	ImportedTextures.Reserve(WebpFiles.Num());

	const FString PackagePath = FPackageName::GetLongPackagePath(DestinationPackage->GetName());
	const FString AssetBaseName = Asset->GetName();

	for (const FString& WebpEntry : WebpFiles)
	{
		const FString SourcePath = FPaths::IsRelative(WebpEntry)
			? FPaths::Combine(ExtractedDirectory, WebpEntry)
			: WebpEntry;

		const FString WebpBaseName = FPaths::GetBaseFilename(SourcePath);
		const FString TextureAssetName = FString::Printf(TEXT("%s_%s"), *AssetBaseName, *WebpBaseName);
		const FString TexturePackageName = PackagePath + TEXT("/") + TextureAssetName;

		SlowTask.EnterProgressFrame(1.0f, FText::FromString(TextureAssetName));

		FGaussianImageRGBA8 Image;
		FString LoadError;
		if (!FGaussianWebPLoader::LoadFile(SourcePath, Image, LoadError))
		{
			UE_LOG(LogTemp, Warning, TEXT("Failed to load SOG WebP %s: %s"), *SourcePath, *LoadError);
			continue;
		}

		FString CreateError;
		UTexture2D* Texture = GaussianSogTextureImporterPrivate::CreateTextureAsset(
			Image, TexturePackageName, TextureAssetName, CreateError);

		if (!Texture)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s"), *CreateError);
			continue;
		}

		ImportedTextures.Add(Texture);
	}

	Asset->SetSourceTextures(ImportedTextures);
	DestinationPackage->MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("Imported %d SOG WebP texture assets beside %s"), ImportedTextures.Num(), *Asset->GetName());
}
