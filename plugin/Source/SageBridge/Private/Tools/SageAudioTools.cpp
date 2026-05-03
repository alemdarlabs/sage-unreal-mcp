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

// ---- audio.read_* readers (Phase 4.x — CommonAIExport parity) --------------
//
// Read-only structural dumpers for audio assets. Each verifies the resolved
// asset is an instance of the expected class (looked up dynamically via
// FindObject<UClass> so projects without the AudioModulation plugin don't
// fail to load), then runs the reflection-based property reader (with
// optional instanced-subobject recursion). For SoundClass and SoundSubmix
// we additionally surface `children` as a flat path array — that's the
// hierarchy field CommonAIExport's tree exporter exposes.

UObject* LoadAudioAsset(const FString& Path, const TCHAR* ExpectedClassPath,
                       FString* OutErr)
{
    UClass* Cls = FindAudioClass(ExpectedClassPath);
    if (!Cls)
    {
        if (OutErr) *OutErr = FString::Printf(
            TEXT("class '%s' not loaded — module/plugin missing?"),
            ExpectedClassPath);
        return nullptr;
    }
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    if (!Obj)
    {
        if (OutErr) *OutErr = FString::Printf(
            TEXT("asset not found: %s"), *Path);
        return nullptr;
    }
    if (!Obj->IsA(Cls))
    {
        if (OutErr) *OutErr = FString::Printf(
            TEXT("asset '%s' is %s, not %s"),
            *Path, *Obj->GetClass()->GetName(), *Cls->GetName());
        return nullptr;
    }
    return Obj;
}

bool ReadAudioRecurseFlag(const TSharedPtr<FJsonObject>& Args, int32& OutMaxDepth)
{
    bool bRecurse = false;
    OutMaxDepth = 4;
    if (Args.IsValid())
    {
        Args->TryGetBoolField(TEXT("recurse_instanced"), bRecurse);
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_depth"), N))
        {
            OutMaxDepth = FMath::Clamp(static_cast<int32>(N), 1, 16);
        }
    }
    return bRecurse;
}

TSharedPtr<FJsonObject> DumpAudioAsset(UObject* Obj, bool bRecurse, int32 MaxDepth,
                                        int32* OutCount = nullptr)
{
    detail::FInstancedRecurseCtx Ctx;
    Ctx.MaxDepth = MaxDepth;
    Ctx.Visited.Add(Obj);
    detail::FInstancedRecurseCtx* CtxPtr = bRecurse ? &Ctx : nullptr;

    auto Props = MakeShared<FJsonObject>();
    int32 Count = 0;
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        FProperty* P = *It;
        if (!P) continue;
        if (P->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) continue;
        auto V = detail::GetUPropertyAsJson(Obj, P, CtxPtr);
        if (V.IsValid()) { Props->SetField(P->GetName(), V); ++Count; }
    }
    if (OutCount) *OutCount = Count;
    return Props;
}

// Pull a TArray<UObject*> property as a JSON array of soft paths. Returns
// nullptr if the named property doesn't exist or isn't an array of objects.
TSharedPtr<FJsonValue> ReadObjectArrayPaths(UObject* Obj, const TCHAR* PropName)
{
    if (!Obj) return nullptr;
    FProperty* P = Obj->GetClass()->FindPropertyByName(FName(PropName));
    FArrayProperty* Arr = CastField<FArrayProperty>(P);
    if (!Arr) return nullptr;
    FObjectProperty* Inner = CastField<FObjectProperty>(Arr->Inner);
    if (!Inner) return nullptr;
    FScriptArrayHelper Helper(Arr, Arr->ContainerPtrToValuePtr<void>(Obj));
    TArray<TSharedPtr<FJsonValue>> Out;
    for (int32 i = 0; i < Helper.Num(); ++i)
    {
        UObject* Child = Inner->GetObjectPropertyValue(Helper.GetRawPtr(i));
        if (Child)
        {
            Out.Add(MakeShared<FJsonValueString>(
                FSoftObjectPath(Child).ToString()));
        }
        else
        {
            Out.Add(MakeShared<FJsonValueNull>());
        }
    }
    return MakeShared<FJsonValueArray>(Out);
}

