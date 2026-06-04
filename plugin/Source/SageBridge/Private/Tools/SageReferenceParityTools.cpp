#include "Tools/SageReferenceParityTools.h"

#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/EngineTypes.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "SageReferenceParity"

namespace sage::tools
{
namespace
{

static const TCHAR* ReferenceParityTools[] = {
    TEXT("monolith.guide"),
    TEXT("describe.schema"),
    TEXT("describe.list_targets"),
    TEXT("describe.action_schema"),
    TEXT("bulk_fill.apply"),
    TEXT("bulk_fill.list_namespaces"),
    TEXT("did_you_mean"),
    TEXT("blueprint.auto_layout"),
    TEXT("auto_layout"),
    TEXT("blueprint.add_timeline_track"),
    TEXT("blueprint.set_component_override_materials"),
    TEXT("blueprint.set_capsule_size"),
    TEXT("blueprint.cleanup_graph"),
    TEXT("blueprint.connect_pins_batch"),
    TEXT("blueprint.set_node_position"),
    TEXT("add_macro"),
    TEXT("remove_function"),
    TEXT("set_function_params"),
    TEXT("promote_pin_to_variable"),
    TEXT("scaffold_interface_implementation"),
    TEXT("seed_data_asset"),
    TEXT("audit_cdo_drift"),
    TEXT("asset.set_datatable_row"),
    TEXT("asset.add_datatable_row"),
    TEXT("asset.update_datatable_row"),
    TEXT("asset.remove_datatable_row"),
    TEXT("reflection.create_enum"),
    TEXT("reflection.set_enum_entries"),
    TEXT("statetree.read"),
    TEXT("statetree.list_states"),
    TEXT("statetree.add_state"),
    TEXT("statetree.remove_state"),
    TEXT("statetree.set_state_property"),
    TEXT("statetree.clear_state_nodes"),
    TEXT("statetree.add_task"),
    TEXT("statetree.remove_task"),
    TEXT("statetree.set_task_property"),
    TEXT("statetree.set_task_instance_property"),
    TEXT("statetree.add_enter_condition"),
    TEXT("statetree.remove_enter_condition"),
    TEXT("statetree.add_transition"),
    TEXT("statetree.remove_transition"),
    TEXT("statetree.add_transition_condition"),
    TEXT("statetree.add_binding"),
    TEXT("statetree.remove_binding"),
    TEXT("statetree.list_bindings"),
    TEXT("statetree.add_evaluator"),
    TEXT("statetree.remove_evaluator"),
    TEXT("statetree.set_evaluator_property"),
    TEXT("statetree.set_evaluator_instance_property"),
    TEXT("statetree.add_global_task"),
    TEXT("statetree.remove_global_task"),
    TEXT("statetree.set_global_task_property"),
    TEXT("statetree.set_global_task_instance_property"),
    TEXT("statetree.list_colors"),
    TEXT("statetree.add_color"),
    TEXT("statetree.list_state_parameters"),
    TEXT("statetree.add_state_parameter"),
    TEXT("statetree.remove_state_parameter"),
    TEXT("statetree.set_state_parameter"),
    TEXT("statetree.set_root_parameters"),
    TEXT("statetree.compile"),
    TEXT("statetree.validate"),
    TEXT("build_state_tree_from_spec"),
    TEXT("export_st_spec"),
    TEXT("generate_st_diagram"),
    TEXT("auto_arrange_st"),
    TEXT("set_st_schema"),
    TEXT("list_st_task_types"),
    TEXT("list_st_condition_types"),
    TEXT("get_st_bindable_properties"),
    TEXT("add_st_consideration"),
    TEXT("configure_st_consideration"),
    TEXT("list_st_extension_types"),
    TEXT("add_st_extension"),
    TEXT("line_trace"),
    TEXT("raycast"),
    TEXT("overlap_test"),
    TEXT("radial_sweep"),
    TEXT("line_of_sight"),
    TEXT("navigation_raycast"),
    TEXT("find_path"),
    TEXT("test_path"),
    TEXT("get_random_navigable_point"),
    TEXT("level.snap_actor_to_floor"),
    TEXT("level.get_relative_transform"),
    TEXT("level.read_actor_motion"),
    TEXT("level.add_actor_tag"),
    TEXT("level.remove_actor_tag"),
    TEXT("level.set_actor_tags"),
    TEXT("level.list_actor_tags"),
    TEXT("level.attach_actor"),
    TEXT("level.detach_actor"),
    TEXT("level.set_actor_mobility"),
    TEXT("level.get_current_edit_level"),
    TEXT("level.set_current_edit_level"),
    TEXT("level.list_streaming_sublevels"),
    TEXT("level.add_streaming_sublevel"),
    TEXT("level.remove_streaming_sublevel"),
    TEXT("level.set_streaming_sublevel_properties"),
    TEXT("level.spawn_grid"),
    TEXT("level.batch_translate"),
    TEXT("level.place_actors_batch"),
    TEXT("scatter_props"),
    TEXT("replace_blockout_with_assets"),
    TEXT("export_layout"),
    TEXT("import_layout"),
    TEXT("create_town"),
    TEXT("construct_house"),
    TEXT("construct_mansion"),
    TEXT("create_tower"),
    TEXT("create_arch"),
    TEXT("create_staircase"),
    TEXT("create_castle_fortress"),
    TEXT("create_suspension_bridge"),
    TEXT("create_aqueduct"),
    TEXT("create_maze"),
    TEXT("create_pyramid"),
    TEXT("create_wall"),
    TEXT("create_parametric_mesh"),
    TEXT("create_horror_prop"),
    TEXT("create_structure"),
    TEXT("create_building"),
    TEXT("create_pipe_network"),
    TEXT("create_fragments"),
    TEXT("create_terrain_patch"),
    TEXT("generate_floor_plan"),
    TEXT("create_building_from_grid"),
    TEXT("generate_facade"),
    TEXT("generate_roof"),
    TEXT("register_building"),
    TEXT("create_city_block"),
    TEXT("landscape.create"),
    TEXT("landscape_edit"),
    TEXT("paint_foliage"),
    TEXT("add_foliage_instances"),
    TEXT("get_foliage_instances"),
    TEXT("remove_foliage"),
    TEXT("foliage_inspect"),
    TEXT("foliage_edit"),
    TEXT("pcg.export_graph"),
    TEXT("pcg.import_graph"),
    TEXT("pcg_graph_edit"),
    TEXT("tag_registry_edit"),
    TEXT("gas_ops"),
    TEXT("create_aim_offset"),
    TEXT("add_aim_offset_sample"),
    TEXT("create_pose_library"),
    TEXT("add_compatible_skeleton"),
    TEXT("remove_compatible_skeleton"),
    TEXT("get_compatible_skeletons"),
    TEXT("preview_animation"),
    TEXT("create_physics_asset"),
    TEXT("add_physics_body"),
    TEXT("configure_physics_body"),
    TEXT("add_physics_constraint"),
    TEXT("configure_constraint_limits"),
    TEXT("list_physics_bodies"),
    TEXT("assign_cloth_asset_to_mesh"),
    TEXT("bind_cloth_to_skeletal_mesh"),
    TEXT("auto_skin_weights"),
    TEXT("copy_weights"),
    TEXT("mirror_weights"),
    TEXT("normalize_weights"),
    TEXT("prune_weights"),
    TEXT("set_vertex_weights"),
    TEXT("create_morph_target"),
    TEXT("import_morph_targets"),
    TEXT("set_morph_target_deltas"),
    TEXT("add_rig_unit"),
    TEXT("connect_rig_elements"),
    TEXT("animation.auto_layout")
};

UWorld* EditorWorld()
{
    return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FString FirstStringArg(const TSharedPtr<FJsonObject>& Args,
                       std::initializer_list<const TCHAR*> Names,
                       const FString& Fallback = FString())
{
    if (!Args.IsValid()) return Fallback;
    FString Value;
    for (const TCHAR* Name : Names)
    {
        if (Args->TryGetStringField(Name, Value) && !Value.IsEmpty()) return Value;
    }
    return Fallback;
}

bool ReadVec(const TSharedPtr<FJsonObject>& Args, const TCHAR* Name, FVector& Out)
{
    return Args.IsValid() && detail::ParseVector3(Args, Name, Out);
}

TSharedPtr<FJsonObject> VecJsonObject(const FVector& V)
{
    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("x"), V.X);
    R->SetNumberField(TEXT("y"), V.Y);
    R->SetNumberField(TEXT("z"), V.Z);
    return R;
}

TSharedPtr<FJsonObject> TransformJsonObject(const FTransform& T)
{
    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("location"), VecJsonObject(T.GetLocation()));
    R->SetObjectField(TEXT("scale"), VecJsonObject(T.GetScale3D()));
    const FRotator Rot = T.Rotator();
    auto RJ = MakeShared<FJsonObject>();
    RJ->SetNumberField(TEXT("pitch"), Rot.Pitch);
    RJ->SetNumberField(TEXT("yaw"), Rot.Yaw);
    RJ->SetNumberField(TEXT("roll"), Rot.Roll);
    R->SetObjectField(TEXT("rotation"), RJ);
    return R;
}

TSharedPtr<FJsonObject> HitJson(const FHitResult& Hit)
{
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("blocking_hit"), Hit.bBlockingHit);
    R->SetStringField(TEXT("actor"), Hit.GetActor() ? Hit.GetActor()->GetPathName() : FString());
    R->SetStringField(TEXT("component"), Hit.GetComponent() ? Hit.GetComponent()->GetPathName() : FString());
    R->SetObjectField(TEXT("location"), VecJsonObject(Hit.Location));
    R->SetObjectField(TEXT("impact_point"), VecJsonObject(Hit.ImpactPoint));
    R->SetObjectField(TEXT("normal"), VecJsonObject(Hit.Normal));
    R->SetNumberField(TEXT("distance"), Hit.Distance);
    return R;
}

