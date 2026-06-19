// Copyright Epic Games, Inc. All Rights Reserved.

#include "Import/GaussianZipUtil.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

namespace GaussianZipPrivate
{
	static bool RunPowerShellExpandArchive(const FString& ZipPath, const FString& DestDir, FString& OutError)
	{
		const FString PowerShellArgs = FString::Printf(
			TEXT("-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force\""),
			*ZipPath.Replace(TEXT("'"), TEXT("''")),
			*DestDir.Replace(TEXT("'"), TEXT("''")));

		int32 ReturnCode = 1;
		FProcHandle ProcHandle = FPlatformProcess::CreateProc(
			TEXT("powershell.exe"),
			*PowerShellArgs,
			true,
			true,
			true,
			nullptr,
			0,
			nullptr,
			nullptr,
			nullptr);

		if (!ProcHandle.IsValid())
		{
			OutError = TEXT("Failed to launch PowerShell for archive extraction");
			return false;
		}

		FPlatformProcess::WaitForProc(ProcHandle);
		FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
		FPlatformProcess::CloseProc(ProcHandle);

		if (ReturnCode != 0)
		{
			OutError = FString::Printf(TEXT("Expand-Archive failed (code %d)"), ReturnCode);
			return false;
		}

		return true;
	}
}

bool FGaussianZipUtil::ExtractArchive(const FString& ArchivePath, FString& OutExtractedDir, FString& OutError)
{
	const FString AbsoluteArchive = FPaths::ConvertRelativePathToFull(ArchivePath);
	if (!IFileManager::Get().FileExists(*AbsoluteArchive))
	{
		OutError = FString::Printf(TEXT("Archive not found: %s"), *AbsoluteArchive);
		return false;
	}

	OutExtractedDir = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("GaussianSimVerse"),
		TEXT("SogImport"),
		FGuid::NewGuid().ToString());

	IFileManager::Get().MakeDirectory(*OutExtractedDir, true);

	const FString AbsoluteOut = FPaths::ConvertRelativePathToFull(OutExtractedDir);

	// Expand-Archive only accepts .zip extension — copy to an intermediate ASCII-safe path first.
	const FString TempZipPath = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("GaussianSimVerse"),
		TEXT("SogImport"),
		FGuid::NewGuid().ToString() + TEXT(".zip"));

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TempZipPath), true);

	if (IFileManager::Get().Copy(*TempZipPath, *AbsoluteArchive, true, true) != COPY_OK)
	{
		OutError = FString::Printf(TEXT("Failed to copy archive for extraction: %s"), *AbsoluteArchive);
		return false;
	}

#if PLATFORM_WINDOWS
	if (!GaussianZipPrivate::RunPowerShellExpandArchive(TempZipPath, AbsoluteOut, OutError))
	{
		IFileManager::Get().Delete(*TempZipPath, false, true);
		OutError = FString::Printf(TEXT("%s for: %s"), *OutError, *AbsoluteArchive);
		return false;
	}
#else
	{
		int32 ReturnCode = 1;
		const FString UnzipArgs = FString::Printf(TEXT("-o \"%s\" -d \"%s\""), *TempZipPath, *AbsoluteOut);
		FProcHandle ProcHandle = FPlatformProcess::CreateProc(TEXT("unzip"), *UnzipArgs, true, true, true, nullptr, 0, nullptr, nullptr, nullptr);
		if (!ProcHandle.IsValid())
		{
			IFileManager::Get().Delete(*TempZipPath, false, true);
			OutError = TEXT("Failed to launch unzip. Extract the .sog archive manually and import meta.json.");
			return false;
		}

		FPlatformProcess::WaitForProc(ProcHandle);
		FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);
		FPlatformProcess::CloseProc(ProcHandle);
		IFileManager::Get().Delete(*TempZipPath, false, true);

		if (ReturnCode != 0)
		{
			OutError = FString::Printf(TEXT("unzip failed (code %d) for: %s"), ReturnCode, *AbsoluteArchive);
			return false;
		}
	}
#endif

	IFileManager::Get().Delete(*TempZipPath, false, true);

	const FString MetaPath = FPaths::Combine(OutExtractedDir, TEXT("meta.json"));
	if (!IFileManager::Get().FileExists(*MetaPath))
	{
		OutError = FString::Printf(TEXT("Extracted archive does not contain meta.json: %s"), *OutExtractedDir);
		return false;
	}

	return true;
}
