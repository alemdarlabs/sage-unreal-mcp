#include "Tools/SageNetworkingTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "SageNetworking"

namespace sage::tools
{
namespace
{

bool TryGetActorIdentifier(const TSharedPtr<FJsonObject>& Args, FString& OutId)
{
    OutId.Reset();
    return Args.IsValid()
        && (Args->TryGetStringField(TEXT("actor_id"), OutId)
            || Args->TryGetStringField(TEXT("actor"), OutId))
        && !OutId.IsEmpty();
}

// ---- networking.set_replicates ---------------------------------------------

FSageToolDispatch::FOutcome NetworkingSetReplicatesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    bool bReplicates = true;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    Args->TryGetBoolField(TEXT("replicates"), bReplicates);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetReplicates", "Set Replicates"));
    A->Modify();
    A->SetReplicates(bReplicates);
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),   A->GetPathName());
    R->SetBoolField  (TEXT("replicates"), bReplicates);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.set_property_replicated ------------------------------------

FSageToolDispatch::FOutcome NetworkingSetPropertyReplicatedImpl(const TSharedPtr<FJsonObject>& Args)
{
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("note"),
        TEXT("UPROPERTY replication flags require source code or Blueprint replication lists; "
             "use bp.set_variable_properties with is_replicated=true for Blueprint properties"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.configure_net_frequency ------------------------------------

FSageToolDispatch::FOutcome NetworkingConfigureNetFrequencyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    float NetFreq = 100.0f;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("net_update_frequency"), NetFreq);

    float MinNetFreq = 2.0f;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("min_net_update_frequency"), MinNetFreq);

    FScopedTransaction Tx(LOCTEXT("SetNetFreq", "Set Net Update Frequency"));
    A->Modify();
    A->SetNetUpdateFrequency(NetFreq);
    A->SetMinNetUpdateFrequency(MinNetFreq);
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),              A->GetPathName());
    R->SetNumberField(TEXT("net_update_frequency"),  A->GetNetUpdateFrequency());
    R->SetNumberField(TEXT("min_net_update_frequency"), A->GetMinNetUpdateFrequency());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.set_dormancy -----------------------------------------------

FSageToolDispatch::FOutcome NetworkingSetDormancyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    FString DormancyStr = TEXT("DORM_Never");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("dormancy"), DormancyStr);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    ENetDormancy Dormancy = DORM_Never;
    if      (DormancyStr == TEXT("DORM_Awake"))           Dormancy = DORM_Awake;
    else if (DormancyStr == TEXT("DORM_DormantAll"))      Dormancy = DORM_DormantAll;
    else if (DormancyStr == TEXT("DORM_DormantPartial"))  Dormancy = DORM_DormantPartial;
    else if (DormancyStr == TEXT("DORM_Initial"))         Dormancy = DORM_Initial;

    FScopedTransaction Tx(LOCTEXT("SetDormancy", "Set Net Dormancy"));
    A->Modify();
    A->NetDormancy = Dormancy;
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),  A->GetPathName());
    R->SetStringField(TEXT("dormancy"),  DormancyStr);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.set_net_load_on_client -------------------------------------

FSageToolDispatch::FOutcome NetworkingSetNetLoadOnClientImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    bool bLoad = true;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    Args->TryGetBoolField(TEXT("net_load_on_client"), bLoad);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetNetLoad", "Set Net Load On Client"));
    A->Modify();
    A->bNetLoadOnClient = bLoad;
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),         A->GetPathName());
    R->SetBoolField  (TEXT("net_load_on_client"), bLoad);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.set_always_relevant ----------------------------------------

FSageToolDispatch::FOutcome NetworkingSetAlwaysRelevantImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    bool bAlways = true;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    Args->TryGetBoolField(TEXT("always_relevant"), bAlways);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetAlwaysRel", "Set Always Relevant"));
    A->Modify();
    A->bAlwaysRelevant = bAlways;
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),       A->GetPathName());
    R->SetBoolField  (TEXT("always_relevant"), bAlways);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.set_only_relevant_to_owner ---------------------------------

FSageToolDispatch::FOutcome NetworkingSetOnlyRelevantToOwnerImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    bool bOnly = true;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    Args->TryGetBoolField(TEXT("only_relevant_to_owner"), bOnly);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetOnlyOwner", "Set Only Relevant To Owner"));
    A->Modify();
    A->bOnlyRelevantToOwner = bOnly;
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),              A->GetPathName());
    R->SetBoolField  (TEXT("only_relevant_to_owner"), bOnly);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.configure_cull_distance ------------------------------------

