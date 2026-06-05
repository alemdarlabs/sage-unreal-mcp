#include "Identity/SageEditorIdentity.h"
#include "Identity/SageSlotID.h"
#include "SageBridge.h"
#include "SageBridgeSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"

namespace
{
FString ResolveSageBridgePluginVersion()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SageBridge"));
    if (Plugin.IsValid() && !Plugin->GetDescriptor().VersionName.IsEmpty())
    {
        return Plugin->GetDescriptor().VersionName;
    }
    return TEXT("unknown");
}
}

FSageEditorIdentity FSageEditorIdentity::Snapshot()
{
    FSageEditorIdentity Id;

    Id.ProjectId   = FSageSlotID::ResolveProjectId();
    Id.ProjectPath = FSageSlotID::ResolveCanonicalPath();
    Id.SlotId      = FSageSlotID::Compute(Id.ProjectId,
                                          Id.ProjectPath,
                                          FSageSlotID::ResolveEngineMajor());

    const FEngineVersion& V = FEngineVersion::Current();
    Id.EngineVersion = FString::Printf(TEXT("%u.%u.%u"),
                                       V.GetMajor(), V.GetMinor(), V.GetPatch());

    Id.Pid = static_cast<int32>(FPlatformProcess::GetCurrentProcessId());
    Id.SessionId = FGuid::NewGuid()
                       .ToString(EGuidFormats::DigitsWithHyphensLower)
                       .Left(8);

    const FString ProjectName = FApp::GetProjectName();
    Id.InstanceId = FString::Printf(TEXT("%s@%s"), *ProjectName, *Id.SessionId);

    if (const USageBridgeSettings* Settings = GetDefault<USageBridgeSettings>())
    {
        Id.Label = Settings->GetResolvedLabel();
    }

    Id.StartedAt = FDateTime::UtcNow();

    return Id;
}

TSharedRef<FJsonObject> FSageEditorIdentity::ToHandshakeJson() const
{
    const TSharedRef<FJsonObject> Editor = MakeShared<FJsonObject>();
    Editor->SetStringField(TEXT("id"),             InstanceId);
    Editor->SetStringField(TEXT("label"),          Label);
    Editor->SetStringField(TEXT("project_id"),     ProjectId);
    Editor->SetStringField(TEXT("project_path"),   ProjectPath);
    Editor->SetStringField(TEXT("engine_version"), EngineVersion);
    Editor->SetStringField(TEXT("session_id"),     SessionId);
    Editor->SetNumberField(TEXT("pid"),            static_cast<double>(Pid));
    Editor->SetStringField(TEXT("started_at"),     StartedAt.ToIso8601());

    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("type"),    TEXT("hello"));
    Root->SetStringField(TEXT("version"), TEXT("0.1.0"));
    Root->SetStringField(TEXT("plugin_version"), ResolveSageBridgePluginVersion());
    Root->SetStringField(TEXT("slot_id"), SlotId);
    Root->SetObjectField(TEXT("editor"),  Editor);
    // asset_registry_hash deferred to Phase 2 (Knowledge layer indexing)
    return Root;
}
