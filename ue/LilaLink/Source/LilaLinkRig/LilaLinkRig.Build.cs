using UnrealBuildTool;

public class LilaLinkRig : ModuleRules
{
	public LilaLinkRig(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"RigVM",
			"ControlRig",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Eigen",
			"AnimationCore",
			"MovieScene",
			"MovieSceneTracks",
			"Json",
		});
	}
}
