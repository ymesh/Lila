using UnrealBuildTool;

public class LilaLinkCrowdRuntime : ModuleRules
{
	public LilaLinkCrowdRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PublicDependencyModuleNames.AddRange(new string[] { "MassEntity" });
		PrivateDependencyModuleNames.AddRange(new string[] { "ZoneGraph", "MassCrowd", "MassCore", "MassCommon", "MassActors", "MassRepresentation" });
	}
}
