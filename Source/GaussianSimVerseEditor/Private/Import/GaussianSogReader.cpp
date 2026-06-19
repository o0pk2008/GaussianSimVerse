// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianSogReader.h"
#include "Import/GaussianImportUtils.h"
#include "Import/GaussianWebPLoader.h"
#include "Import/GaussianZipUtil.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace GaussianSogPrivate
{
	static bool LoadJson(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			OutError = FString::Printf(TEXT("Failed to read meta.json: %s"), *FilePath);
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("Failed to parse meta.json: %s"), *FilePath);
			return false;
		}

		return true;
	}

	static bool LoadCodebook(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, TArray<float>& OutCodebook, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			OutError = FString::Printf(TEXT("meta.json missing %s codebook"), *FieldName);
			return false;
		}

		OutCodebook.Reset();
		OutCodebook.Reserve(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			OutCodebook.Add(static_cast<float>(Value->AsNumber()));
		}
		return true;
	}

	static FString ResolveImagePath(const FString& Directory, const TArray<TSharedPtr<FJsonValue>>* Files, int32 Index, FString& OutError)
	{
		if (!Files || !Files->IsValidIndex(Index))
		{
			OutError = TEXT("meta.json image file list is invalid");
			return FString();
		}

		const FString RelativeName = (*Files)[Index]->AsString();
		return FPaths::Combine(Directory, RelativeName);
	}

	static FVector4f DecodeSogQuaternion(uint8 R, uint8 G, uint8 B, uint8 A, FString& OutError)
	{
		const int32 Mode = static_cast<int32>(A) - 252;
		if (Mode < 0 || Mode > 3)
		{
			OutError = TEXT("Invalid SOG quaternion mode byte");
			return FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
		}

		const auto ToComp = [](uint8 C) -> float
		{
			return (static_cast<float>(C) / 255.0f - 0.5f) * 2.0f / GaussianImport::Sqrt2;
		};

		const float CompA = ToComp(R);
		const float CompB = ToComp(G);
		const float CompC = ToComp(B);
		const float T = CompA * CompA + CompB * CompB + CompC * CompC;
		const float D = FMath::Sqrt(FMath::Max(0.0f, 1.0f - T));

		float W = 0.0f;
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;

		switch (Mode)
		{
		case 0: W = D; X = CompA; Y = CompB; Z = CompC; break;
		case 1: W = CompA; X = D; Y = CompB; Z = CompC; break;
		case 2: W = CompA; X = CompB; Y = D; Z = CompC; break;
		case 3: W = CompA; X = CompB; Y = CompC; Z = D; break;
		default: break;
		}

		return GaussianImport::PlayCanvasToUERotation(W, X, Y, Z);
	}
}

