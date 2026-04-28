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
            "LevelEditor",       // FLevelEditorViewportClient (Phase 4.6-r3)
            "EditorSubsystem",
            "MaterialEditor",
            "SourceControl",
            "AssetRegistry",
            "Json",
            "JsonUtilities",
            "Projects",
            // Blueprint read + write (Phase 4.2)
            "Kismet",
            "KismetCompiler",
            "BlueprintGraph",
            "GraphEditor",
            // Material graph + instance creation (Phase 4.3)
            "AssetTools",
            // Dialog tools (Phase 4.6 r2): SWindow / SButton / STextBlock /
            // FSlateApplication for active modal traversal + button click
            // simulation. Slate is transitively available via UnrealEd, but
            // SButton's OnMouseButtonDown/Up live in Slate proper.
            "Slate",
            "SlateCore",
            "ApplicationCore",
            "InputCore",
            // Python scripting (Phase 4.6 r3 b5) — IPythonScriptPlugin.
            // Module ships with the engine; if the project hasn't enabled
            // the Python plugin, IPythonScriptPlugin::Get()->IsPythonAvailable()
            // returns false and our handler reports a graceful error.
            "PythonScriptPlugin",
            // UMG widget authoring (Phase 4.11) — UWidgetBlueprint,
            // UWidgetTree, UWidget hierarchy. UMGEditor for the factory.
            "UMG",
            "UMGEditor",
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
