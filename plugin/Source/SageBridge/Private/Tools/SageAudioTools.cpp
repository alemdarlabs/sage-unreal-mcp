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
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Subsystems/EditorAssetSubsystem.h"
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

static constexpr int32 kAudioUnsupportedCode = -32005;

FSageToolDispatch::FOutcome AudioUnsupported(const FString& Tool, const FString& Reason)
{
    return FSageToolDispatch::FOutcome::MakeError(kAudioUnsupportedCode,
        FString::Printf(TEXT("%s is not exposed as a safe public audio mutation yet: %s"),
                        *Tool, *Reason));
}

FString FirstAudioStringArg(const TSharedPtr<FJsonObject>& Args,
                            std::initializer_list<const TCHAR*> Fields)
{
    if (!Args.IsValid()) return FString();
    FString Value;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetStringField(Field, Value) && !Value.IsEmpty())
        {
            return Value;
        }
    }
    return FString();
}

bool FirstAudioBoolArg(const TSharedPtr<FJsonObject>& Args,
                       std::initializer_list<const TCHAR*> Fields,
                       bool DefaultValue = false)
{
    if (!Args.IsValid()) return DefaultValue;
    bool Value = DefaultValue;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetBoolField(Field, Value))
        {
            return Value;
        }
    }
    return DefaultValue;
}

int32 FirstAudioIntArg(const TSharedPtr<FJsonObject>& Args,
                       std::initializer_list<const TCHAR*> Fields,
                       int32 DefaultValue = INDEX_NONE)
{
    if (!Args.IsValid()) return DefaultValue;
    int32 Value = DefaultValue;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetNumberField(Field, Value))
        {
            return Value;
        }
    }
    return DefaultValue;
}

TArray<FString> AudioStringArrayArg(const TSharedPtr<FJsonObject>& Args,
                                    std::initializer_list<const TCHAR*> Fields)
{
    TArray<FString> Out;
    if (!Args.IsValid()) return Out;
    for (const TCHAR* Field : Fields)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Args->TryGetArrayField(Field, Values) || !Values) continue;
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
            {
                Out.Add(Text);
            }
        }
        if (Out.Num() > 0) return Out;
    }
    FString Single = FirstAudioStringArg(Args, {TEXT("path"), TEXT("asset")});
    if (!Single.IsEmpty())
    {
        Out.Add(Single);
    }
    return Out;
}

UObject* LoadAudioObjectPath(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    if (UObject* Existing = FindObject<UObject>(nullptr, *Path))
    {
        return Existing;
    }
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj)
    {
        Obj = Soft.TryLoad();
    }
    return Obj;
}

const TCHAR* CreateAudioClassForTool(const FString& Tool)
{
    if (Tool == TEXT("create_sound_attenuation")) return TEXT("/Script/Engine.SoundAttenuation");
    if (Tool == TEXT("create_sound_class"))       return TEXT("/Script/Engine.SoundClass");
    if (Tool == TEXT("create_sound_mix"))         return TEXT("/Script/Engine.SoundMix");
    if (Tool == TEXT("create_sound_concurrency")) return TEXT("/Script/Engine.SoundConcurrency");
    if (Tool == TEXT("create_sound_submix"))      return TEXT("/Script/Engine.SoundSubmix");
    if (Tool == TEXT("create_metasound_source"))  return TEXT("/Script/MetasoundEngine.MetaSoundSource");
    if (Tool == TEXT("create_metasound_patch"))   return TEXT("/Script/MetasoundEngine.MetaSoundPatch");
    return nullptr;
}

const TCHAR* SetAudioClassForTool(const FString& Tool)
{
    if (Tool == TEXT("set_attenuation_settings"))     return TEXT("/Script/Engine.SoundAttenuation");
    if (Tool == TEXT("set_sound_class_properties"))   return TEXT("/Script/Engine.SoundClass");
    if (Tool == TEXT("set_sound_mix_settings"))       return TEXT("/Script/Engine.SoundMix");
    if (Tool == TEXT("set_concurrency_settings"))     return TEXT("/Script/Engine.SoundConcurrency");
    if (Tool == TEXT("set_submix_properties"))        return TEXT("/Script/Engine.SoundSubmixBase");
    return nullptr;
}

