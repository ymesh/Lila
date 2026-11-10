using UnrealBuildTool;

public class LilaLink : ModuleRules
{
	public LilaLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"Sockets",
			"Networking",
			"Json",
			"DeveloperSettings",
			"Projects",
			"AssetTools",
			"AssetRegistry",
			"ContentBrowser",
			"InterchangeCore",
			"InterchangeEngine",
			"InterchangeNodes",
			"InterchangePipelines",
			"MaterialEditor",
			"Niagara",
			"NiagaraCore",
			"AlembicLibrary",
			"GeometryCache",
			"Landscape",
			"Foliage",
			"ImageCore",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"DesktopPlatform",
			"Settings",
			"ClothingSystemRuntimeCommon",
			"ClothingSystemRuntimeInterface",
			"ClothingSystemEditorInterface",
			"ChaosCloth",
			"ChaosClothAsset",
			"ChaosClothAssetEngine",
			"Chaos",
			"PhysicsCore",
			"GeometryCore",
			"GeometryCollectionEngine",
			"ChaosSolverEngine",
			"ChaosCaching",
			"StaticMeshEditor",
			"EditorSubsystem",
			"RenderCore",
			"RHI",
			"LilaLinkRuntime",
			"LilaLinkAnimGraph",
			"AnimGraph",
			"BlueprintGraph",
			"ControlRig",
			"ControlRigDeveloper",
			"HairStrandsCore",
		});

		// Editor modules that register component exporters include LilaComponentExporter.h, which uses FJsonObject.
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "Json" });

		// Header-only use: the module is looked up at runtime, so a project without Python still loads.
		PrivateIncludePathModuleNames.Add("PythonScriptPlugin");
	}
}
