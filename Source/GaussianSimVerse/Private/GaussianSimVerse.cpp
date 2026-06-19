// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSimVerse.h"
#include "Rendering/GaussianRenderer.h"
#include "Engine/Engine.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogGaussianSimVerse);

#define LOCTEXT_NAMESPACE "FGaussianSimVerseModule"

void FGaussianSimVerseModule::StartupModule()
{
	RegisterShaderDirectory();

	if (GEngine)
	{
		InitializeRenderer();
	}
	else
	{
		PostEngineInitDelegateHandle = FCoreDelegates::OnPostEngineInit.AddLambda([this]()
		{
			InitializeRenderer();
		});
	}
}

void FGaussianSimVerseModule::ShutdownModule()
{
	if (PostEngineInitDelegateHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitDelegateHandle);
		PostEngineInitDelegateHandle.Reset();
	}

	FGaussianRenderer::Get().Shutdown();
	UnregisterShaderDirectory();
}

void FGaussianSimVerseModule::InitializeRenderer()
{
	if (!FGaussianRenderer::Get().IsInitialized())
	{
		FGaussianRenderer::Get().Initialize();
	}
}

void FGaussianSimVerseModule::RegisterShaderDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GaussianSimVerse"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogGaussianSimVerse, Error, TEXT("Failed to locate GaussianSimVerse plugin for shader directory mapping"));
		return;
	}

	ShaderDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/GaussianSimVerse"), ShaderDirectory);

	UE_LOG(LogGaussianSimVerse, Log, TEXT("Mapped shader directory /Plugin/GaussianSimVerse -> %s"), *ShaderDirectory);
}

void FGaussianSimVerseModule::UnregisterShaderDirectory()
{
	// UE does not expose per-mapping removal; mapping is process-lifetime.
	ShaderDirectory.Reset();
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGaussianSimVerseModule, GaussianSimVerse)
