#include "Tools/SageAudioTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "SageAudio"

namespace sage::tools
{
namespace
{

static UClass* FindAudioClass(const TCHAR* Name)
{
    UClass* Cls = FindObject<UClass>(nullptr, Name);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, Name);
    return Cls;
}

// ---- audio.list ------------------------------------------------------------

FSageToolDispatch::FOutcome AudioListImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);

    FString TypeFilter;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("type"), TypeFilter);

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = true;

    // Add SoundWave + SoundCue by default; MetaSound if requested
    UClass* WaveCls  = FindObject<UClass>(nullptr, TEXT("/Script/Engine.SoundWave"));
    UClass* CueCls   = FindObject<UClass>(nullptr, TEXT("/Script/Engine.SoundCue"));
    UClass* MetaCls  = FindAudioClass(TEXT("/Script/MetasoundEngine.MetaSoundSource"));

    if (TypeFilter.IsEmpty() || TypeFilter.Equals(TEXT("SoundWave"), ESearchCase::IgnoreCase))
        if (WaveCls)  Filter.ClassPaths.Add(WaveCls->GetClassPathName());
    if (TypeFilter.IsEmpty() || TypeFilter.Equals(TEXT("SoundCue"), ESearchCase::IgnoreCase))
        if (CueCls)   Filter.ClassPaths.Add(CueCls->GetClassPathName());
    if (TypeFilter.Equals(TEXT("MetaSound"), ESearchCase::IgnoreCase))
        if (MetaCls)  Filter.ClassPaths.Add(MetaCls->GetClassPathName());

    if (Filter.ClassPaths.IsEmpty())
    {
        // Fallback: list all assets under path
        Filter.bRecursiveClasses = false;
    }

    TArray<FAssetData> Assets;
    ARM.Get().GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FAssetData& D : Assets)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),  D.AssetName.ToString());
        J->SetStringField(TEXT("path"),  D.GetSoftObjectPath().ToString());
        J->SetStringField(TEXT("class"), D.AssetClassPath.GetAssetName().ToString());
        Items.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("assets"), Items);
    R->SetNumberField(TEXT("count"),  Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- audio.play_at_location ------------------------------------------------

FSageToolDispatch::FOutcome AudioPlayAtLocationImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SoundPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), SoundPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FVector Loc = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Loc);

    float Volume = 1.0f;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("volume"), Volume);

    // Trigger via console command — PlaySound is editor-only
    FString Cmd = FString::Printf(TEXT("au.PlaySound %s"), *SoundPath);
    GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Cmd, *GLog);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),   SoundPath);
    R->SetArrayField (TEXT("location"), {
        MakeShared<FJsonValueNumber>(Loc.X),
        MakeShared<FJsonValueNumber>(Loc.Y),
        MakeShared<FJsonValueNumber>(Loc.Z)
    });
    R->SetNumberField(TEXT("volume"), Volume);
    R->SetBoolField  (TEXT("triggered"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- audio.spawn_ambient ---------------------------------------------------

FSageToolDispatch::FOutcome AudioSpawnAmbientImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SoundPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), SoundPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FVector Loc = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Loc);

    UClass* AmbientCls = FindObject<UClass>(nullptr, TEXT("/Script/Engine.AmbientSound"));
    if (!AmbientCls) AmbientCls = LoadObject<UClass>(nullptr, TEXT("/Script/Engine.AmbientSound"));
    if (!AmbientCls) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("AmbientSound class not found"));

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FTransform Tf(FRotator::ZeroRotator, Loc);
    FScopedTransaction Tx(LOCTEXT("SpawnAmbient", "Spawn Ambient Sound"));
    AActor* A = GEditor->AddActor(World->GetCurrentLevel(), AmbientCls, Tf);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("AddActor failed"));

    // Assign sound via reflection
    FProperty* Prop = FindFProperty<FProperty>(A->GetClass(), TEXT("AudioComponent"));
    if (!Prop)
    {
        // Try to set via component
        TArray<UActorComponent*> Comps;
        A->GetComponents(Comps);
        for (UActorComponent* C : Comps)
        {
            if (C && C->GetClass()->GetName().Contains(TEXT("Audio")))
            {
                FProperty* SoundProp = FindFProperty<FProperty>(C->GetClass(), TEXT("Sound"));
                if (SoundProp)
                {
                    detail::SetUPropertyFromJson(C, SoundProp,
                        MakeShared<FJsonValueString>(SoundPath));
                }
                break;
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),   A->GetPathName());
    R->SetStringField(TEXT("sound_path"), SoundPath);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- audio.create_cue ------------------------------------------------------

FSageToolDispatch::FOutcome AudioCreateCueImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* CueCls = FindObject<UClass>(nullptr, TEXT("/Script/Engine.SoundCue"));
    if (!CueCls) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("SoundCue class not found"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, CueCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("SoundCue"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- audio.create_metasound ------------------------------------------------

FSageToolDispatch::FOutcome AudioCreateMetaSoundImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* MetaCls = FindAudioClass(TEXT("/Script/MetasoundEngine.MetaSoundSource"));
    if (!MetaCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("MetaSoundSource not found — MetaSound plugin required"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, MetaCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  NewObj->GetPathName());
    R->SetStringField(TEXT("class"), TEXT("MetaSoundSource"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterAudioTools(FSageToolDispatch& Dispatch)
{
    auto GT = [](FSageToolDispatch::FOutcome (*Fn)(const TSharedPtr<FJsonObject>&))
    {
        return [Fn](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
        {
            return detail::RunOnGameThread([&]() -> FSageToolDispatch::FOutcome
            {
                return Fn(Args);
            });
        };
    };

    Dispatch.RegisterHandler(TEXT("audio.list"),             GT(&AudioListImpl));
    Dispatch.RegisterHandler(TEXT("audio.play_at_location"), GT(&AudioPlayAtLocationImpl));
    Dispatch.RegisterHandler(TEXT("audio.spawn_ambient"),    GT(&AudioSpawnAmbientImpl));
    Dispatch.RegisterHandler(TEXT("audio.create_cue"),       GT(&AudioCreateCueImpl));
    Dispatch.RegisterHandler(TEXT("audio.create_metasound"), GT(&AudioCreateMetaSoundImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