ECollisionChannel CollisionChannelArg(const TSharedPtr<FJsonObject>& Args)
{
    const FString Channel = FirstStringArg(Args, {TEXT("channel"), TEXT("trace_channel")}, TEXT("visibility")).ToLower();
    if (Channel == TEXT("camera")) return ECC_Camera;
    if (Channel == TEXT("world_static")) return ECC_WorldStatic;
    if (Channel == TEXT("world_dynamic")) return ECC_WorldDynamic;
    if (Channel == TEXT("pawn")) return ECC_Pawn;
    if (Channel == TEXT("physics_body")) return ECC_PhysicsBody;
    return ECC_Visibility;
}

AActor* ActorArg(const TSharedPtr<FJsonObject>& Args,
                 std::initializer_list<const TCHAR*> Names,
                 FString& OutId)
{
    OutId = FirstStringArg(Args, Names);
    return detail::ResolveActor(OutId);
}

FSageToolDispatch::FOutcome TraceImpl(const TSharedPtr<FJsonObject>& Args, bool bMulti, bool bSweep)
{
    UWorld* World = EditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FVector Start = FVector::ZeroVector;
    FVector End = FVector::ZeroVector;
    if (!ReadVec(Args, TEXT("start"), Start) || !ReadVec(Args, TEXT("end"), End))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing start/end [x,y,z]"));
    }
    double Radius = 50.0;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("radius"), Radius);

    FCollisionQueryParams Params(SCENE_QUERY_STAT(SageReferenceTrace), true);
    FString IgnoreId;
    if (AActor* Ignore = ActorArg(Args, {TEXT("ignore_actor"), TEXT("ignore")}, IgnoreId))
    {
        Params.AddIgnoredActor(Ignore);
    }

    TArray<FHitResult> Hits;
    if (bSweep)
    {
        World->SweepMultiByChannel(Hits, Start, End, FQuat::Identity,
            CollisionChannelArg(Args), FCollisionShape::MakeSphere(static_cast<float>(Radius)), Params);
    }
    else if (bMulti)
    {
        World->LineTraceMultiByChannel(Hits, Start, End, CollisionChannelArg(Args), Params);
    }
    else
    {
        FHitResult Hit;
        World->LineTraceSingleByChannel(Hit, Start, End, CollisionChannelArg(Args), Params);
        if (Hit.bBlockingHit) Hits.Add(Hit);
    }

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FHitResult& Hit : Hits)
    {
        Rows.Add(MakeShared<FJsonValueObject>(HitJson(Hit)));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("hits"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetBoolField(TEXT("hit"), Rows.Num() > 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome OverlapImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = EditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FVector Location = FVector::ZeroVector;
    if (!ReadVec(Args, TEXT("location"), Location))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing location [x,y,z]"));
    }
    double Radius = 100.0;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("radius"), Radius);

    TArray<FOverlapResult> Overlaps;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SageReferenceOverlap), true);
    World->OverlapMultiByChannel(Overlaps, Location, FQuat::Identity,
        CollisionChannelArg(Args), FCollisionShape::MakeSphere(static_cast<float>(Radius)), Params);

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FOverlapResult& O : Overlaps)
    {
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("actor"), O.GetActor() ? O.GetActor()->GetPathName() : FString());
        Row->SetStringField(TEXT("component"), O.GetComponent() ? O.GetComponent()->GetPathName() : FString());
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("location"), VecJsonObject(Location));
    R->SetNumberField(TEXT("radius"), Radius);
    R->SetArrayField(TEXT("overlaps"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome LineOfSightImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString AId, BId;
    AActor* A = ActorArg(Args, {TEXT("from_actor"), TEXT("actor"), TEXT("a")}, AId);
    AActor* B = ActorArg(Args, {TEXT("to_actor"), TEXT("target"), TEXT("b")}, BId);
    if (!A || !B)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing or invalid from_actor/to_actor"));
    }
    UWorld* World = EditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(SageReferenceLineOfSight), true);
    Params.AddIgnoredActor(A);
    World->LineTraceSingleByChannel(Hit, A->GetActorLocation(), B->GetActorLocation(), CollisionChannelArg(Args), Params);
    const bool bHitTarget = Hit.GetActor() == B;
    const bool bBlocked = Hit.bBlockingHit && !bHitTarget;

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("from_actor"), A->GetPathName());
    R->SetStringField(TEXT("to_actor"), B->GetPathName());
    R->SetBoolField(TEXT("line_of_sight"), !bBlocked);
    R->SetObjectField(TEXT("hit"), HitJson(Hit));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ActorTagsImpl(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    AActor* Actor = ActorArg(Args, {TEXT("actor"), TEXT("actor_id")}, ActorId);
    if (!Actor) return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("actor not found: %s"), *ActorId));

    if (ToolName == TEXT("level.list_actor_tags"))
    {
        TArray<TSharedPtr<FJsonValue>> Tags;
        for (const FName& Tag : Actor->Tags)
        {
            Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("actor"), Actor->GetPathName());
        R->SetArrayField(TEXT("tags"), Tags);
        R->SetNumberField(TEXT("count"), Tags.Num());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FScopedTransaction Tx(LOCTEXT("ActorTags", "Sage: Edit Actor Tags"));
    Actor->Modify();

    if (ToolName == TEXT("level.set_actor_tags"))
    {
        Actor->Tags.Reset();
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Args.IsValid() && Args->TryGetArrayField(TEXT("tags"), Arr) && Arr)
        {
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                if (V.IsValid() && V->Type == EJson::String) Actor->Tags.Add(FName(*V->AsString()));
            }
        }
    }
    else
    {
        const FString Tag = FirstStringArg(Args, {TEXT("tag"), TEXT("name")});
        if (Tag.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing tag/name"));
        if (ToolName == TEXT("level.add_actor_tag")) Actor->Tags.AddUnique(FName(*Tag));
        else Actor->Tags.Remove(FName(*Tag));
    }
    Actor->MarkPackageDirty();
    return ActorTagsImpl(TEXT("level.list_actor_tags"), Args);
}

