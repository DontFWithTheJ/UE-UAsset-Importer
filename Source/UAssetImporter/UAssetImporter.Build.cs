using UnrealBuildTool;

public class UAssetImporter : ModuleRules
{
	public UAssetImporter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"ContentBrowser"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"AssetRegistry",
			"AssetTools",
			"DesktopPlatform",
			"MessageLog",
			"Projects",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"ContentBrowserData",
			"ImageCore",
			"InputCore",
			"RenderCore"
		});
	}
}