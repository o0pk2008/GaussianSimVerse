// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class GaussianSimVerse : ModuleRules
{
	public GaussianSimVerse(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Public",
		});

		PrivateIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Private",
		});

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RenderCore",
			"RHI",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"Renderer",
			"Slate",
			"SlateCore",
			"Json",
			"JsonUtilities",
			"ImageWrapper",
			"FreeImage",
		});

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		string RendererPath = Path.Combine(EngineDirectory, "Source", "Runtime", "Renderer");
		PrivateIncludePaths.AddRange(new string[]
		{
			Path.Combine(RendererPath, "Internal"),
			Path.Combine(RendererPath, "Private"),
		});

		PublicDefinitions.Add("GAUSSIANSIMVERSE_SHADERS_DIR=\"/Plugin/GaussianSimVerse\"");
	}
}