FSageToolDispatch::FOutcome ActorTransformImpl(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    AActor* Actor = ActorArg(Args, {TEXT("actor"), TEXT("actor_id")}, ActorId);
    if (!Actor) return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("actor not found: %s"), *ActorId));

    if (ToolName == TEXT("level.read_actor_motion"))
    {
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("actor"), Actor->GetPathName());
        R->SetObjectField(TEXT("transform"), TransformJsonObject(Actor->GetActorTransform()));
        R->SetObjectField(TEXT("velocity"), VecJsonObject(Actor->GetVelocity()));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (ToolName == TEXT("level.get_relative_transform"))
    {
        FString ParentId;
        AActor* Parent = ActorArg(Args, {TEXT("parent"), TEXT("parent_actor")}, ParentId);
        if (!Parent) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing or invalid parent"));
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("actor"), Actor->GetPathName());
        R->SetStringField(TEXT("parent"), Parent->GetPathName());
        R->SetObjectField(TEXT("relative_transform"), TransformJsonObject(
            Actor->GetActorTransform().GetRelativeTransform(Parent->GetActorTransform())));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    if (ToolName == TEXT("level.snap_actor_to_floor"))
    {
        UWorld* World = EditorWorld();
        if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));
        const FVector Start = Actor->GetActorLocation();
        const FVector End = Start - FVector(0, 0, 100000.0);
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(SageSnapActorToFloor), true);
        Params.AddIgnoredActor(Actor);
        if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params) || !Hit.bBlockingHit)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("no floor hit below actor"));
        }
        FScopedTransaction Tx(LOCTEXT("SnapActorToFloor", "Sage: Snap Actor To Floor"));
        Actor->Modify();
        Actor->SetActorLocation(Hit.Location);
        Actor->MarkPackageDirty();
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("actor"), Actor->GetPathName());
        R->SetObjectField(TEXT("hit"), HitJson(Hit));
        R->SetObjectField(TEXT("location"), VecJsonObject(Actor->GetActorLocation()));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (ToolName == TEXT("level.attach_actor"))
    {
        FString ParentId;
        AActor* Parent = ActorArg(Args, {TEXT("parent"), TEXT("parent_actor")}, ParentId);
        if (!Parent) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing or invalid parent"));
        FScopedTransaction Tx(LOCTEXT("AttachActor", "Sage: Attach Actor"));
        Actor->Modify();
        Actor->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
        Actor->MarkPackageDirty();
    }
    else if (ToolName == TEXT("level.detach_actor"))
    {
        FScopedTransaction Tx(LOCTEXT("DetachActor", "Sage: Detach Actor"));
        Actor->Modify();
        Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
        Actor->MarkPackageDirty();
    }
    else if (ToolName == TEXT("level.set_actor_mobility"))
    {
        const FString Mobility = FirstStringArg(Args, {TEXT("mobility")}, TEXT("movable")).ToLower();
        EComponentMobility::Type NewMobility = EComponentMobility::Movable;
        if (Mobility == TEXT("static")) NewMobility = EComponentMobility::Static;
        else if (Mobility == TEXT("stationary")) NewMobility = EComponentMobility::Stationary;
        FScopedTransaction Tx(LOCTEXT("SetActorMobility", "Sage: Set Actor Mobility"));
        Actor->Modify();
        if (USceneComponent* Root = Actor->GetRootComponent())
        {
            Root->Modify();
            Root->SetMobility(NewMobility);
        }
        Actor->MarkPackageDirty();
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor"), Actor->GetPathName());
    R->SetObjectField(TEXT("transform"), TransformJsonObject(Actor->GetActorTransform()));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CurrentLevelImpl(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
    UWorld* World = EditorWorld();
    if (!World) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no editor world"));

    if (ToolName == TEXT("level.get_current_edit_level"))
    {
        ULevel* Level = World->GetCurrentLevel();
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("world"), World->GetPathName());
        R->SetStringField(TEXT("current_level"), Level ? Level->GetPathName() : FString());
        R->SetStringField(TEXT("package"), Level && Level->GetOutermost() ? Level->GetOutermost()->GetName() : FString());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    if (ToolName == TEXT("level.list_streaming_sublevels"))
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (ULevelStreaming* Streaming : World->GetStreamingLevels())
        {
            if (!Streaming) continue;
            auto Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("package"), Streaming->GetWorldAssetPackageName());
            Row->SetStringField(TEXT("class"), Streaming->GetClass()->GetPathName());
            Row->SetBoolField(TEXT("loaded"), Streaming->IsLevelLoaded());
            Row->SetBoolField(TEXT("visible"), Streaming->IsLevelVisible());
            Row->SetBoolField(TEXT("should_be_loaded"), Streaming->ShouldBeLoaded());
            Row->SetBoolField(TEXT("should_be_visible"), Streaming->ShouldBeVisible());
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("world"), World->GetPathName());
        R->SetArrayField(TEXT("streaming_levels"), Rows);
        R->SetNumberField(TEXT("count"), Rows.Num());
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("streaming sublevel mutation/current edit-level switching requires editor level utilities not exposed in this safe reference-parity wrapper"));
}

