#pragma once

// Internal helpers shared by SageActorTools / SageComponentTools / future
// SageAssetTools. Lives under Private/ — not part of the plugin's public ABI.

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "ToolDispatch/SageToolDispatch.h"

class AActor;
class UActorComponent;
class FProperty;
class FJsonObject;
class FJsonValue;

namespace sage::tools::detail
{

// ---- identity --------------------------------------------------------------

[[nodiscard]] AActor*          ResolveActor(const FString& ActorPath);
[[nodiscard]] UActorComponent* ResolveComponent(const FString& ComponentPath);

// ---- guards ---------------------------------------------------------------

// Populates OutErr with the canonical -32004 PIE-active error and returns
// true when the editor is in PIE; otherwise returns false untouched.
[[nodiscard]] bool RejectIfPie(FSageToolDispatch::FOutcome& OutErr);

// ---- JSON helpers ----------------------------------------------------------

bool ParseVector3(const TSharedPtr<FJsonObject>& Args,
                  const FString& FieldName, FVector& Out);
bool ParseRotator3(const TSharedPtr<FJsonObject>& Args,
                   const FString& FieldName, FRotator& Out);

TSharedRef<FJsonValue> Vec3ToJson(const FVector& V);
TSharedRef<FJsonValue> Rot3ToJson(const FRotator& R);

// ---- reflection ------------------------------------------------------------

// Sets a UProperty on Container from a JSON value. Phase 1 supports primitive
// types: bool, int, int64, float, double, string, name, text, byte. Returns
// false when the property type is unsupported.
bool SetUPropertyFromJson(UObject* Container,
                          FProperty* Property,
                          const TSharedPtr<FJsonValue>& Value);

// ---- thread marshalling ---------------------------------------------------

// Runs `Body` on the GameThread. If we're already there, calls inline;
// otherwise marshals via Async() + Future.Get() (the calling WS worker
// thread blocks until UE finishes).
template <typename Fn>
[[nodiscard]] FSageToolDispatch::FOutcome RunOnGameThread(Fn&& Body)
{
    if (IsInGameThread())
    {
        return Body();
    }
    auto Future = Async(EAsyncExecution::TaskGraphMainThread, std::forward<Fn>(Body));
    return Future.Get();
}

}  // namespace sage::tools::detail
