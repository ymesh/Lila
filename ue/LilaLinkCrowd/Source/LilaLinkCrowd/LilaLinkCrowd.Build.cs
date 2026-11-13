using UnrealBuildTool;

public class LilaLinkCrowd : ModuleRules
{
	public LilaLinkCrowd(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Json",
			"AssetRegistry",
			"LilaLink",
			"ZoneGraph",
			"MassEntity",
			"MassCore",
			"PropertyBindingUtils",
			"MassCommon",
			"MassSpawner",
			"MassActors",
			"MassMovement",
			"MassLOD",
			"MassRepresentation",
			"MassNavigation",
			"MassZoneGraphNavigation",
			"MassAIBehavior",
			"MassCrowd",
			"LilaLinkCrowdRuntime",
			"ZoneGraphAnnotations",
			"StateTreeModule",
			"StateTreeEditorModule",
		});
	}
}