bool FGaussianSogReader::ReadDirectory(const FString& DirectoryPath, TArray<FGaussianSplatData>& OutSplats, FString& OutError)
{
	const FString MetaPath = FPaths::Combine(DirectoryPath, TEXT("meta.json"));
	TSharedPtr<FJsonObject> MetaObject;
	if (!GaussianSogPrivate::LoadJson(MetaPath, MetaObject, OutError))
	{
		return false;
	}

	int32 Version = 0;
	if (!MetaObject->TryGetNumberField(TEXT("version"), Version) || Version != 2)
	{
		OutError = TEXT("Unsupported SOG version (expected version 2)");
		return false;
	}

	int32 Count = 0;
	if (!MetaObject->TryGetNumberField(TEXT("count"), Count) || Count <= 0)
	{
		OutError = TEXT("SOG meta.json has invalid count");
		return false;
	}

	const TSharedPtr<FJsonObject>* MeansObject = nullptr;
	const TSharedPtr<FJsonObject>* ScalesObject = nullptr;
	const TSharedPtr<FJsonObject>* QuatsObject = nullptr;
	const TSharedPtr<FJsonObject>* Sh0Object = nullptr;
	if (!MetaObject->TryGetObjectField(TEXT("means"), MeansObject)
		|| !MetaObject->TryGetObjectField(TEXT("scales"), ScalesObject)
		|| !MetaObject->TryGetObjectField(TEXT("quats"), QuatsObject)
		|| !MetaObject->TryGetObjectField(TEXT("sh0"), Sh0Object))
	{
		OutError = TEXT("SOG meta.json missing required sections");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* MeansMins = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* MeansMaxs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* MeansFiles = nullptr;
	(*MeansObject)->TryGetArrayField(TEXT("mins"), MeansMins);
	(*MeansObject)->TryGetArrayField(TEXT("maxs"), MeansMaxs);
	(*MeansObject)->TryGetArrayField(TEXT("files"), MeansFiles);
	if (!MeansMins || MeansMins->Num() != 3 || !MeansMaxs || MeansMaxs->Num() != 3 || !MeansFiles || MeansFiles->Num() < 2)
	{
		OutError = TEXT("SOG means section is invalid");
		return false;
	}

	const FVector3f MeansMin(
		static_cast<float>((*MeansMins)[0]->AsNumber()),
		static_cast<float>((*MeansMins)[1]->AsNumber()),
		static_cast<float>((*MeansMins)[2]->AsNumber()));
	const FVector3f MeansMax(
		static_cast<float>((*MeansMaxs)[0]->AsNumber()),
		static_cast<float>((*MeansMaxs)[1]->AsNumber()),
		static_cast<float>((*MeansMaxs)[2]->AsNumber()));

	const TArray<TSharedPtr<FJsonValue>>* ScalesFiles = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* QuatsFiles = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Sh0Files = nullptr;
	(*ScalesObject)->TryGetArrayField(TEXT("files"), ScalesFiles);
	(*QuatsObject)->TryGetArrayField(TEXT("files"), QuatsFiles);
	(*Sh0Object)->TryGetArrayField(TEXT("files"), Sh0Files);
	if (!ScalesFiles || ScalesFiles->Num() < 1 || !QuatsFiles || QuatsFiles->Num() < 1 || !Sh0Files || Sh0Files->Num() < 1)
	{
		OutError = TEXT("SOG property file lists are invalid");
		return false;
	}

	TArray<float> ScalesCodebook;
	TArray<float> Sh0Codebook;
	if (!GaussianSogPrivate::LoadCodebook(*ScalesObject, TEXT("codebook"), ScalesCodebook, OutError))
	{
		return false;
	}
	if (!GaussianSogPrivate::LoadCodebook(*Sh0Object, TEXT("codebook"), Sh0Codebook, OutError))
	{
		return false;
	}

	const FString MeansLPath = GaussianSogPrivate::ResolveImagePath(DirectoryPath, MeansFiles, 0, OutError);
	const FString MeansUPath = GaussianSogPrivate::ResolveImagePath(DirectoryPath, MeansFiles, 1, OutError);
	const FString ScalesPath = GaussianSogPrivate::ResolveImagePath(DirectoryPath, ScalesFiles, 0, OutError);
	const FString QuatsPath = GaussianSogPrivate::ResolveImagePath(DirectoryPath, QuatsFiles, 0, OutError);
	const FString Sh0Path = GaussianSogPrivate::ResolveImagePath(DirectoryPath, Sh0Files, 0, OutError);

	FGaussianImageRGBA8 MeansL;
	FGaussianImageRGBA8 MeansU;
	FGaussianImageRGBA8 ScalesImage;
	FGaussianImageRGBA8 QuatsImage;
	FGaussianImageRGBA8 Sh0Image;

	if (!FGaussianWebPLoader::LoadFile(MeansLPath, MeansL, OutError)
		|| !FGaussianWebPLoader::LoadFile(MeansUPath, MeansU, OutError)
		|| !FGaussianWebPLoader::LoadFile(ScalesPath, ScalesImage, OutError)
		|| !FGaussianWebPLoader::LoadFile(QuatsPath, QuatsImage, OutError)
		|| !FGaussianWebPLoader::LoadFile(Sh0Path, Sh0Image, OutError))
	{
		return false;
	}

	const int32 Width = MeansL.Width;
	const int32 Height = MeansL.Height;
	if (MeansU.Width != Width || MeansU.Height != Height
		|| ScalesImage.Width != Width || ScalesImage.Height != Height
		|| QuatsImage.Width != Width || QuatsImage.Height != Height
		|| Sh0Image.Width != Width || Sh0Image.Height != Height)
	{
		OutError = TEXT("SOG WebP images have mismatched dimensions");
		return false;
	}

	const int32 Capacity = Width * Height;
	if (Count > Capacity)
	{
		OutError = FString::Printf(TEXT("SOG count (%d) exceeds image capacity (%d)"), Count, Capacity);
		return false;
	}

	OutSplats.Reset();
	OutSplats.Reserve(Count);

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 X = Index % Width;
		const int32 Y = Index / Width;

		uint8 MeansLR = 0, MeansLG = 0, MeansLB = 0, MeansLA = 0;
		uint8 MeansUR = 0, MeansUG = 0, MeansUB = 0, MeansUA = 0;
		uint8 ScalesR = 0, ScalesG = 0, ScalesB = 0, ScalesA = 0;
		uint8 QuatsR = 0, QuatsG = 0, QuatsB = 0, QuatsA = 0;
		uint8 Sh0R = 0, Sh0G = 0, Sh0B = 0, Sh0A = 0;

		MeansL.GetPixel(X, Y, MeansLR, MeansLG, MeansLB, MeansLA);
		MeansU.GetPixel(X, Y, MeansUR, MeansUG, MeansUB, MeansUA);
		ScalesImage.GetPixel(X, Y, ScalesR, ScalesG, ScalesB, ScalesA);
		QuatsImage.GetPixel(X, Y, QuatsR, QuatsG, QuatsB, QuatsA);
		Sh0Image.GetPixel(X, Y, Sh0R, Sh0G, Sh0B, Sh0A);

		const int32 QX = (static_cast<int32>(MeansUR) << 8) | MeansLR;
		const int32 QY = (static_cast<int32>(MeansUG) << 8) | MeansLG;
		const int32 QZ = (static_cast<int32>(MeansUB) << 8) | MeansLB;

		const FVector3f Norm(
			static_cast<float>(QX) / 65535.0f,
			static_cast<float>(QY) / 65535.0f,
			static_cast<float>(QZ) / 65535.0f);

		const FVector3f LogPos = GaussianImport::LerpVec3(MeansMin, MeansMax, Norm);
		const FVector3f PlayCanvasPos(
			GaussianImport::UnlogPosition(LogPos.X),
			GaussianImport::UnlogPosition(LogPos.Y),
			GaussianImport::UnlogPosition(LogPos.Z));

		const float ScaleX = ScalesCodebook.IsValidIndex(ScalesR) ? FMath::Exp(ScalesCodebook[ScalesR]) : 0.01f;
		const float ScaleY = ScalesCodebook.IsValidIndex(ScalesG) ? FMath::Exp(ScalesCodebook[ScalesG]) : 0.01f;
		const float ScaleZ = ScalesCodebook.IsValidIndex(ScalesB) ? FMath::Exp(ScalesCodebook[ScalesB]) : 0.01f;

		const float Fdc0 = Sh0Codebook.IsValidIndex(Sh0R) ? Sh0Codebook[Sh0R] : 0.0f;
		const float Fdc1 = Sh0Codebook.IsValidIndex(Sh0G) ? Sh0Codebook[Sh0G] : 0.0f;
		const float Fdc2 = Sh0Codebook.IsValidIndex(Sh0B) ? Sh0Codebook[Sh0B] : 0.0f;
		const float Opacity = Sh0A / 255.0f;

		FGaussianSplatData Splat;
		Splat.Position = GaussianImport::PlayCanvasToUEPosition(PlayCanvasPos);
		Splat.Scale = GaussianImport::MetersToUEScale(FVector3f(ScaleX, ScaleY, ScaleZ));
		Splat.Rotation = GaussianSogPrivate::DecodeSogQuaternion(QuatsR, QuatsG, QuatsB, QuatsA, OutError);
		Splat.Color = GaussianImport::SH0ToLinearColor(Fdc0, Fdc1, Fdc2, Opacity);
		OutSplats.Add(Splat);
	}

	return OutSplats.Num() > 0;
}

