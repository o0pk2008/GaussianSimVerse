// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianSplatReader.h"
#include "Import/GaussianImportUtils.h"
#include "Misc/FileHelper.h"

namespace GaussianSplatPrivate
{
#pragma pack(push, 1)
	struct FSplatRecord
	{
		float Position[3];
		float Scale[3];
		uint8 Color[4];
		uint8 Rotation[4];
	};
#pragma pack(pop)
	static_assert(sizeof(FSplatRecord) == 32, "FSplatRecord must be 32 bytes");

	static FVector4f DecodeQuaternion(const uint8 Rotation[4])
	{
		FVector4f Q;
		Q.X = (Rotation[0] / 255.0f) * 2.0f - 1.0f;
		Q.Y = (Rotation[1] / 255.0f) * 2.0f - 1.0f;
		Q.Z = (Rotation[2] / 255.0f) * 2.0f - 1.0f;
		Q.W = (Rotation[3] / 255.0f) * 2.0f - 1.0f;

		const float LenSq = Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W;
		if (LenSq > KINDA_SMALL_NUMBER)
		{
			const float InvLen = FMath::InvSqrt(LenSq);
			Q.X *= InvLen;
			Q.Y *= InvLen;
			Q.Z *= InvLen;
			Q.W *= InvLen;
		}
		else
		{
			Q = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
		}

		return GaussianImport::PlyToUERotation(Q.W, Q.X, Q.Y, Q.Z);
	}
}

bool FGaussianSplatReader::ReadFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError)
{
	TArray<uint8> FileBytes;
	if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath))
	{
		OutError = FString::Printf(TEXT("Failed to read .splat file: %s"), *FilePath);
		return false;
	}

	if (FileBytes.Num() < static_cast<int32>(sizeof(GaussianSplatPrivate::FSplatRecord)))
	{
		OutError = TEXT(".splat file is too small");
		return false;
	}

	if (FileBytes.Num() % sizeof(GaussianSplatPrivate::FSplatRecord) != 0)
	{
		OutError = TEXT(".splat file size is not a multiple of 32 bytes");
		return false;
	}

	const int32 SplatCount = FileBytes.Num() / sizeof(GaussianSplatPrivate::FSplatRecord);
	OutSplats.Reset();
	OutSplats.Reserve(SplatCount);

	const GaussianSplatPrivate::FSplatRecord* Records = reinterpret_cast<const GaussianSplatPrivate::FSplatRecord*>(FileBytes.GetData());
	for (int32 Index = 0; Index < SplatCount; ++Index)
	{
		const GaussianSplatPrivate::FSplatRecord& Record = Records[Index];

		FGaussianSplatData Splat;
		const FVector3f RawPos(Record.Position[0], Record.Position[1], Record.Position[2]);
		Splat.Position = GaussianImport::PlyToUEPosition(RawPos);
		Splat.Scale = GaussianImport::MetersToUEScale(FVector3f(
			FMath::Max(Record.Scale[0], KINDA_SMALL_NUMBER),
			FMath::Max(Record.Scale[1], KINDA_SMALL_NUMBER),
			FMath::Max(Record.Scale[2], KINDA_SMALL_NUMBER)));
		Splat.Rotation = GaussianSplatPrivate::DecodeQuaternion(Record.Rotation);
		Splat.Color = FVector4f(
			Record.Color[0] / 255.0f,
			Record.Color[1] / 255.0f,
			Record.Color[2] / 255.0f,
			Record.Color[3] / 255.0f);
		OutSplats.Add(Splat);
	}

	return OutSplats.Num() > 0;
}
