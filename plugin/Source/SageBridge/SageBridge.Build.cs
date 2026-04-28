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
            "MaterialEditor",
            "SourceControl",
            "AssetRegistry",
            "Json",
            "JsonUtilities",
            "Projects",
        });

        // Live Coding ships only on Windows in UE 5.7; Mac/Linux use the
        // cross-platform stub paths in SageCompileTools.cpp.
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            PrivateDependencyModuleNames.Add("LiveCoding");
        }

        DynamicallyLoadedModuleNames.AddRange(new string[] { });
    }
}
