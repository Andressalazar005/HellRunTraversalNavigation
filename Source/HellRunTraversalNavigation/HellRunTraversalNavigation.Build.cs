using UnrealBuildTool;

public class HellRunTraversalNavigation : ModuleRules
{
    public HellRunTraversalNavigation(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AIModule",
            "NavigationSystem",
            "DeveloperSettings"
        });

        PrivateDependencyModuleNames.Add("Navmesh");

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[]
            {
                "Slate",
                "SlateCore",
                "UnrealEd",
                "AutomationController"
            });
        }
    }
}
