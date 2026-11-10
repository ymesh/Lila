using UnrealBuildTool;

public class LilaLinkDeformer : ModuleRules
{
	public LilaLinkDeformer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ComputeFramework",
			"OptimusCore",
			"RenderCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"Json",
			"Renderer",
			"RHI",
		});
	}
}
