// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FGaussianImageRGBA8
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> Pixels;

	bool IsValid() const { return Width > 0 && Height > 0 && Pixels.Num() == Width * Height * 4; }

	void GetPixel(int32 X, int32 Y, uint8& OutR, uint8& OutG, uint8& OutB, uint8& OutA) const
	{
		const int32 Index = (Y * Width + X) * 4;
		OutR = Pixels[Index + 0];
		OutG = Pixels[Index + 1];
		OutB = Pixels[Index + 2];
		OutA = Pixels[Index + 3];
	}
};

class GAUSSIANSIMVERSEEDITOR_API FGaussianWebPLoader
{
public:
	static bool LoadFile(const FString& FilePath, FGaussianImageRGBA8& OutImage, FString& OutError);
};
