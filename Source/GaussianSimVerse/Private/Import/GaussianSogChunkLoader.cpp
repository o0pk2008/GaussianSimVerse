// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianSogChunkLoader.h"
#include "Import/GaussianImportUtils.h"
#include "Import/GaussianWebPLoader.h"
#include "GaussianSimVerse.h"
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

bool FGaussianSogChunkLoader::LoadDirectory(
	const FString& DirectoryPath,
	TArray<FGaussianSplatData>& OutSplats,
	FString& OutError,
	TArray<float>* OutShCoefficients,
	int32* OutImportedShDegree,
	const FLoadRange* Range)
{
	if (OutShCoefficients)
	{
		OutShCoefficients->Reset();
	}
	if (OutImportedShDegree)
	{
		*OutImportedShDegree = 0;
	}

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

	// Optional higher-order SH (shN): labels -> centroids palette -> codebook.
	const TSharedPtr<FJsonObject>* ShNObject = nullptr;
	FGaussianImageRGBA8 ShNCentroids;
	FGaussianImageRGBA8 ShNLabels;
	TArray<float> ShNCodebook;
	int32 ShNBands = 0;
	int32 ShNPaletteCount = 0;
	int32 ShNRestCoeffs = 0;
	bool bHasShN = false;

	if (MetaObject->TryGetObjectField(TEXT("shN"), ShNObject) && ShNObject && (*ShNObject).IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ShNFiles = nullptr;
		(*ShNObject)->TryGetArrayField(TEXT("files"), ShNFiles);
		(*ShNObject)->TryGetNumberField(TEXT("bands"), ShNBands);
		(*ShNObject)->TryGetNumberField(TEXT("count"), ShNPaletteCount);

		static const int32 RestCoeffsByBand[4] = { 0, 3, 8, 15 };
		ShNRestCoeffs = (ShNBands >= 0 && ShNBands <= 3) ? RestCoeffsByBand[ShNBands] : 0;

		FString ShNError;
		if (ShNRestCoeffs > 0
			&& ShNPaletteCount > 0
			&& ShNFiles
			&& ShNFiles->Num() >= 2
			&& GaussianSogPrivate::LoadCodebook(*ShNObject, TEXT("codebook"), ShNCodebook, ShNError))
		{
			const FString CentroidsPath = GaussianSogPrivate::ResolveImagePath(DirectoryPath, ShNFiles, 0, ShNError);
			const FString LabelsPath = GaussianSogPrivate::ResolveImagePath(DirectoryPath, ShNFiles, 1, ShNError);
			if (!CentroidsPath.IsEmpty()
				&& !LabelsPath.IsEmpty()
				&& FGaussianWebPLoader::LoadFile(CentroidsPath, ShNCentroids, ShNError)
				&& FGaussianWebPLoader::LoadFile(LabelsPath, ShNLabels, ShNError))
			{
				const int32 ExpectedCentroidWidth = 64 * ShNRestCoeffs;
				if (ShNLabels.Width * ShNLabels.Height < Count)
				{
					UE_LOG(LogTemp, Warning, TEXT("SOG shN labels texture too small; importing without higher-order SH"));
				}
				else if (ShNCentroids.Width != ExpectedCentroidWidth)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("SOG shN centroids width %d != expected %d (bands=%d); importing without higher-order SH"),
						ShNCentroids.Width, ExpectedCentroidWidth, ShNBands);
				}
				else
				{
					bHasShN = true;
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("SOG shN present but failed to load (%s); importing SH0 only"), *ShNError);
			}
		}
		else if (ShNRestCoeffs > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("SOG shN present but incomplete (%s); importing SH0 only"), *ShNError);
		}
	}

	if (OutImportedShDegree)
	{
		*OutImportedShDegree = bHasShN ? ShNBands : 0;
	}
	const int32 StartIndex = Range ? FMath::Clamp(Range->Offset, 0, FMath::Max(0, Count - 1)) : 0;
	const int32 RequestedCount = (Range && Range->Count > 0) ? Range->Count : (Count - StartIndex);
	const int32 EndIndex = FMath::Min(Count, StartIndex + RequestedCount);
	const int32 SubsetCount = FMath::Max(0, EndIndex - StartIndex);

	if (OutShCoefficients)
	{
		OutShCoefficients->Reset();
		if (bHasShN && SubsetCount > 0)
		{
			OutShCoefficients->SetNumZeroed(SubsetCount * GaussianShCoefficientsPerSplat);
		}
	}

	OutSplats.Reset();
	OutSplats.Reserve(SubsetCount);

	int32 OutIndex = 0;
	for (int32 Index = StartIndex; Index < EndIndex; ++Index)
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

		if (bHasShN && OutShCoefficients)
		{
			const int32 ShBase = OutIndex * GaussianShCoefficientsPerSplat;
			(*OutShCoefficients)[ShBase + 0] = Fdc0;
			(*OutShCoefficients)[ShBase + 1] = Fdc1;
			(*OutShCoefficients)[ShBase + 2] = Fdc2;

			const int32 LabelX = Index % ShNLabels.Width;
			const int32 LabelY = Index / ShNLabels.Width;
			uint8 LabelR = 0, LabelG = 0, LabelB = 0, LabelA = 0;
			ShNLabels.GetPixel(LabelX, LabelY, LabelR, LabelG, LabelB, LabelA);
			const int32 PaletteIndex = static_cast<int32>(LabelR) | (static_cast<int32>(LabelG) << 8);
			if (PaletteIndex >= 0 && PaletteIndex < ShNPaletteCount)
			{
				const int32 CentroidCol = PaletteIndex % 64;
				const int32 CentroidRow = PaletteIndex / 64;
				for (int32 Coeff = 0; Coeff < ShNRestCoeffs; ++Coeff)
				{
					const int32 Cx = CentroidCol * ShNRestCoeffs + Coeff;
					const int32 Cy = CentroidRow;
					if (Cx < 0 || Cy < 0 || Cx >= ShNCentroids.Width || Cy >= ShNCentroids.Height)
					{
						continue;
					}

					uint8 Cr = 0, Cg = 0, Cb = 0, Ca = 0;
					ShNCentroids.GetPixel(Cx, Cy, Cr, Cg, Cb, Ca);
					const float RestR = ShNCodebook.IsValidIndex(Cr) ? ShNCodebook[Cr] : 0.0f;
					const float RestG = ShNCodebook.IsValidIndex(Cg) ? ShNCodebook[Cg] : 0.0f;
					const float RestB = ShNCodebook.IsValidIndex(Cb) ? ShNCodebook[Cb] : 0.0f;
					// Interleaved RGB per rest coefficient (matches PLY / shader layout).
					(*OutShCoefficients)[ShBase + 3 + Coeff * 3 + 0] = RestR;
					(*OutShCoefficients)[ShBase + 3 + Coeff * 3 + 1] = RestG;
					(*OutShCoefficients)[ShBase + 3 + Coeff * 3 + 2] = RestB;
				}
			}
		}

		++OutIndex;
	}

	return OutSplats.Num() > 0;
}
