// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianPlyReader.h"
#include "Import/GaussianImportUtils.h"
#include "Misc/FileHelper.h"

namespace GaussianPlyPrivate
{
	enum class EPropertyType : uint8
	{
		Float,
		Double,
		UChar,
		Char,
		UShort,
		Short,
		UInt,
		Int,
	};

	struct FProperty
	{
		FString Name;
		EPropertyType Type = EPropertyType::Float;
		int32 TypeSize = 4;
	};

	struct FElement
	{
		FString Name;
		int32 Count = 0;
		TArray<FProperty> Properties;
		int32 RecordSize = 0;
	};

	static constexpr int32 CompressedChunkSize = 256;
	static constexpr uint32 Mask11 = 0x7FFu;
	static constexpr uint32 Mask10 = 0x3FFu;
	static constexpr uint32 Mask8 = 0xFFu;
	static constexpr uint32 Mask2 = 0x3u;
	static constexpr float Inv2047 = 1.0f / 2047.0f;
	static constexpr float Inv1023 = 1.0f / 1023.0f;
	static constexpr float Inv255 = 1.0f / 255.0f;
	static constexpr float QuatNorm = 1.41421356237f;

	static bool ParseType(const FString& TypeName, EPropertyType& OutType, int32& OutSize)
	{
		if (TypeName == TEXT("float")) { OutType = EPropertyType::Float; OutSize = 4; return true; }
		if (TypeName == TEXT("double")) { OutType = EPropertyType::Double; OutSize = 8; return true; }
		if (TypeName == TEXT("uchar")) { OutType = EPropertyType::UChar; OutSize = 1; return true; }
		if (TypeName == TEXT("char")) { OutType = EPropertyType::Char; OutSize = 1; return true; }
		if (TypeName == TEXT("ushort")) { OutType = EPropertyType::UShort; OutSize = 2; return true; }
		if (TypeName == TEXT("short")) { OutType = EPropertyType::Short; OutSize = 2; return true; }
		if (TypeName == TEXT("uint")) { OutType = EPropertyType::UInt; OutSize = 4; return true; }
		if (TypeName == TEXT("int")) { OutType = EPropertyType::Int; OutSize = 4; return true; }
		return false;
	}

	static float ReadScalarAsFloat(const uint8* Data, EPropertyType Type)
	{
		switch (Type)
		{
		case EPropertyType::Float: return *reinterpret_cast<const float*>(Data);
		case EPropertyType::Double: return static_cast<float>(*reinterpret_cast<const double*>(Data));
		case EPropertyType::UChar: return static_cast<float>(*Data);
		case EPropertyType::Char: return static_cast<float>(*reinterpret_cast<const int8*>(Data));
		case EPropertyType::UShort: return static_cast<float>(*reinterpret_cast<const uint16*>(Data));
		case EPropertyType::Short: return static_cast<float>(*reinterpret_cast<const int16*>(Data));
		case EPropertyType::UInt: return static_cast<float>(*reinterpret_cast<const uint32*>(Data));
		case EPropertyType::Int: return static_cast<float>(*reinterpret_cast<const int32*>(Data));
		default: return 0.0f;
		}
	}

