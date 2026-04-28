#include "Tools/SageLandscapeTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "SageLandscape"

namespace sage::tools
{
namespace
{

static UClass* FindLandscapeClass(const TCHAR* Name)
{
    UClass* Cls = FindObject<UClass>(nullptr, Name);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, Name);
    return Cls;
}

static AActor* FindFirstLandscape(UWorld* World)
{
    UClass* LsCls = FindLandscapeClass(TEXT("/Script/Landscape.Landscape"));
    if (!LsCls || !World) return nullptr;
    for (TActorIterator<AActor> It(World, LsCls); It; ++It)
        return *It;
    return nullptr;
}

// ---- landscape.get_info ----------------------------------------------------

FSageToolDispatch::FOutcome LandscapeGetInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    AActor* LsActor = FindFirstLandscape(World);
    auto R = MakeShared<FJsonObject>();
    if (LsActor)
    {
        R->SetStringField(TEXT("actor_id"), LsActor->GetPathName());
        R->SetStringField(TEXT("name"),     LsActor->GetActorLabel());
        FVector Loc = LsActor->GetActorLocation();
        R->SetArrayField(TEXT("location"), {
            MakeShared<FJsonValueNumber>(Loc.X),
            MakeShared<FJsonValueNumber>(Loc.Y),
            MakeShared<FJsonValueNumber>(Loc.Z)
        });

        // Reflect key landscape properties
        TArray<TSharedPtr<FJsonValue>> Props;
        for (TFieldIterator<FProperty> It(LsActor->GetClass()); It; ++It)
        {
            if (It->GetName().Contains(TEXT("Section")) ||
                It->GetName().Contains(TEXT("Component")) ||
                It->GetName().Contains(TEXT("Scale")))
            {
                auto J = MakeShared<FJsonObject>();
                J->SetStringField(TEXT("name"), It->GetName());
                TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(LsActor, *It);
                if (Val) J->SetField(TEXT("value"), Val);
                Props.Add(MakeShared<FJsonValueObject>(J));
            }
        }
        R->SetArrayField(TEXT("properties"), Props);
    }
    else
    {
        R->SetBoolField  (TEXT("landscape_found"), false);
        R->SetStringField(TEXT("note"), TEXT("no Landscape actor in current world"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- landscape.list_layers -------------------------------------------------

FSageToolDispatch::FOutcome LandscapeListLayersImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    AActor* LsActor = FindFirstLandscape(World);

    TArray<TSharedPtr<FJsonValue>> Layers;
    if (LsActor)
    {
        // Reflect LayerInfoObjs property
        FProperty* Prop = FindFProperty<FProperty>(LsActor->GetClass(), TEXT("EditorLayerSettings"));
        if (!Prop) Prop = FindFProperty<FProperty>(LsActor->GetClass(), TEXT("LayerInfoObjs"));
        if (Prop)
        {
            TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(LsActor, Prop);
            if (Val) Layers.Add(Val);
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("layers"), Layers);
    R->SetNumberField(TEXT("count"),  Layers.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- landscape.sample ------------------------------------------------------

FSageToolDispatch::FOutcome LandscapeSampleImpl(const TSharedPtr<FJsonObject>& Args)
{
    FVector Point = FVector::ZeroVector;
    detail::ParseVector3(Args, TEXT("location"), Point);

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("location"), {
        MakeShared<FJsonValueNumber>(Point.X),
        MakeShared<FJsonValueNumber>(Point.Y),
        MakeShared<FJsonValueNumber>(Point.Z)
    });
    R->SetStringField(TEXT("note"),
        TEXT("heightmap sampling requires landscape data interface; "
             "use LandscapeHeightmapFileFormat or GetHeightAtLocation"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- landscape.list_splines ------------------------------------------------

FSageToolDispatch::FOutcome LandscapeListSplinesImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    UClass* SplinesCls = FindLandscapeClass(TEXT("/Script/Landscape.LandscapeSplineControlPoint"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("landscape splines live in ULandscapeSplinesComponent; "
             "use level.get_spline_info on the landscape actor"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- landscape.get_component -----------------------------------------------

FSageToolDispatch::FOutcome LandscapeGetComponentImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"), A->GetPathName());
    R->SetStringField(TEXT("class"),    A->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- landscape.sculpt / paint_layer ----------------------------------------

FSageToolDispatch::FOutcome LandscapeSculptImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("landscape sculpting requires FLandscapeEditDataInterface; "
             "use editor.run_python with unreal.LandscapeEditorObject"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome LandscapePaintLayerImpl(const TSharedPtr<FJsonObject>& Args)
{ return LandscapeSculptImpl(Args); }

// ---- landscape.set_material ------------------------------------------------

FSageToolDispatch::FOutcome LandscapeSetMaterialImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString MatPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("material_path"), MatPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'material_path'"));

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    AActor* LsActor = FindFirstLandscape(World);
    if (!LsActor) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("no Landscape actor found"));

    UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MatPath);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *MatPath));

    FScopedTransaction Tx(LOCTEXT("SetLsMat", "Set Landscape Material"));
    LsActor->Modify();

    FProperty* Prop = FindFProperty<FProperty>(LsActor->GetClass(), TEXT("LandscapeMaterial"));
    if (Prop)
        detail::SetUPropertyFromJson(LsActor, Prop,
            MakeShared<FJsonValueString>(MatPath));

    LsActor->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material_path"), MatPath);
    R->SetBoolField  (TEXT("modified"),      true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- landscape.add_layer_info ----------------------------------------------

FSageToolDispatch::FOutcome LandscapeAddLayerInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("layer info addition requires ULandscapeLayerInfoObject creation; "
             "use editor.run_python with unreal landscape APIs"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- landscape.import_heightmap --------------------------------------------

FSageToolDispatch::FOutcome LandscapeImportHeightmapImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString FilePath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("file"), FilePath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'file'"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("file"), FilePath);
    R->SetStringField(TEXT("note"),
        TEXT("heightmap import requires FLandscapeImportDescriptor; "
             "use editor.run_python with unreal.EditorLevelLibrary.import_heightmap"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- landscape.get_material_usage_summary ----------------------------------

FSageToolDispatch::FOutcome LandscapeGetMaterialUsageSummaryImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    AActor* LsActor = FindFirstLandscape(World);

    auto R = MakeShared<FJsonObject>();
    if (LsActor)
    {
        FProperty* Prop = FindFProperty<FProperty>(LsActor->GetClass(), TEXT("LandscapeMaterial"));
        if (Prop)
        {
            TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(LsActor, Prop);
            if (Val) R->SetField(TEXT("material"), Val);
        }
        R->SetStringField(TEXT("actor_id"), LsActor->GetPathName());
    }
    else
    {
        R->SetBoolField(TEXT("landscape_found"), false);
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterLandscapeTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("landscape.get_info"),                  GT(&LandscapeGetInfoImpl));
    Dispatch.RegisterHandler(TEXT("landscape.list_layers"),               GT(&LandscapeListLayersImpl));
    Dispatch.RegisterHandler(TEXT("landscape.sample"),                    GT(&LandscapeSampleImpl));
    Dispatch.RegisterHandler(TEXT("landscape.list_splines"),              GT(&LandscapeListSplinesImpl));
    Dispatch.RegisterHandler(TEXT("landscape.get_component"),             GT(&LandscapeGetComponentImpl));
    Dispatch.RegisterHandler(TEXT("landscape.sculpt"),                    GT(&LandscapeSculptImpl));
    Dispatch.RegisterHandler(TEXT("landscape.paint_layer"),               GT(&LandscapePaintLayerImpl));
    Dispatch.RegisterHandler(TEXT("landscape.set_material"),              GT(&LandscapeSetMaterialImpl));
    Dispatch.RegisterHandler(TEXT("landscape.add_layer_info"),            GT(&LandscapeAddLayerInfoImpl));
    Dispatch.RegisterHandler(TEXT("landscape.import_heightmap"),          GT(&LandscapeImportHeightmapImpl));
    Dispatch.RegisterHandler(TEXT("landscape.get_material_usage_summary"),GT(&LandscapeGetMaterialUsageSummaryImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
