// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianWebPLoader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

#if WITH_FREEIMAGE_LIB

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif

THIRD_PARTY_INCLUDES_START
#include "FreeImage.h"
THIRD_PARTY_INCLUDES_END

namespace GaussianWebPPrivate
{
	static void* FreeImageDllHandle = nullptr;

	static bool EnsureFreeImageLoaded()
	{
		if (FreeImageDllHandle != nullptr)
		{
			return true;
		}

		const FString FreeImageDir = FPaths::Combine(FPaths::EngineDir(), TEXT("Binaries/ThirdParty/FreeImage"), FPlatformProcess::GetBinariesSubdirectory());
		const FString FreeImageLibPath = FPaths::Combine(FreeImageDir, TEXT(FREEIMAGE_LIB_FILENAME));
		FPlatformProcess::PushDllDirectory(*FreeImageDir);
		FreeImageDllHandle = FPlatformProcess::GetDllHandle(*FreeImageLibPath);
		FPlatformProcess::PopDllDirectory(*FreeImageDir);

		if (FreeImageDllHandle)
		{
			::FreeImage_Initialise(FALSE);
		}

		return FreeImageDllHandle != nullptr;
	}
}

bool FGaussianWebPLoader::LoadFile(const FString& FilePath, FGaussianImageRGBA8& OutImage, FString& OutError)
{
	if (!GaussianWebPPrivate::EnsureFreeImageLoaded())
	{
		OutError = TEXT("Failed to load FreeImage library for WebP decode");
		return false;
	}

	FIBITMAP* Bitmap = FreeImage_Load(FIF_WEBP, TCHAR_TO_UTF8(*FilePath), 0);
	if (!Bitmap)
	{
		OutError = FString::Printf(TEXT("FreeImage failed to load WebP: %s"), *FilePath);
		return false;
	}

	const int32 Width = static_cast<int32>(FreeImage_GetWidth(Bitmap));
	const int32 Height = static_cast<int32>(FreeImage_GetHeight(Bitmap));
	FIBITMAP* Converted = FreeImage_ConvertTo32Bits(Bitmap);
	FreeImage_Unload(Bitmap);
	Bitmap = Converted;

	if (!Bitmap)
	{
		OutError = FString::Printf(TEXT("Failed to convert WebP to RGBA: %s"), *FilePath);
		return false;
	}

	OutImage.Width = Width;
	OutImage.Height = Height;
	OutImage.Pixels.SetNumUninitialized(Width * Height * 4);

	const int32 Pitch = static_cast<int32>(FreeImage_GetPitch(Bitmap));
	const uint8* Bits = FreeImage_GetBits(Bitmap);
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const uint8* SrcRow = Bits + (Height - 1 - Y) * Pitch;
		uint8* DstRow = OutImage.Pixels.GetData() + Y * Width * 4;
		for (int32 X = 0; X < Width; ++X)
		{
			DstRow[X * 4 + 0] = SrcRow[X * 4 + 2];
			DstRow[X * 4 + 1] = SrcRow[X * 4 + 1];
			DstRow[X * 4 + 2] = SrcRow[X * 4 + 0];
			DstRow[X * 4 + 3] = SrcRow[X * 4 + 3];
		}
	}

	FreeImage_Unload(Bitmap);
	return OutImage.IsValid();
}

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#else

bool FGaussianWebPLoader::LoadFile(const FString& FilePath, FGaussianImageRGBA8& OutImage, FString& OutError)
{
	OutError = TEXT("WebP import requires FreeImage (WITH_FREEIMAGE_LIB)");
	return false;
}

#endif