	static int32 FindPropertyIndex(const TArray<FProperty>& Properties, const TCHAR* Name)
	{
		for (int32 Index = 0; Index < Properties.Num(); ++Index)
		{
			if (Properties[Index].Name == Name)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	static void FinalizeElementRecordSize(FElement& Element)
	{
		Element.RecordSize = 0;
		for (const FProperty& Prop : Element.Properties)
		{
			Element.RecordSize += Prop.TypeSize;
		}
	}

	struct FCompressedChunkBounds
	{
		float MinX = 0, MinY = 0, MinZ = 0, MaxX = 0, MaxY = 0, MaxZ = 0;
		float MinScaleX = 0, MinScaleY = 0, MinScaleZ = 0, MaxScaleX = 0, MaxScaleY = 0, MaxScaleZ = 0;
		float MinR = 0, MinG = 0, MinB = 0, MaxR = 1, MaxG = 1, MaxB = 1;
	};

	static FVector4f DecodeCompressedQuaternion(uint32 Packed)
	{
		const float A = (static_cast<float>((Packed >> 20) & Mask10) * Inv1023 - 0.5f) * QuatNorm;
		const float B = (static_cast<float>((Packed >> 10) & Mask10) * Inv1023 - 0.5f) * QuatNorm;
		const float C = (static_cast<float>((Packed >> 0) & Mask10) * Inv1023 - 0.5f) * QuatNorm;
		const float M = FMath::Sqrt(FMath::Max(0.0f, 1.0f - (A * A + B * B + C * C)));
		const uint32 Which = (Packed >> 30) & Mask2;

		float W = 1.0f, X = 0.0f, Y = 0.0f, Z = 0.0f;
		switch (Which)
		{
		case 0: W = M; X = A; Y = B; Z = C; break;
		case 1: W = A; X = M; Y = B; Z = C; break;
		case 2: W = A; X = B; Y = M; Z = C; break;
		default: W = A; X = B; Y = C; Z = M; break;
		}

		return GaussianImport::PlyToUERotation(W, X, Y, Z);
	}

	static bool ReadCompressedBinaryPly(
		const TArray<uint8>& FileBytes,
		int32 HeaderEndOffset,
		const FElement& ChunkElement,
		const FElement& VertexElement,
		TArray<FGaussianSplatData>& OutSplats,
		FString& OutError)
	{
		if (!ChunkElement.Properties.IsValidIndex(17) || VertexElement.Properties.Num() < 4)
		{
			OutError = TEXT("Compressed PLY schema is incomplete");
			return false;
		}

		const int32 BodyOffset = HeaderEndOffset;
		const int32 ChunkBytes = ChunkElement.Count * ChunkElement.RecordSize;
		const int32 VertexBytes = VertexElement.Count * VertexElement.RecordSize;

		if (BodyOffset + ChunkBytes + VertexBytes > FileBytes.Num())
		{
			OutError = TEXT("Compressed PLY binary payload is truncated");
			return false;
		}

		TArray<FCompressedChunkBounds> Chunks;
		Chunks.SetNum(ChunkElement.Count);
		const uint8* ChunkData = FileBytes.GetData() + BodyOffset;

		for (int32 ChunkIndex = 0; ChunkIndex < ChunkElement.Count; ++ChunkIndex)
		{
			const uint8* Record = ChunkData + ChunkIndex * ChunkElement.RecordSize;
			TArray<float> Values;
			Values.SetNum(ChunkElement.Properties.Num());
			int32 RunningOffset = 0;
			for (int32 p = 0; p < ChunkElement.Properties.Num(); ++p)
			{
				Values[p] = ReadScalarAsFloat(Record + RunningOffset, ChunkElement.Properties[p].Type);
				RunningOffset += ChunkElement.Properties[p].TypeSize;
			}

			FCompressedChunkBounds& Chunk = Chunks[ChunkIndex];
			Chunk.MinX = Values[0]; Chunk.MinY = Values[1]; Chunk.MinZ = Values[2];
			Chunk.MaxX = Values[3]; Chunk.MaxY = Values[4]; Chunk.MaxZ = Values[5];
			Chunk.MinScaleX = Values[6]; Chunk.MinScaleY = Values[7]; Chunk.MinScaleZ = Values[8];
			Chunk.MaxScaleX = Values[9]; Chunk.MaxScaleY = Values[10]; Chunk.MaxScaleZ = Values[11];
			Chunk.MinR = Values[12]; Chunk.MinG = Values[13]; Chunk.MinB = Values[14];
			Chunk.MaxR = Values[15]; Chunk.MaxG = Values[16]; Chunk.MaxB = Values[17];
		}

		const uint8* VertexData = ChunkData + ChunkBytes;
		const int32 NumSplats = VertexElement.Count;
		OutSplats.Reset();
		OutSplats.Reserve(NumSplats);

		for (int32 SplatIndex = 0; SplatIndex < NumSplats; ++SplatIndex)
		{
			const uint8* Record = VertexData + SplatIndex * VertexElement.RecordSize;
			const uint32 PackedPosition = *reinterpret_cast<const uint32*>(Record + 0);
			const uint32 PackedRotation = *reinterpret_cast<const uint32*>(Record + 4);
			const uint32 PackedScale = *reinterpret_cast<const uint32*>(Record + 8);
			const uint32 PackedColor = *reinterpret_cast<const uint32*>(Record + 12);

			const int32 ChunkIndex = SplatIndex / CompressedChunkSize;
			if (!Chunks.IsValidIndex(ChunkIndex))
			{
				continue;
			}

			const FCompressedChunkBounds& Chunk = Chunks[ChunkIndex];

			const float Px = static_cast<float>((PackedPosition >> 21) & Mask11) * Inv2047;
			const float Py = static_cast<float>((PackedPosition >> 11) & Mask10) * Inv1023;
			const float Pz = static_cast<float>((PackedPosition >> 0) & Mask11) * Inv2047;
			const FVector3f RawPos(
				Chunk.MinX + Px * (Chunk.MaxX - Chunk.MinX),
				Chunk.MinY + Py * (Chunk.MaxY - Chunk.MinY),
				Chunk.MinZ + Pz * (Chunk.MaxZ - Chunk.MinZ));

			const float Sx = static_cast<float>((PackedScale >> 21) & Mask11) * Inv2047;
			const float Sy = static_cast<float>((PackedScale >> 11) & Mask10) * Inv1023;
			const float Sz = static_cast<float>((PackedScale >> 0) & Mask11) * Inv2047;

			const float Cr = static_cast<float>((PackedColor >> 24) & Mask8) * Inv255;
			const float Cg = static_cast<float>((PackedColor >> 16) & Mask8) * Inv255;
			const float Cb = static_cast<float>((PackedColor >> 8) & Mask8) * Inv255;
			const float Co = static_cast<float>((PackedColor >> 0) & Mask8) * Inv255;

			FGaussianSplatData Splat;
			Splat.Position = GaussianImport::PlyToUEPosition(RawPos);
			Splat.Scale = GaussianImport::MetersToUEScale(FVector3f(
				FMath::Max(Chunk.MinScaleX + Sx * (Chunk.MaxScaleX - Chunk.MinScaleX), KINDA_SMALL_NUMBER),
				FMath::Max(Chunk.MinScaleY + Sy * (Chunk.MaxScaleY - Chunk.MinScaleY), KINDA_SMALL_NUMBER),
				FMath::Max(Chunk.MinScaleZ + Sz * (Chunk.MaxScaleZ - Chunk.MinScaleZ), KINDA_SMALL_NUMBER)));
			Splat.Rotation = DecodeCompressedQuaternion(PackedRotation);
			Splat.Color = FVector4f(
				FMath::Clamp(Chunk.MinR + Cr * (Chunk.MaxR - Chunk.MinR), 0.0f, 1.0f),
				FMath::Clamp(Chunk.MinG + Cg * (Chunk.MaxG - Chunk.MinG), 0.0f, 1.0f),
				FMath::Clamp(Chunk.MinB + Cb * (Chunk.MaxB - Chunk.MinB), 0.0f, 1.0f),
				FMath::Clamp(Co, 0.0f, 1.0f));
			OutSplats.Add(Splat);
		}

		return OutSplats.Num() > 0;
	}
}

bool FGaussianPlyReader::ReadFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError)
{
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath))
	{
		OutError = FString::Printf(TEXT("Failed to read PLY file: %s"), *FilePath);
		return false;
	}

	if (FileBytes.Num() < 3 || FileBytes[0] != 'p' || FileBytes[1] != 'l' || FileBytes[2] != 'y')
	{
		OutError = TEXT("Invalid PLY header");
		return false;
	}

	FString HeaderText;
	int32 HeaderEndOffset = INDEX_NONE;
	{
		const ANSICHAR* Start = reinterpret_cast<const ANSICHAR*>(FileBytes.GetData());
		const int32 Len = FileBytes.Num();
		const ANSICHAR EndHeader[] = "end_header";
		const int32 EndHeaderLen = UE_ARRAY_COUNT(EndHeader) - 1;

		for (int32 i = 0; i <= Len - EndHeaderLen; ++i)
		{
			if (FMemory::Memcmp(Start + i, EndHeader, EndHeaderLen) == 0)
			{
				int32 j = i + EndHeaderLen;
				while (j < Len && (Start[j] == '\r' || Start[j] == '\n' || Start[j] == ' ' || Start[j] == '\t'))
				{
					++j;
				}
				while (j < Len && (Start[j] == '\r' || Start[j] == '\n'))
				{
					++j;
				}
				HeaderEndOffset = j;
				HeaderText = FString::ConstructFromPtrSize(Start, i + EndHeaderLen);
				break;
			}
		}
	}

	if (HeaderEndOffset == INDEX_NONE)
	{
		OutError = TEXT("PLY header end not found");
		return false;
	}

	TArray<FString> HeaderLines;
	HeaderText.ParseIntoArrayLines(HeaderLines);

	bool bBinaryLittleEndian = false;
	bool bBinary = false;
	TArray<GaussianPlyPrivate::FElement> Elements;
	GaussianPlyPrivate::FElement* CurrentElement = nullptr;

	for (const FString& Line : HeaderLines)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			continue;
		}

		TArray<FString> Tokens;
		Trimmed.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() == 0)
		{
			continue;
		}

		if (Tokens[0] == TEXT("format"))
		{
			if (Tokens.Num() >= 2)
			{
				if (Tokens[1] == TEXT("ascii"))
				{
					bBinary = false;
				}
				else if (Tokens[1] == TEXT("binary_little_endian"))
				{
					bBinary = true;
					bBinaryLittleEndian = true;
				}
				else if (Tokens[1] == TEXT("binary_big_endian"))
				{
					OutError = TEXT("binary_big_endian PLY is not supported");
					return false;
				}
			}
		}
		else if (Tokens[0] == TEXT("element") && Tokens.Num() >= 3)
		{
			GaussianPlyPrivate::FElement NewElement;
			NewElement.Name = Tokens[1];
			NewElement.Count = FCString::Atoi(*Tokens[2]);
			Elements.Add(NewElement);
			CurrentElement = &Elements.Last();
		}
		else if (Tokens[0] == TEXT("property") && CurrentElement && Tokens.Num() >= 3)
		{
			if (Tokens[1] == TEXT("list"))
			{
				OutError = TEXT("PLY list properties are not supported");
				return false;
			}

			GaussianPlyPrivate::FProperty Prop;
			Prop.Name = Tokens.Last();
			if (!GaussianPlyPrivate::ParseType(Tokens[1], Prop.Type, Prop.TypeSize))
			{
				OutError = FString::Printf(TEXT("Unsupported PLY property type: %s"), *Tokens[1]);
				return false;
			}
			CurrentElement->Properties.Add(Prop);
		}
	}

	for (GaussianPlyPrivate::FElement& Element : Elements)
	{
		GaussianPlyPrivate::FinalizeElementRecordSize(Element);
	}

	const GaussianPlyPrivate::FElement* VertexElement = nullptr;
	const GaussianPlyPrivate::FElement* ChunkElement = nullptr;
	for (const GaussianPlyPrivate::FElement& Element : Elements)
	{
		if (Element.Name == TEXT("vertex"))
		{
			VertexElement = &Element;
		}
		else if (Element.Name == TEXT("chunk"))
		{
			ChunkElement = &Element;
		}
	}

	if (!VertexElement || VertexElement->Count <= 0)
	{
		OutError = TEXT("PLY has no vertex data");
		return false;
	}

	const bool bIsCompressed = GaussianPlyPrivate::FindPropertyIndex(VertexElement->Properties, TEXT("packed_position")) != INDEX_NONE;
	if (bIsCompressed)
	{
		if (!bBinary || !bBinaryLittleEndian)
		{
			OutError = TEXT("Compressed PLY must be binary_little_endian");
			return false;
		}
		if (!ChunkElement)
		{
			OutError = TEXT("Compressed PLY is missing chunk element");
			return false;
		}

		if (!GaussianPlyPrivate::ReadCompressedBinaryPly(FileBytes, HeaderEndOffset, *ChunkElement, *VertexElement, OutSplats, OutError))
		{
			return false;
		}

		UE_LOG(LogTemp, Log, TEXT("Imported compressed SuperSplat PLY with %d Gaussians"), OutSplats.Num());
		return true;
	}

	const TArray<GaussianPlyPrivate::FProperty>& VertexProperties = VertexElement->Properties;
	const int32 VertexCount = VertexElement->Count;

	const int32 IdxX = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("x"));
	const int32 IdxY = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("y"));
	const int32 IdxZ = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("z"));
	if (IdxX == INDEX_NONE || IdxY == INDEX_NONE || IdxZ == INDEX_NONE)
	{
		OutError = TEXT("PLY is missing x/y/z properties (not standard or compressed 3DGS PLY)");
		return false;
	}

	const int32 IdxFdc0 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("f_dc_0"));
	const int32 IdxFdc1 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("f_dc_1"));
	const int32 IdxFdc2 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("f_dc_2"));
	const int32 IdxRed = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("red"));
	const int32 IdxGreen = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("green"));
	const int32 IdxBlue = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("blue"));
	const int32 IdxOpacity = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("opacity"));
	const int32 IdxScale0 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("scale_0"));
	const int32 IdxScale1 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("scale_1"));
	const int32 IdxScale2 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("scale_2"));
	const int32 IdxRot0 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("rot_0"));
	const int32 IdxRot1 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("rot_1"));
	const int32 IdxRot2 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("rot_2"));
	const int32 IdxRot3 = GaussianPlyPrivate::FindPropertyIndex(VertexProperties, TEXT("rot_3"));

	OutSplats.Reset();
	OutSplats.Reserve(VertexCount);

	auto GetPropertyValue = [&](const TArray<float>& Values, int32 Index, float DefaultValue) -> float
	{
		return Index != INDEX_NONE && Values.IsValidIndex(Index) ? Values[Index] : DefaultValue;
	};

	if (!bBinary)
	{
		FString AsciiData;
		AsciiData.Reserve(FileBytes.Num() - HeaderEndOffset);
		for (int32 i = HeaderEndOffset; i < FileBytes.Num(); ++i)
		{
			AsciiData.AppendChar(static_cast<TCHAR>(FileBytes[i]));
		}

		TArray<FString> DataLines;
		AsciiData.ParseIntoArrayLines(DataLines);

		int32 VertexRead = 0;
		for (const FString& Line : DataLines)
		{
			if (VertexRead >= VertexCount)
			{
				break;
			}

			const FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				continue;
			}

			TArray<FString> ValueTokens;
			Trimmed.ParseIntoArrayWS(ValueTokens);
			if (ValueTokens.Num() < VertexProperties.Num())
			{
				continue;
			}

			TArray<float> Values;
			Values.SetNum(VertexProperties.Num());
			for (int32 p = 0; p < VertexProperties.Num(); ++p)
			{
				Values[p] = FCString::Atof(*ValueTokens[p]);
			}

			const FVector3f RawPos(
				GetPropertyValue(Values, IdxX, 0.0f),
				GetPropertyValue(Values, IdxY, 0.0f),
				GetPropertyValue(Values, IdxZ, 0.0f));

			const float Fdc0 = GetPropertyValue(Values, IdxFdc0, 0.0f);
			const float Fdc1 = GetPropertyValue(Values, IdxFdc1, 0.0f);
			const float Fdc2 = GetPropertyValue(Values, IdxFdc2, 0.0f);
			const float OpacityLogit = GetPropertyValue(Values, IdxOpacity, 0.0f);

			float R = 1.0f;
			float G = 1.0f;
			float B = 1.0f;
			float A = GaussianImport::Sigmoid(OpacityLogit);

			if (IdxFdc0 != INDEX_NONE)
			{
				const FVector4f Color = GaussianImport::SH0ToLinearColor(Fdc0, Fdc1, Fdc2, A);
				R = Color.X; G = Color.Y; B = Color.Z; A = Color.W;
			}
			else if (IdxRed != INDEX_NONE)
			{
				R = GetPropertyValue(Values, IdxRed, 255.0f) / 255.0f;
				G = GetPropertyValue(Values, IdxGreen, 255.0f) / 255.0f;
				B = GetPropertyValue(Values, IdxBlue, 255.0f) / 255.0f;
			}

			const float ScaleX = FMath::Exp(GetPropertyValue(Values, IdxScale0, FMath::Loge(0.01f)));
			const float ScaleY = FMath::Exp(GetPropertyValue(Values, IdxScale1, FMath::Loge(0.01f)));
			const float ScaleZ = FMath::Exp(GetPropertyValue(Values, IdxScale2, FMath::Loge(0.01f)));

			const float RotW = GetPropertyValue(Values, IdxRot0, 1.0f);
			const float RotX = GetPropertyValue(Values, IdxRot1, 0.0f);
			const float RotY = GetPropertyValue(Values, IdxRot2, 0.0f);
			const float RotZ = GetPropertyValue(Values, IdxRot3, 0.0f);

			FGaussianSplatData Splat;
			Splat.Position = GaussianImport::PlyToUEPosition(RawPos);
			Splat.Scale = GaussianImport::MetersToUEScale(FVector3f(ScaleX, ScaleY, ScaleZ));
			Splat.Rotation = GaussianImport::PlyToUERotation(RotW, RotX, RotY, RotZ);
			Splat.Color = FVector4f(R, G, B, A);
			OutSplats.Add(Splat);
			++VertexRead;
		}
	}
	else
	{
		if (!bBinaryLittleEndian)
		{
			OutError = TEXT("Only little-endian binary PLY is supported");
			return false;
		}

		int32 DataOffset = HeaderEndOffset;
		for (const GaussianPlyPrivate::FElement& Element : Elements)
		{
			if (Element.Name == TEXT("vertex"))
			{
				break;
			}
			DataOffset += Element.Count * Element.RecordSize;
		}

		const int32 VertexStride = VertexElement->RecordSize;
		if (DataOffset + VertexCount * VertexStride > FileBytes.Num())
		{
			OutError = TEXT("PLY binary payload is truncated");
			return false;
		}

		TArray<int32> PropertyOffsets;
		PropertyOffsets.SetNum(VertexProperties.Num());
		int32 RunningOffset = 0;
		for (int32 p = 0; p < VertexProperties.Num(); ++p)
		{
			PropertyOffsets[p] = RunningOffset;
			RunningOffset += VertexProperties[p].TypeSize;
		}

		auto ReadProperty = [&](const uint8* VertexData, int32 PropertyIndex) -> float
		{
			const GaussianPlyPrivate::FProperty& Prop = VertexProperties[PropertyIndex];
			return GaussianPlyPrivate::ReadScalarAsFloat(VertexData + PropertyOffsets[PropertyIndex], Prop.Type);
		};

		for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			const uint8* VertexData = FileBytes.GetData() + DataOffset + VertexIndex * VertexStride;

			const FVector3f RawPos(ReadProperty(VertexData, IdxX), ReadProperty(VertexData, IdxY), ReadProperty(VertexData, IdxZ));

			const float Fdc0 = IdxFdc0 != INDEX_NONE ? ReadProperty(VertexData, IdxFdc0) : 0.0f;
			const float Fdc1 = IdxFdc1 != INDEX_NONE ? ReadProperty(VertexData, IdxFdc1) : 0.0f;
			const float Fdc2 = IdxFdc2 != INDEX_NONE ? ReadProperty(VertexData, IdxFdc2) : 0.0f;
			const float OpacityLogit = IdxOpacity != INDEX_NONE ? ReadProperty(VertexData, IdxOpacity) : 0.0f;

			float R = 1.0f, G = 1.0f, B = 1.0f;
			float A = GaussianImport::Sigmoid(OpacityLogit);

			if (IdxFdc0 != INDEX_NONE)
			{
				const FVector4f Color = GaussianImport::SH0ToLinearColor(Fdc0, Fdc1, Fdc2, A);
				R = Color.X; G = Color.Y; B = Color.Z; A = Color.W;
			}
			else if (IdxRed != INDEX_NONE)
			{
				R = ReadProperty(VertexData, IdxRed) / 255.0f;
				G = ReadProperty(VertexData, IdxGreen) / 255.0f;
				B = ReadProperty(VertexData, IdxBlue) / 255.0f;
			}

			const float ScaleX = FMath::Exp(IdxScale0 != INDEX_NONE ? ReadProperty(VertexData, IdxScale0) : FMath::Loge(0.01f));
			const float ScaleY = FMath::Exp(IdxScale1 != INDEX_NONE ? ReadProperty(VertexData, IdxScale1) : FMath::Loge(0.01f));
			const float ScaleZ = FMath::Exp(IdxScale2 != INDEX_NONE ? ReadProperty(VertexData, IdxScale2) : FMath::Loge(0.01f));

			const float RotW = IdxRot0 != INDEX_NONE ? ReadProperty(VertexData, IdxRot0) : 1.0f;
			const float RotX = IdxRot1 != INDEX_NONE ? ReadProperty(VertexData, IdxRot1) : 0.0f;
			const float RotY = IdxRot2 != INDEX_NONE ? ReadProperty(VertexData, IdxRot2) : 0.0f;
			const float RotZ = IdxRot3 != INDEX_NONE ? ReadProperty(VertexData, IdxRot3) : 0.0f;

			FGaussianSplatData Splat;
			Splat.Position = GaussianImport::PlyToUEPosition(RawPos);
			Splat.Scale = GaussianImport::MetersToUEScale(FVector3f(ScaleX, ScaleY, ScaleZ));
			Splat.Rotation = GaussianImport::PlyToUERotation(RotW, RotX, RotY, RotZ);
			Splat.Color = FVector4f(R, G, B, A);
			OutSplats.Add(Splat);
		}
	}

	if (OutSplats.Num() == 0)
	{
		OutError = TEXT("No Gaussians parsed from PLY");
		return false;
	}

	return true;
}