FSageToolDispatch::FOutcome BatchTranslateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    FVector Delta = FVector::ZeroVector;
    if (!ReadVec(Args, TEXT("delta"), Delta))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing delta [x,y,z]"));
    }
    const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
    if (!Args.IsValid() || !Args->TryGetArrayField(TEXT("actors"), Actors) || !Actors)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing actors array"));
    }
    TArray<TSharedPtr<FJsonValue>> Rows;
    FScopedTransaction Tx(LOCTEXT("BatchTranslateActors", "Sage: Batch Translate Actors"));
    for (const TSharedPtr<FJsonValue>& Value : *Actors)
    {
        const FString Id = Value.IsValid() ? Value->AsString() : FString();
        AActor* Actor = detail::ResolveActor(Id);
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("actor"), Id);
        if (Actor)
        {
            Actor->Modify();
            Actor->AddActorWorldOffset(Delta, false);
            Actor->MarkPackageDirty();
            Row->SetBoolField(TEXT("moved"), true);
            Row->SetObjectField(TEXT("location"), VecJsonObject(Actor->GetActorLocation()));
        }
        else
        {
            Row->SetBoolField(TEXT("moved"), false);
            Row->SetStringField(TEXT("error"), TEXT("actor not found"));
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("delta"), VecJsonObject(Delta));
    R->SetArrayField(TEXT("results"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReferenceMetaImpl(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("tool"), ToolName);
    if (ToolName == TEXT("monolith.guide"))
    {
        R->SetStringField(TEXT("scope"), TEXT("Reference parity guide"));
        R->SetStringField(TEXT("guidance"), TEXT("Sage exposes exact reference tool names where stable. Unsupported vendor/private graph mutations return hard MCP errors instead of placeholder success."));
    }
    else if (ToolName == TEXT("bulk_fill.list_namespaces"))
    {
        R->SetArrayField(TEXT("namespaces"), {
            MakeShared<FJsonValueString>(TEXT("blueprint")),
            MakeShared<FJsonValueString>(TEXT("statetree")),
            MakeShared<FJsonValueString>(TEXT("level")),
            MakeShared<FJsonValueString>(TEXT("animation")),
            MakeShared<FJsonValueString>(TEXT("pcg")),
            MakeShared<FJsonValueString>(TEXT("landscape")),
            MakeShared<FJsonValueString>(TEXT("foliage"))
        });
    }
    else if (ToolName.StartsWith(TEXT("describe.")))
    {
        R->SetStringField(TEXT("description"), TEXT("Use tools/list for canonical MCP schemas; this compatibility action reports the reference-parity registration boundary."));
    }
    else if (ToolName == TEXT("did_you_mean"))
    {
        const FString Query = FirstStringArg(Args, {TEXT("query"), TEXT("tool"), TEXT("name")});
        R->SetStringField(TEXT("query"), Query);
        R->SetStringField(TEXT("hint"), TEXT("Call tools/list and fuzzy-match names client-side; Sage keeps exact reference aliases registered for known gaps."));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome UnsupportedImpl(const FString& ToolName)
{
    FString Reason = TEXT("requires a dedicated exported editor/plugin graph contract that is not safe to fake through generic reflection");
    if (ToolName.StartsWith(TEXT("statetree.")) || ToolName.Contains(TEXT("_st")))
    {
        Reason = TEXT("StateTree graph authoring requires StateTree editor schema, node instance, binding, and compile contracts beyond this reference-parity wrapper");
    }
    else if (ToolName.Contains(TEXT("foliage")) || ToolName.StartsWith(TEXT("landscape")) || ToolName.StartsWith(TEXT("pcg")))
    {
        Reason = TEXT("domain-specific editor subsystem support is required for durable Landscape/Foliage/PCG graph mutation");
    }
    else if (ToolName.StartsWith(TEXT("create_")) || ToolName.StartsWith(TEXT("construct_")) || ToolName.StartsWith(TEXT("generate_")) || ToolName.StartsWith(TEXT("scatter_")))
    {
        Reason = TEXT("procedural generation actions require a closed spec-to-asset contract; Sage will not emit placeholder geometry");
    }
    else if (ToolName.Contains(TEXT("weight")) || ToolName.Contains(TEXT("cloth")) || ToolName.Contains(TEXT("physics")) || ToolName.Contains(TEXT("rig")) || ToolName.Contains(TEXT("morph")))
    {
        Reason = TEXT("animation/physics/cloth/ControlRig authoring requires asset-specific editor APIs and validation not exposed through this generic wrapper");
    }
    return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("%s is registered for reference parity but unsupported in the safe generic wrapper: %s"),
            *ToolName, *Reason));
}

FSageToolDispatch::FOutcome ReferenceParityImpl(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
    if (ToolName == TEXT("monolith.guide")
        || ToolName.StartsWith(TEXT("describe."))
        || ToolName == TEXT("bulk_fill.list_namespaces")
        || ToolName == TEXT("did_you_mean"))
    {
        return ReferenceMetaImpl(ToolName, Args);
    }
    if (ToolName == TEXT("line_trace") || ToolName == TEXT("raycast"))
    {
        return TraceImpl(Args, false, false);
    }
    if (ToolName == TEXT("radial_sweep"))
    {
        return TraceImpl(Args, true, true);
    }
    if (ToolName == TEXT("overlap_test"))
    {
        return OverlapImpl(Args);
    }
    if (ToolName == TEXT("line_of_sight"))
    {
        return LineOfSightImpl(Args);
    }
    if (ToolName == TEXT("level.add_actor_tag")
        || ToolName == TEXT("level.remove_actor_tag")
        || ToolName == TEXT("level.set_actor_tags")
        || ToolName == TEXT("level.list_actor_tags"))
    {
        return ActorTagsImpl(ToolName, Args);
    }
    if (ToolName == TEXT("level.snap_actor_to_floor")
        || ToolName == TEXT("level.get_relative_transform")
        || ToolName == TEXT("level.read_actor_motion")
        || ToolName == TEXT("level.attach_actor")
        || ToolName == TEXT("level.detach_actor")
        || ToolName == TEXT("level.set_actor_mobility"))
    {
        return ActorTransformImpl(ToolName, Args);
    }
    if (ToolName == TEXT("level.get_current_edit_level")
        || ToolName == TEXT("level.set_current_edit_level")
        || ToolName == TEXT("level.list_streaming_sublevels")
        || ToolName == TEXT("level.add_streaming_sublevel")
        || ToolName == TEXT("level.remove_streaming_sublevel")
        || ToolName == TEXT("level.set_streaming_sublevel_properties"))
    {
        return CurrentLevelImpl(ToolName, Args);
    }
    if (ToolName == TEXT("level.batch_translate"))
    {
        return BatchTranslateImpl(Args);
    }
    return UnsupportedImpl(ToolName);
}

}  // namespace

void RegisterReferenceParityTools(FSageToolDispatch& Dispatch)
{
    for (const TCHAR* Name : ReferenceParityTools)
    {
        const FString ToolName(Name);
        Dispatch.RegisterHandler(ToolName,
            [ToolName](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
            {
                return detail::RunOnGameThread([&]() -> FSageToolDispatch::FOutcome
                {
                    return ReferenceParityImpl(ToolName, Args);
                });
            });
    }
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