FSageToolDispatch::FOutcome AudioReadGenericImpl(
    const TSharedPtr<FJsonObject>& Args, const TCHAR* ExpectedClassPath,
    const TCHAR* HierarchyPropName /* "ChildClasses"/"ChildSubmixes" or nullptr */)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path'"));

    FString Err;
    UObject* Obj = LoadAudioAsset(Path, ExpectedClassPath, &Err);
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    int32 MaxDepth = 4;
    const bool bRecurse = ReadAudioRecurseFlag(Args, MaxDepth);
    int32 Count = 0;
    auto Props = DumpAudioAsset(Obj, bRecurse, MaxDepth, &Count);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       Obj->GetPathName());
    R->SetStringField(TEXT("class"),      Obj->GetClass()->GetName());
    R->SetObjectField(TEXT("properties"), Props);
    R->SetNumberField(TEXT("count"),      Count);
    if (HierarchyPropName)
    {
        if (auto Children = ReadObjectArrayPaths(Obj, HierarchyPropName))
        {
            R->SetField(TEXT("children"), Children);
        }
    }
    if (bRecurse)
    {
        R->SetBoolField  (TEXT("recurse_instanced"), true);
        R->SetNumberField(TEXT("max_depth"),         MaxDepth);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AudioReadSoundClassImpl(const TSharedPtr<FJsonObject>& Args)
{
    return AudioReadGenericImpl(Args,
        TEXT("/Script/Engine.SoundClass"), TEXT("ChildClasses"));
}

FSageToolDispatch::FOutcome AudioReadSoundSubmixImpl(const TSharedPtr<FJsonObject>& Args)
{
    // SoundSubmix's child array is `ChildSubmixes`. SoundSubmixBase is the
    // base type (Engine has both `USoundSubmix` and the AudioMixer plugin
    // adds `USoundfieldSubmix` / `UEndpointSubmix` — SoundSubmixBase covers
    // them all and lives in /Script/Engine.
    return AudioReadGenericImpl(Args,
        TEXT("/Script/Engine.SoundSubmixBase"), TEXT("ChildSubmixes"));
}

FSageToolDispatch::FOutcome AudioReadSoundConcurrencyImpl(const TSharedPtr<FJsonObject>& Args)
{
    return AudioReadGenericImpl(Args,
        TEXT("/Script/Engine.SoundConcurrency"), nullptr);
}

FSageToolDispatch::FOutcome AudioReadSoundAttenuationImpl(const TSharedPtr<FJsonObject>& Args)
{
    return AudioReadGenericImpl(Args,
        TEXT("/Script/Engine.SoundAttenuation"), nullptr);
}

FSageToolDispatch::FOutcome AudioReadControlBusImpl(const TSharedPtr<FJsonObject>& Args)
{
    return AudioReadGenericImpl(Args,
        TEXT("/Script/AudioModulation.SoundControlBus"), nullptr);
}

FSageToolDispatch::FOutcome AudioReadControlBusMixImpl(const TSharedPtr<FJsonObject>& Args)
{
    return AudioReadGenericImpl(Args,
        TEXT("/Script/AudioModulation.SoundControlBusMix"), nullptr);
}

FSageToolDispatch::FOutcome AudioReadModulationPatchImpl(const TSharedPtr<FJsonObject>& Args)
{
    return AudioReadGenericImpl(Args,
        TEXT("/Script/AudioModulation.SoundModulationPatch"), nullptr);
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

    // Read-only audio asset dumpers (Phase 4.x — CommonAIExport parity)
    Dispatch.RegisterHandler(TEXT("audio.read_sound_class"),
                             GT(&AudioReadSoundClassImpl));
    Dispatch.RegisterHandler(TEXT("audio.read_sound_submix"),
                             GT(&AudioReadSoundSubmixImpl));
    Dispatch.RegisterHandler(TEXT("audio.read_sound_concurrency"),
                             GT(&AudioReadSoundConcurrencyImpl));
    Dispatch.RegisterHandler(TEXT("audio.read_sound_attenuation"),
                             GT(&AudioReadSoundAttenuationImpl));
    Dispatch.RegisterHandler(TEXT("audio.read_control_bus"),
                             GT(&AudioReadControlBusImpl));
    Dispatch.RegisterHandler(TEXT("audio.read_control_bus_mix"),
                             GT(&AudioReadControlBusMixImpl));
    Dispatch.RegisterHandler(TEXT("audio.read_modulation_patch"),
                             GT(&AudioReadModulationPatchImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
