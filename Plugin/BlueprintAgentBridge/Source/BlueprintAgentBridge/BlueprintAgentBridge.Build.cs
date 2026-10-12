// Copyright (c) 2026 Matteo La Barbera. MIT License.

using UnrealBuildTool;

public class BlueprintAgentBridge : ModuleRules
{
	public BlueprintAgentBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"InputCore",
			"UnrealEd",
			"BlueprintGraph",
			"KismetCompiler",
			"Kismet",
			"GraphEditor",
			"Json",
			"JsonUtilities",
			"HTTPServer",
			"AssetRegistry",
			"Projects",
			"DeveloperSettings",
			"Slate",
			"SlateCore",
			"EditorSubsystem",
		});
	}
}