FSageToolDispatch::FOutcome NetworkingConfigureCullDistanceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));

    float CullDist = 0.0f;
    if (Args.IsValid()) Args->TryGetNumberField(TEXT("cull_distance"), CullDist);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetCull", "Set Net Cull Distance"));
    A->Modify();
    A->SetNetCullDistanceSquared(CullDist * CullDist);
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),     A->GetPathName());
    R->SetNumberField(TEXT("cull_distance"), CullDist);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.set_priority -----------------------------------------------

FSageToolDispatch::FOutcome NetworkingSetPriorityImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    float Priority = 1.0f;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    Args->TryGetNumberField(TEXT("net_priority"), Priority);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetNetPriority", "Set Net Priority"));
    A->Modify();
    A->NetPriority = Priority;
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),    A->GetPathName());
    R->SetNumberField(TEXT("net_priority"), A->NetPriority);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.set_replicate_movement -------------------------------------

FSageToolDispatch::FOutcome NetworkingSetReplicateMovementImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString ActorId;
    bool bReplicate = true;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor_id'"));
    Args->TryGetBoolField(TEXT("replicate_movement"), bReplicate);

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    FScopedTransaction Tx(LOCTEXT("SetRepMove", "Set Replicate Movement"));
    A->Modify();
    A->SetReplicateMovement(bReplicate);
    A->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),          A->GetPathName());
    R->SetBoolField  (TEXT("replicate_movement"), bReplicate);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- networking.get_info ---------------------------------------------------

FSageToolDispatch::FOutcome NetworkingGetInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString ActorId;
    if (!TryGetActorIdentifier(Args, ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'actor'"));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("actor_id"),                A->GetPathName());
    R->SetBoolField  (TEXT("replicates"),              A->GetIsReplicated());
    R->SetBoolField  (TEXT("always_relevant"),         A->bAlwaysRelevant);
    R->SetBoolField  (TEXT("only_relevant_to_owner"),  A->bOnlyRelevantToOwner);
    R->SetBoolField  (TEXT("net_load_on_client"),      A->bNetLoadOnClient);
    R->SetBoolField  (TEXT("replicate_movement"),      A->IsReplicatingMovement());
    R->SetNumberField(TEXT("net_update_frequency"),    A->GetNetUpdateFrequency());
    R->SetNumberField(TEXT("min_net_update_frequency"),A->GetMinNetUpdateFrequency());
    R->SetNumberField(TEXT("net_priority"),            A->NetPriority);
    R->SetNumberField(TEXT("net_cull_distance"),       FMath::Sqrt(A->GetNetCullDistanceSquared()));

    FString DormancyStr;
    switch (A->NetDormancy)
    {
    case DORM_Awake:          DormancyStr = TEXT("DORM_Awake");          break;
    case DORM_DormantAll:     DormancyStr = TEXT("DORM_DormantAll");     break;
    case DORM_DormantPartial: DormancyStr = TEXT("DORM_DormantPartial"); break;
    case DORM_Initial:        DormancyStr = TEXT("DORM_Initial");        break;
    default:                  DormancyStr = TEXT("DORM_Never");          break;
    }
    R->SetStringField(TEXT("dormancy"), DormancyStr);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterNetworkingTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("networking.set_replicates"),             GT(&NetworkingSetReplicatesImpl));
    Dispatch.RegisterHandler(TEXT("networking.set_property_replicated"),    GT(&NetworkingSetPropertyReplicatedImpl));
    Dispatch.RegisterHandler(TEXT("networking.configure_net_frequency"),    GT(&NetworkingConfigureNetFrequencyImpl));
    Dispatch.RegisterHandler(TEXT("networking.set_dormancy"),               GT(&NetworkingSetDormancyImpl));
    Dispatch.RegisterHandler(TEXT("networking.set_net_load_on_client"),     GT(&NetworkingSetNetLoadOnClientImpl));
    Dispatch.RegisterHandler(TEXT("networking.set_always_relevant"),        GT(&NetworkingSetAlwaysRelevantImpl));
    Dispatch.RegisterHandler(TEXT("networking.set_only_relevant_to_owner"), GT(&NetworkingSetOnlyRelevantToOwnerImpl));
    Dispatch.RegisterHandler(TEXT("networking.configure_cull_distance"),    GT(&NetworkingConfigureCullDistanceImpl));
    Dispatch.RegisterHandler(TEXT("networking.set_priority"),               GT(&NetworkingSetPriorityImpl));
    Dispatch.RegisterHandler(TEXT("networking.set_replicate_movement"),     GT(&NetworkingSetReplicateMovementImpl));
    Dispatch.RegisterHandler(TEXT("networking.get_info"),                   GT(&NetworkingGetInfoImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
