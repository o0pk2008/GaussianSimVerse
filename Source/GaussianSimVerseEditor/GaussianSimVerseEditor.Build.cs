// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class GaussianSimVerseEditor : ModuleRules
{
	public GaussianSimVerseEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GaussianSimVerse",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"AssetTools",
			"EditorFramework",
			"Slate",
			"SlateCore",
			"Projects",
			"Json",
			"JsonUtilities",
			"FreeImage",
			"LevelEditor",
			"ImageWrapper",
		});
	}
}
