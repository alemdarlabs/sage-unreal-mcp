#include "Identity/SageSlotID.h"
#include "SageBridge.h"

#include "Hash/Blake3.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "String/BytesToHex.h"

namespace
{
constexpr uint8 kNullSeparator = 0;

void UpdateUtf8(FBlake3& Hasher, const FString& Value)
{
    const FTCHARToUTF8 Utf8(*Value);
    if (Utf8.Length() > 0)
    {
        Hasher.Update(reinterpret_cast<const void*>(Utf8.Get()),
                      static_cast<uint64>(Utf8.Length()));
    }
}
}  // namespace

FString FSageSlotID::Compute(const FString& ProjectId,
                              const FString& CanonicalPath,
                              const FString& EngineMajor)
{
    FBlake3 Hasher;
    UpdateUtf8(Hasher, ProjectId);
    Hasher.Update(&kNullSeparator, 1);
    UpdateUtf8(Hasher, CanonicalPath);
    Hasher.Update(&kNullSeparator, 1);
    UpdateUtf8(Hasher, EngineMajor);

    const FBlake3Hash Hash = Hasher.Finalize();
    return BytesToHex(Hash.GetBytes(), 32).ToLower();
}

FString FSageSlotID::ResolveProjectId()
{
    FString ProjectId;
    if (GConfig != nullptr)
    {
        GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"),
                           TEXT("ProjectID"),
                           ProjectId,
                           GGameIni);
    }
    // FGuid::ToString returns "{XXX...}" sometimes; strip braces for a stable identity input.
    ProjectId.RemoveFromStart(TEXT("{"));
    ProjectId.RemoveFromEnd(TEXT("}"));
    return ProjectId;
}

FString FSageSlotID::ResolveCanonicalPath()
{
    FString ProjectFile = FPaths::GetProjectFilePath();
    ProjectFile = FPaths::ConvertRelativePathToFull(ProjectFile);
    FPaths::NormalizeFilename(ProjectFile);
    FPaths::CollapseRelativeDirectories(ProjectFile);
    return ProjectFile;
}

FString FSageSlotID::ResolveEngineMajor()
{
    const FEngineVersion& V = FEngineVersion::Current();
    return FString::Printf(TEXT("%u.%u"), V.GetMajor(), V.GetMinor());
}

FString FSageSlotID::ComputeForCurrentProject()
{
    return Compute(ResolveProjectId(), ResolveCanonicalPath(), ResolveEngineMajor());
}
