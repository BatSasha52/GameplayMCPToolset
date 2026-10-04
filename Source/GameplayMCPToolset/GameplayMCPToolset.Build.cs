// Copyright (c) GameplayMCPToolset contributors. Licensed under the MIT License.

using UnrealBuildTool;

public class GameplayMCPToolset : ModuleRules
{
	public GameplayMCPToolset(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
				"JsonUtilities",
				"ToolsetRegistry",
			}
		);

		// The Unreal MCP plugin (ModelContextProtocol) discovers toolsets through the
		// ToolsetRegistry at runtime, so this module does not link against it.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"EnhancedInput",
				"LevelEditor",
				"RHI",
				"Slate",
				"SlateCore",
				"SubobjectDataInterface",
				"UnrealEd",
			}
		);

		// Live Coding only exists on targets built with it (Win64 editor). The tools report a clear
		// error everywhere else.
		if (Target.bWithLiveCoding)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
