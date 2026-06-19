// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGaussianSimVerse, Log, All);

class FGaussianRenderer;

class GAUSSIANSIMVERSE_API FGaussianSimVerseModule : public IModuleInterface
{
public:
	static inline FGaussianSimVerseModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FGaussianSimVerseModule>(TEXT("GaussianSimVerse"));
	}

	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("GaussianSimVerse"));
	}

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	const FString& GetShaderDirectory() const { return ShaderDirectory; }

private:
	void RegisterShaderDirectory();
	void UnregisterShaderDirectory();
	void InitializeRenderer();

	FString ShaderDirectory;
	FDelegateHandle PostEngineInitDelegateHandle;
};