void SaveAudioAssetIfRequested(UObject* Obj, const TSharedPtr<FJsonObject>& Args, const TSharedRef<FJsonObject>& R)
{
    if (!Obj || !FirstAudioBoolArg(Args, {TEXT("save")}, false) || !GEditor)
    {
        return;
    }
    if (UEditorAssetSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
    {
        R->SetBoolField(TEXT("saved"), Sub->SaveLoadedAsset(Obj));
    }
}

FSageToolDispatch::FOutcome ApplyAudioProperties(UObject* Obj,
                                                 const TSharedPtr<FJsonObject>& Properties,
                                                 const TSharedPtr<FJsonObject>& Args,
                                                 const FString& TxName)
{
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("target object is null"));
    if (!Properties.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'properties' or 'settings' object"));
    }
    if (FSageToolDispatch::FOutcome Reject; detail::RejectIfPie(Reject)) return Reject;

    FScopedTransaction Tx(FText::FromString(TxName));
    Obj->Modify();
    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<FString> Failures;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
    {
        FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*Pair.Key));
        if (!Prop)
        {
            Failures.Add(FString::Printf(TEXT("%s: property not found"), *Pair.Key));
            continue;
        }
        if (!detail::SetUPropertyFromJson(Obj, Prop, Pair.Value))
        {
            Failures.Add(FString::Printf(TEXT("%s: value rejected for %s"),
                *Pair.Key, *Prop->GetClass()->GetName()));
            continue;
        }
        Applied.Add(MakeShared<FJsonValueString>(Pair.Key));
    }

    if (Failures.Num() > 0)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Join(Failures, TEXT("; ")));
    }

    Obj->MarkPackageDirty();
    Obj->PostEditChange();
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
    R->SetArrayField(TEXT("applied"), Applied);
    SaveAudioAssetIfRequested(Obj, Args, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

TSharedPtr<FJsonObject> AudioSettingsObject(const TSharedPtr<FJsonObject>& Args)
{
    if (!Args.IsValid()) return nullptr;
    const TSharedPtr<FJsonObject>* Obj = nullptr;
    if (Args->TryGetObjectField(TEXT("properties"), Obj) && Obj && Obj->IsValid())
    {
        return *Obj;
    }
    if (Args->TryGetObjectField(TEXT("settings"), Obj) && Obj && Obj->IsValid())
    {
        return *Obj;
    }
    return nullptr;
}

FSageToolDispatch::FOutcome CreateAudioAssetParityImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    const TCHAR* ClassPath = CreateAudioClassForTool(Tool);
    if (!ClassPath) return AudioUnsupported(Tool, TEXT("no audio class mapping"));
    if (FSageToolDispatch::FOutcome Reject; detail::RejectIfPie(Reject)) return Reject;

    FString Path = FirstAudioStringArg(Args, {TEXT("path"), TEXT("asset")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UClass* Cls = FindAudioClass(ClassPath);
    if (!Cls)
    {
        return AudioUnsupported(Tool, FString::Printf(TEXT("class %s is not loaded"), ClassPath));
    }
    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid asset path"));
    }
    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, Cls, nullptr);
    if (!NewObj)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));
    }
    if (TSharedPtr<FJsonObject> Props = AudioSettingsObject(Args))
    {
        FSageToolDispatch::FOutcome Applied = ApplyAudioProperties(NewObj, Props, Args, FString::Printf(TEXT("Sage: %s"), *Tool));
        if (!Applied.bSuccess) return Applied;
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), NewObj->GetPathName());
    R->SetStringField(TEXT("class"), NewObj->GetClass()->GetName());
    SaveAudioAssetIfRequested(NewObj, Args, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetAudioAssetParityImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    const TCHAR* ClassPath = SetAudioClassForTool(Tool);
    if (!ClassPath) return AudioUnsupported(Tool, TEXT("no audio class mapping"));
    FString Path = FirstAudioStringArg(Args, {TEXT("path"), TEXT("asset")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    FString Err;
    UObject* Obj = LoadAudioAsset(Path, ClassPath, &Err);
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);
    return ApplyAudioProperties(Obj, AudioSettingsObject(Args), Args,
        FString::Printf(TEXT("Sage: %s"), *Tool));
}

TArray<FAssetData> QueryAudioAssets(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), SearchPath);
    const FString TypeFilter = FirstAudioStringArg(Args, {TEXT("type"), TEXT("class")});

    const TCHAR* ClassPaths[] = {
        TEXT("/Script/Engine.SoundWave"),
        TEXT("/Script/Engine.SoundCue"),
        TEXT("/Script/Engine.SoundClass"),
        TEXT("/Script/Engine.SoundMix"),
        TEXT("/Script/Engine.SoundConcurrency"),
        TEXT("/Script/Engine.SoundAttenuation"),
        TEXT("/Script/Engine.SoundSubmix"),
        TEXT("/Script/MetasoundEngine.MetaSoundSource"),
        TEXT("/Script/MetasoundEngine.MetaSoundPatch"),
    };

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;
    for (const TCHAR* ClassPath : ClassPaths)
    {
        UClass* Cls = FindAudioClass(ClassPath);
        if (!Cls) continue;
        if (TypeFilter.IsEmpty() ||
            Cls->GetName().Equals(TypeFilter, ESearchCase::IgnoreCase) ||
            FString(ClassPath).Contains(TypeFilter))
        {
            Filter.ClassPaths.Add(Cls->GetClassPathName());
        }
    }
    if (Filter.ClassPaths.IsEmpty())
    {
        return {};
    }
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets;
    ARM.Get().GetAssets(Filter, Assets);
    return Assets;
}

