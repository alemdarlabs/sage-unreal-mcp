// Copyright alemdarlabs. SPDX-License-Identifier: TBD (see ADR-014 follow-up)

using UnrealBuildTool;

public class SageBridge : ModuleRules
{
    public SageBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        IWYUSupport = IWYUSupport.Full;

        PublicIncludePaths.AddRange(new string[] { });
        PrivateIncludePaths.AddRange(new string[] { });

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "DeveloperSettings",
            "WebSockets",
        });

        PrivateDependencyModuleNames.AddRange(new string[] {
            "UnrealEd",
            "EditorSubsystem",
            "Json",
            "JsonUtilities",
            "Projects",
        });

        DynamicallyLoadedModuleNames.AddRange(new string[] { });
    }
}
