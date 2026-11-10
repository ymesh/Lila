using UnrealBuildTool;

public class LilaLinkAnimGraph : ModuleRules
{
	public LilaLinkAnimGraph(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AnimGraph",
			"BlueprintGraph",
			"LilaLinkRuntime",
		});
	}
}