FSageToolDispatch::FOutcome SearchAudioAssetsImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Query = FirstAudioStringArg(Args, {TEXT("query"), TEXT("name")});
    const int32 Max = FMath::Max(0, FirstAudioIntArg(Args, {TEXT("max"), TEXT("max_results")}, 250));
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FAssetData& D : QueryAudioAssets(Args))
    {
        if (!Query.IsEmpty() &&
            !D.AssetName.ToString().Contains(Query, ESearchCase::IgnoreCase) &&
            !D.GetSoftObjectPath().ToString().Contains(Query, ESearchCase::IgnoreCase))
        {
            continue;
        }
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), D.AssetName.ToString());
        J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
        J->SetStringField(TEXT("class"), D.AssetClassPath.GetAssetName().ToString());
        Items.Add(MakeShared<FJsonValueObject>(J));
        if (Max > 0 && Items.Num() >= Max) break;
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("assets"), Items);
    R->SetNumberField(TEXT("count"), Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool AudioObjectPropertyIsNull(UObject* Obj, const TCHAR* PropName)
{
    if (!Obj) return true;
    FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(PropName));
    FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop);
    if (!ObjProp) return true;
    UObject* Value = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Obj));
    return Value == nullptr;
}

FSageToolDispatch::FOutcome FindAudioHealthImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FAssetData& D : QueryAudioAssets(Args))
    {
        bool bInclude = false;
        FString Reason;
        if (Tool == TEXT("find_unused_audio"))
        {
            const FString PackageName = FPackageName::ObjectPathToPackageName(D.GetSoftObjectPath().ToString());
            TArray<FName> Referencers;
            ARM.Get().GetReferencers(FName(*PackageName), Referencers);
            bInclude = Referencers.Num() == 0;
            Reason = TEXT("no package referencers");
        }
        else
        {
            UObject* Obj = D.GetAsset();
            USoundBase* Sound = Cast<USoundBase>(Obj);
            if (!Sound) continue;
            if (Tool == TEXT("find_sounds_without_class"))
            {
                bInclude = AudioObjectPropertyIsNull(Sound, TEXT("SoundClassObject"));
                Reason = TEXT("SoundClassObject is null");
            }
            else if (Tool == TEXT("find_unattenuated_sounds"))
            {
                bInclude = AudioObjectPropertyIsNull(Sound, TEXT("AttenuationSettings"));
                Reason = TEXT("AttenuationSettings is null");
            }
        }
        if (bInclude)
        {
            TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("path"), D.GetSoftObjectPath().ToString());
            J->SetStringField(TEXT("class"), D.AssetClassPath.GetAssetName().ToString());
            J->SetStringField(TEXT("reason"), Reason);
            Items.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("assets"), Items);
    R->SetNumberField(TEXT("count"), Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AudioReferencesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path = FirstAudioStringArg(Args, {TEXT("path"), TEXT("asset")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    const FString PackageName = FPackageName::ObjectPathToPackageName(Path);
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FName> Referencers;
    ARM.Get().GetReferencers(FName(*PackageName), Referencers);
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FName& Ref : Referencers)
    {
        Items.Add(MakeShared<FJsonValueString>(Ref.ToString()));
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("package"), PackageName);
    R->SetArrayField(TEXT("referencers"), Items);
    R->SetNumberField(TEXT("count"), Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AudioStatsImpl(const TSharedPtr<FJsonObject>& Args)
{
    TMap<FString, int32> Counts;
    double TotalDuration = 0.0;
    int32 DurationAssets = 0;
    for (const FAssetData& D : QueryAudioAssets(Args))
    {
        Counts.FindOrAdd(D.AssetClassPath.GetAssetName().ToString())++;
        if (USoundBase* Sound = Cast<USoundBase>(D.GetAsset()))
        {
            const float Duration = Sound->GetDuration();
            if (Duration > 0.f)
            {
                TotalDuration += Duration;
                ++DurationAssets;
            }
        }
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> ByClass = MakeShared<FJsonObject>();
    int32 Total = 0;
    for (const TPair<FString, int32>& Pair : Counts)
    {
        ByClass->SetNumberField(Pair.Key, Pair.Value);
        Total += Pair.Value;
    }
    R->SetObjectField(TEXT("by_class"), ByClass);
    R->SetNumberField(TEXT("total_assets"), Total);
    R->SetNumberField(TEXT("duration_assets"), DurationAssets);
    R->SetNumberField(TEXT("total_duration_seconds"), TotalDuration);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

TSharedPtr<FJsonObject> BuildBatchProperties(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    TSharedPtr<FJsonObject> Props = AudioSettingsObject(Args);
    if (Props.IsValid()) return Props;
    Props = MakeShared<FJsonObject>();
    if (Tool == TEXT("batch_assign_sound_class"))
    {
        Props->SetField(TEXT("SoundClassObject"),
            MakeShared<FJsonValueString>(FirstAudioStringArg(Args, {TEXT("sound_class"), TEXT("class_path"), TEXT("value")})));
    }
    else if (Tool == TEXT("batch_assign_attenuation"))
    {
        Props->SetField(TEXT("AttenuationSettings"),
            MakeShared<FJsonValueString>(FirstAudioStringArg(Args, {TEXT("attenuation"), TEXT("attenuation_path"), TEXT("value")})));
    }
    else if (Tool == TEXT("batch_set_submix"))
    {
        Props->SetField(TEXT("SoundSubmixObject"),
            MakeShared<FJsonValueString>(FirstAudioStringArg(Args, {TEXT("submix"), TEXT("submix_path"), TEXT("value")})));
    }
    else if (Tool == TEXT("batch_set_concurrency"))
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        const FString Concurrency = FirstAudioStringArg(Args, {TEXT("concurrency"), TEXT("concurrency_path"), TEXT("value")});
        if (!Concurrency.IsEmpty()) Values.Add(MakeShared<FJsonValueString>(Concurrency));
        Props->SetArrayField(TEXT("ConcurrencySet"), Values);
    }
    else if (Tool == TEXT("batch_set_looping"))
    {
        Props->SetField(TEXT("bLooping"), MakeShared<FJsonValueBoolean>(FirstAudioBoolArg(Args, {TEXT("looping"), TEXT("value")}, true)));
    }
    else if (Tool == TEXT("batch_set_virtualization"))
    {
        Props->SetField(TEXT("VirtualizationMode"),
            MakeShared<FJsonValueString>(FirstAudioStringArg(Args, {TEXT("virtualization"), TEXT("virtualization_mode"), TEXT("value")})));
    }
    else if (Tool == TEXT("batch_set_compression"))
    {
        const int32 Quality = FirstAudioIntArg(Args, {TEXT("compression_quality"), TEXT("quality")}, INDEX_NONE);
        if (Quality != INDEX_NONE)
        {
            Props->SetField(TEXT("CompressionQuality"), MakeShared<FJsonValueNumber>(Quality));
        }
        const FString Type = FirstAudioStringArg(Args, {TEXT("compression_type"), TEXT("sound_asset_compression_type")});
        if (!Type.IsEmpty())
        {
            Props->SetField(TEXT("SoundAssetCompressionType"), MakeShared<FJsonValueString>(Type));
        }
    }
    return Props;
}

FSageToolDispatch::FOutcome BatchAudioPropertiesImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("batch_rename_audio"))
    {
        return AudioUnsupported(Tool, TEXT("use existing asset.bulk_rename for atomic rename; this alias is not wired to avoid duplicate semantics"));
    }
    const TArray<FString> Paths = AudioStringArrayArg(Args, {TEXT("paths"), TEXT("assets")});
    if (Paths.Num() == 0) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'paths'"));
    TSharedPtr<FJsonObject> Props = BuildBatchProperties(Tool, Args);
    if (!Props.IsValid() || Props->Values.Num() == 0)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("no properties to apply"));
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Changed = 0;
    for (const FString& Path : Paths)
    {
        UObject* Obj = LoadAudioObjectPath(Path);
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), Path);
        if (!Obj)
        {
            Row->SetBoolField(TEXT("ok"), false);
            Row->SetStringField(TEXT("reason"), TEXT("asset not found"));
            Results.Add(MakeShared<FJsonValueObject>(Row));
            continue;
        }
        FSageToolDispatch::FOutcome Applied = ApplyAudioProperties(Obj, Props, Args,
            FString::Printf(TEXT("Sage: %s"), *Tool));
        Row->SetBoolField(TEXT("ok"), Applied.bSuccess);
        if (Applied.bSuccess)
        {
            ++Changed;
        }
        else if (Applied.Error.IsValid())
        {
            Row->SetStringField(TEXT("reason"), Applied.Error->GetStringField(TEXT("message")));
        }
        Results.Add(MakeShared<FJsonValueObject>(Row));
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("changed"), Changed);
    R->SetNumberField(TEXT("count"), Results.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SoundCueGraphImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    FString Path = FirstAudioStringArg(Args, {TEXT("path"), TEXT("cue"), TEXT("asset")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    FString Err;
    UObject* Obj = LoadAudioAsset(Path, TEXT("/Script/Engine.SoundCue"), &Err);
    USoundCue* Cue = Cast<USoundCue>(Obj);
    if (!Cue) return FSageToolDispatch::FOutcome::MakeError(-32602, Err);

    FArrayProperty* AllNodesProp = CastField<FArrayProperty>(Cue->GetClass()->FindPropertyByName(TEXT("AllNodes")));
    FObjectPropertyBase* InnerObj = AllNodesProp ? CastField<FObjectPropertyBase>(AllNodesProp->Inner) : nullptr;
    TArray<TSharedPtr<FJsonValue>> Nodes;
    if (AllNodesProp && InnerObj)
    {
        FScriptArrayHelper Helper(AllNodesProp, AllNodesProp->ContainerPtrToValuePtr<void>(Cue));
        for (int32 I = 0; I < Helper.Num(); ++I)
        {
            UObject* Node = InnerObj->GetObjectPropertyValue(Helper.GetRawPtr(I));
            if (!Node) continue;
            TSharedRef<FJsonObject> N = MakeShared<FJsonObject>();
            N->SetNumberField(TEXT("index"), I);
            N->SetStringField(TEXT("path"), Node->GetPathName());
            N->SetStringField(TEXT("class"), Node->GetClass()->GetName());
            int32 PropCount = 0;
            N->SetObjectField(TEXT("properties"), DumpAudioAsset(Node, false, 1, &PropCount));
            N->SetNumberField(TEXT("property_count"), PropCount);
            if (auto Children = ReadObjectArrayPaths(Node, TEXT("ChildNodes")))
            {
                N->SetField(TEXT("children"), Children);
            }
            Nodes.Add(MakeShared<FJsonValueObject>(N));
        }
    }

    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Cue->GetPathName());
    R->SetStringField(TEXT("tool"), Tool);
    R->SetArrayField(TEXT("nodes"), Nodes);
    R->SetNumberField(TEXT("node_count"), Nodes.Num());
    FObjectPropertyBase* FirstProp = CastField<FObjectPropertyBase>(Cue->GetClass()->FindPropertyByName(TEXT("FirstNode")));
    if (FirstProp)
    {
        UObject* First = FirstProp->GetObjectPropertyValue(FirstProp->ContainerPtrToValuePtr<void>(Cue));
        R->SetStringField(TEXT("first_node"), First ? First->GetPathName() : FString());
    }
    if (Tool == TEXT("validate_sound_cue"))
    {
        TArray<TSharedPtr<FJsonValue>> Issues;
        if (Nodes.Num() == 0)
        {
            TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("code"), TEXT("no_nodes"));
            Issue->SetStringField(TEXT("detail"), TEXT("SoundCue has no AllNodes entries"));
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
        }
        R->SetArrayField(TEXT("issues"), Issues);
        R->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ListSoundCueNodeTypesImpl(const TSharedPtr<FJsonObject>& Args)
{
    UClass* Base = FindAudioClass(TEXT("/Script/Engine.SoundNode"));
    if (!Base) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("SoundNode base class not loaded"));
    TArray<TSharedPtr<FJsonValue>> Items;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Cls = *It;
        if (!Cls || !Cls->IsChildOf(Base) || Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            continue;
        }
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), Cls->GetName());
        J->SetStringField(TEXT("path"), Cls->GetPathName());
        Items.Add(MakeShared<FJsonValueObject>(J));
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("node_types"), Items);
    R->SetNumberField(TEXT("count"), Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SoundDurationImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path = FirstAudioStringArg(Args, {TEXT("path"), TEXT("sound"), TEXT("cue"), TEXT("asset")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    USoundBase* Sound = Cast<USoundBase>(LoadAudioObjectPath(Path));
    if (!Sound) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("path is not a USoundBase"));
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Sound->GetPathName());
    R->SetStringField(TEXT("class"), Sound->GetClass()->GetName());
    R->SetNumberField(TEXT("duration"), Sound->GetDuration());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AudioPreviewParityImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("preview_sound"))
    {
        return AudioPlayAtLocationImpl(Args);
    }
    if (!GEditor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("GEditor unavailable"));
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    GEditor->Exec(World, TEXT("au.StopSounds"), *GLog);
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("triggered"), true);
    R->SetStringField(TEXT("command"), TEXT("au.StopSounds"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome MetaSoundReadImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    FString Path = FirstAudioStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("metasound")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UObject* Obj = LoadAudioObjectPath(Path);
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));
    if (!Obj->GetClass()->GetPathName().Contains(TEXT("MetaSound")))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset is not a MetaSound asset"));
    }
    int32 Count = 0;
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetStringField(TEXT("class"), Obj->GetClass()->GetName());
    R->SetStringField(TEXT("tool"), Tool);
    R->SetObjectField(TEXT("properties"), DumpAudioAsset(Obj, false, 1, &Count));
    R->SetNumberField(TEXT("property_count"), Count);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool IsSoundCueMutationTool(const FString& Tool)
{
    static const TSet<FString> Names = {
        TEXT("add_sound_cue_node"),
        TEXT("remove_sound_cue_node"),
        TEXT("connect_sound_cue_nodes"),
        TEXT("set_sound_cue_first_node"),
        TEXT("set_sound_cue_node_property"),
        TEXT("build_sound_cue_from_spec"),
        TEXT("create_random_sound_cue"),
        TEXT("create_layered_sound_cue"),
        TEXT("create_looping_ambient_cue"),
        TEXT("create_distance_crossfade_cue"),
        TEXT("create_switch_sound_cue"),
    };
    return Names.Contains(Tool);
}

