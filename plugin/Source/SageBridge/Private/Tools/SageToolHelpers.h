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
//
// Phase 4.0 — full UProperty surface:
//   - Primitives: bool, int8/16/32/64, uint8/16/32/64, float, double,
//     FString, FName, FText, byte (uint8 with optional UEnum metadata)
//   - Containers: TArray<T>, TMap<K,V>, TSet<T> (recursive on inner type)
//   - References:
//     - TObjectPtr<T> / UObject*: accept asset path string or null
//     - FSoftObjectPtr / TSoftObjectPtr<T>: accept path string
//     - TSubclassOf<T> / UClass*: accept class path or null
//     - TSoftClassPtr<T>: accept class path
//   - Enums (FEnumProperty + FByteProperty with UEnum):
//     accept name string or numeric value
//   - Structs: vector/rotator/transform/color/linearColor/intpoint/intvector
//     have shorthand JSON forms; arbitrary USTRUCTs go through ImportText/
//     ExportText round-trip.
//
// Per-element edit ops (set_property_array_op family) call the lower-level
// raw-pointer API directly with the inner FProperty + element pointer.

// Public: write a UProperty on a UObject container from a JSON value.
// Returns false when the property type isn't supported or the value shape
// doesn't match (e.g. JSON object on a primitive).
bool SetUPropertyFromJson(UObject* Container,
                          FProperty* Property,
                          const TSharedPtr<FJsonValue>& Value);

// Public: read a UProperty into a JSON value. Symmetric inverse.
[[nodiscard]] TSharedPtr<FJsonValue> GetUPropertyAsJson(const UObject* Container,
                                                        const FProperty* Property);

// Lower-level: write into a raw memory location given a property descriptor.
// Used by both the public form (after ContainerPtrToValuePtr) and the
// per-element ops (when iterating TArray elements via FScriptArrayHelper).
bool SetPropertyValueAtPtr(FProperty* Property, void* ValuePtr,
                           const TSharedPtr<FJsonValue>& Value);

// Symmetric read from raw pointer.
[[nodiscard]] TSharedPtr<FJsonValue> GetPropertyValueAtPtr(const FProperty* Property,
                                                            const void* ValuePtr);

// Type-aware JSON value equality for the primitive set we round-trip.
// Numbers compared with FMath::IsNearlyEqual; arrays/objects fall back to false.
[[nodiscard]] bool JsonValuesEqual(const TSharedPtr<FJsonValue>& A,
                                    const TSharedPtr<FJsonValue>& B);

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