bool FGaussianSogReader::ReadFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError, FString* OutExtractedDirectory)
{
	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	const FString AbsolutePath = FPaths::ConvertRelativePathToFull(FilePath);

	if (Extension == TEXT("json") && FPaths::GetCleanFilename(FilePath).Equals(TEXT("meta.json"), ESearchCase::IgnoreCase))
	{
		if (OutExtractedDirectory)
		{
			*OutExtractedDirectory = FPaths::GetPath(AbsolutePath);
		}
		return ReadDirectory(FPaths::GetPath(AbsolutePath), OutSplats, OutError);
	}

	if (Extension == TEXT("sog"))
	{
		FString ExtractedDir;
		if (!FGaussianZipUtil::ExtractArchive(AbsolutePath, ExtractedDir, OutError))
		{
			return false;
		}
		if (OutExtractedDirectory)
		{
			*OutExtractedDirectory = ExtractedDir;
		}
		return ReadDirectory(ExtractedDir, OutSplats, OutError);
	}

	if (IFileManager::Get().DirectoryExists(*AbsolutePath))
	{
		if (OutExtractedDirectory)
		{
			*OutExtractedDirectory = AbsolutePath;
		}
		return ReadDirectory(AbsolutePath, OutSplats, OutError);
	}

	OutError = FString::Printf(TEXT("Unsupported SOG input: %s"), *FilePath);
	return false;
}