bool IsMetaSoundMutationTool(const FString& Tool)
{
    static const TSet<FString> Names = {
        TEXT("add_metasound_node"),
        TEXT("remove_metasound_node"),
        TEXT("connect_metasound_nodes"),
        TEXT("disconnect_metasound_nodes"),
        TEXT("add_metasound_input"),
        TEXT("add_metasound_output"),
        TEXT("set_metasound_input_default"),
        TEXT("add_metasound_interface"),
        TEXT("build_metasound_from_spec"),
        TEXT("create_metasound_preset"),
        TEXT("create_oneshot_sfx"),
        TEXT("create_looping_ambient_metasound"),
        TEXT("create_synthesized_tone"),
        TEXT("create_interactive_metasound"),
        TEXT("add_metasound_variable"),
        TEXT("set_metasound_node_location"),
    };
    return Names.Contains(Tool);
}

FSageToolDispatch::FOutcome DispatchAudioParityTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (CreateAudioClassForTool(Tool)) return CreateAudioAssetParityImpl(Tool, Args);
    if (SetAudioClassForTool(Tool)) return SetAudioAssetParityImpl(Tool, Args);
    if (Tool == TEXT("search_audio_assets")) return SearchAudioAssetsImpl(Args);
    if (Tool == TEXT("find_audio_references")) return AudioReferencesImpl(Args);
    if (Tool == TEXT("find_unused_audio") ||
        Tool == TEXT("find_sounds_without_class") ||
        Tool == TEXT("find_unattenuated_sounds"))
    {
        return FindAudioHealthImpl(Tool, Args);
    }
    if (Tool == TEXT("get_audio_stats")) return AudioStatsImpl(Args);
    if (Tool.StartsWith(TEXT("batch_")) || Tool == TEXT("apply_audio_template"))
    {
        return BatchAudioPropertiesImpl(Tool, Args);
    }
    if (Tool == TEXT("get_sound_cue_graph") || Tool == TEXT("validate_sound_cue"))
    {
        return SoundCueGraphImpl(Tool, Args);
    }
    if (Tool == TEXT("list_sound_cue_node_types")) return ListSoundCueNodeTypesImpl(Args);
    if (Tool == TEXT("preview_sound") || Tool == TEXT("stop_preview")) return AudioPreviewParityImpl(Tool, Args);
    if (Tool == TEXT("get_sound_cue_duration")) return SoundDurationImpl(Args);
    if (Tool == TEXT("get_metasound_graph")) return MetaSoundReadImpl(Tool, Args);
    if (Tool.StartsWith(TEXT("list_metasound_")) ||
        Tool.StartsWith(TEXT("get_metasound_")) ||
        Tool.StartsWith(TEXT("find_metasound_")) ||
        Tool == TEXT("list_available_metasound_nodes"))
    {
        return AudioUnsupported(Tool, TEXT("MetaSound graph registry/editor APIs are optional and not safely mapped in this build"));
    }
    if (IsSoundCueMutationTool(Tool))
    {
        return AudioUnsupported(Tool, TEXT("SoundCue graph mutation requires SoundCue editor graph wiring; read/validate/list-node-types are implemented"));
    }
    if (IsMetaSoundMutationTool(Tool))
    {
        return AudioUnsupported(Tool, TEXT("MetaSound graph mutation requires MetaSoundEditor builder APIs; read/create-source/create-patch are implemented"));
    }
    if (Tool == TEXT("bind_sound_to_perception") ||
        Tool == TEXT("unbind_sound_from_perception") ||
        Tool == TEXT("get_sound_perception_binding") ||
        Tool == TEXT("list_perception_bound_sounds"))
    {
        return AudioUnsupported(Tool, TEXT("audio/perception binding needs a project-level data model; no implicit asset metadata is mutated"));
    }
    return AudioUnsupported(Tool, TEXT("no dispatch mapping"));
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

    auto RegisterParity = [&Dispatch](const TCHAR* ToolName)
    {
        Dispatch.RegisterHandler(ToolName,
            [Name = FString(ToolName)](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
            {
                return detail::RunOnGameThread([&]() -> FSageToolDispatch::FOutcome
                {
                    return DispatchAudioParityTool(Name, Args);
                });
            });
    };

    static const TCHAR* kAudioParityTools[] = {
        TEXT("create_sound_attenuation"),
        TEXT("set_attenuation_settings"),
        TEXT("create_sound_class"),
        TEXT("set_sound_class_properties"),
        TEXT("create_sound_mix"),
        TEXT("set_sound_mix_settings"),
        TEXT("create_sound_concurrency"),
        TEXT("set_concurrency_settings"),
        TEXT("create_sound_submix"),
        TEXT("set_submix_properties"),
        TEXT("search_audio_assets"),
        TEXT("find_audio_references"),
        TEXT("find_unused_audio"),
        TEXT("find_sounds_without_class"),
        TEXT("find_unattenuated_sounds"),
        TEXT("get_audio_stats"),
        TEXT("batch_assign_sound_class"),
        TEXT("batch_assign_attenuation"),
        TEXT("batch_set_compression"),
        TEXT("batch_set_submix"),
        TEXT("batch_set_concurrency"),
        TEXT("batch_set_looping"),
        TEXT("batch_set_virtualization"),
        TEXT("batch_rename_audio"),
        TEXT("batch_set_sound_wave_properties"),
        TEXT("apply_audio_template"),
        TEXT("get_sound_cue_graph"),
        TEXT("add_sound_cue_node"),
        TEXT("remove_sound_cue_node"),
        TEXT("connect_sound_cue_nodes"),
        TEXT("set_sound_cue_first_node"),
        TEXT("set_sound_cue_node_property"),
        TEXT("list_sound_cue_node_types"),
        TEXT("validate_sound_cue"),
        TEXT("build_sound_cue_from_spec"),
        TEXT("create_random_sound_cue"),
        TEXT("create_layered_sound_cue"),
        TEXT("create_looping_ambient_cue"),
        TEXT("create_distance_crossfade_cue"),
        TEXT("create_switch_sound_cue"),
        TEXT("preview_sound"),
        TEXT("stop_preview"),
        TEXT("get_sound_cue_duration"),
        TEXT("create_metasound_source"),
        TEXT("create_metasound_patch"),
        TEXT("add_metasound_node"),
        TEXT("remove_metasound_node"),
        TEXT("connect_metasound_nodes"),
        TEXT("disconnect_metasound_nodes"),
        TEXT("add_metasound_input"),
        TEXT("add_metasound_output"),
        TEXT("set_metasound_input_default"),
        TEXT("add_metasound_interface"),
        TEXT("build_metasound_from_spec"),
        TEXT("get_metasound_graph"),
        TEXT("list_metasound_connections"),
        TEXT("list_available_metasound_nodes"),
        TEXT("get_metasound_node_info"),
        TEXT("find_metasound_node_inputs"),
        TEXT("find_metasound_node_outputs"),
        TEXT("get_metasound_input_names"),
        TEXT("create_metasound_preset"),
        TEXT("create_oneshot_sfx"),
        TEXT("create_looping_ambient_metasound"),
        TEXT("create_synthesized_tone"),
        TEXT("create_interactive_metasound"),
        TEXT("add_metasound_variable"),
        TEXT("set_metasound_node_location"),
        TEXT("bind_sound_to_perception"),
        TEXT("unbind_sound_from_perception"),
        TEXT("get_sound_perception_binding"),
        TEXT("list_perception_bound_sounds"),
    };

    for (const TCHAR* ToolName : kAudioParityTools)
    {
        RegisterParity(ToolName);
    }
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
