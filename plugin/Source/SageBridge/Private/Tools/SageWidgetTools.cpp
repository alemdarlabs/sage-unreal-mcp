#include "Tools/SageWidgetTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetNavigation.h"
#include "IAssetTools.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

// Editor utility widget support (Blutility). Pulled in dynamically via class
// lookup at runtime so plugin still loads when Blutility is disabled, but the
// concrete factory + subsystem types are referenced when available.
#include "EditorUtilityWidget.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "EditorUtilityWidgetBlueprintFactory.h"
#include "EditorUtilitySubsystem.h"
#include "Editor/EditorEngine.h"

// Widget animation authoring (Phase 4.11-r3 — CommonAIExport parity).
// Storage: UWidgetBlueprint::Animations is a TArray<UWidgetAnimation*>. Each
// UWidgetAnimation owns a UMovieScene + a TArray<FWidgetAnimationBinding>
// mapping widget names to MovieScene possessables.
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneChannelProxy.h"

#define LOCTEXT_NAMESPACE "SageWidget"

namespace sage::tools
{
namespace
{

UObject* ResolveAsset(const FString& Path)
{
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Obj;
}

UWidgetBlueprint* ResolveWidgetBlueprint(const FString& Path)
{
    UObject* Obj = ResolveAsset(Path);
    return Cast<UWidgetBlueprint>(Obj);
}

// Recursively walk a UWidget hierarchy emitting {name, class, children}
TSharedRef<FJsonObject> WidgetToJson(const UWidget* W)
{
    auto J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("name"),  W->GetName());
    J->SetStringField(TEXT("class"), W->GetClass()->GetName());
    if (const UPanelWidget* Panel = Cast<UPanelWidget>(W))
    {
        TArray<TSharedPtr<FJsonValue>> Kids;
        const int32 Count = Panel->GetChildrenCount();
        for (int32 I = 0; I < Count; ++I)
        {
            UWidget* Child = Panel->GetChildAt(I);
            if (!Child) continue;
            Kids.Add(MakeShared<FJsonValueObject>(WidgetToJson(Child)));
        }
        if (Kids.Num() > 0) J->SetArrayField(TEXT("children"), Kids);
    }
    return J;
}

// ---- widget.create -------------------------------------------------------

FSageToolDispatch::FOutcome CreateWidgetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }

    // Optional parent_class — must be UUserWidget subclass
    UClass* ParentClass = UUserWidget::StaticClass();
    FString ParentStr;
    if (Args->TryGetStringField(TEXT("parent_class"), ParentStr) && !ParentStr.IsEmpty())
    {
        UClass* Found = FindObject<UClass>(nullptr, *ParentStr);
        if (!Found) Found = LoadObject<UClass>(nullptr, *ParentStr);
        if (!Found)
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("parent_class not found: %s"), *ParentStr));
        }
        if (!Found->IsChildOf(UUserWidget::StaticClass()))
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("parent_class %s is not a UUserWidget"),
                                *Found->GetName()));
        }
        ParentClass = Found;
    }

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd) ||
        PackagePath.IsEmpty() || AssetName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("path must be /Folder/AssetName form"));
    }
    if (FindPackage(nullptr, *(PackagePath / AssetName)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("package already exists: %s/%s"), *PackagePath, *AssetName));
    }

    FScopedTransaction Tx(LOCTEXT("CreateWidget", "Create Widget Blueprint"));
    UPackage* Pkg = CreatePackage(*(PackagePath / AssetName));
    if (!Pkg)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("CreatePackage failed for %s/%s"), *PackagePath, *AssetName));
    }
    Pkg->FullyLoad();
    Pkg->Modify();

    UWidgetBlueprintFactory* Fac = NewObject<UWidgetBlueprintFactory>();
    Fac->ParentClass = ParentClass;
    // RootWidgetClass is private; factory uses its CanvasPanel default.
    UObject* Created = Fac->FactoryCreateNew(UWidgetBlueprint::StaticClass(),
        Pkg, *AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn);
    if (!Created)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("WidgetBlueprintFactory failed for %s"), *AssetName));
    }
    FAssetRegistryModule::AssetCreated(Created);
    Created->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         Created->GetPathName());
    R->SetStringField(TEXT("name"),         Created->GetName());
    R->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    if (UWidgetBlueprint* WB = Cast<UWidgetBlueprint>(Created))
    {
        if (WB->WidgetTree && WB->WidgetTree->RootWidget)
        {
            R->SetStringField(TEXT("root_widget_class"),
                WB->WidgetTree->RootWidget->GetClass()->GetName());
        }
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.list ---------------------------------------------------------

FSageToolDispatch::FOutcome ListWidgetsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Dir = TEXT("/Game");
    if (Args.IsValid()) Args->TryGetStringField(TEXT("directory"), Dir);
    int32 MaxResults = 1000;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("max_results"), N))
        {
            MaxResults = FMath::Clamp(static_cast<int32>(N), 1, 50000);
        }
    }

    FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry"));
    IAssetRegistry& Registry = Module.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*Dir));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/UMGEditor.WidgetBlueprint")));

    TArray<FAssetData> Found;
    Registry.GetAssets(Filter, Found);

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const FAssetData& A : Found)
    {
        if (Out.Num() >= MaxResults) break;
        auto O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("path"), A.GetSoftObjectPath().ToString());
        O->SetStringField(TEXT("name"), A.AssetName.ToString());
        Out.Add(MakeShared<FJsonValueObject>(O));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("directory"), Dir);
    R->SetArrayField (TEXT("widgets"),   Out);
    R->SetNumberField(TEXT("returned"),  Out.Num());
    R->SetNumberField(TEXT("total"),     Found.Num());
    R->SetBoolField  (TEXT("capped"),    Out.Num() < Found.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.add_widget ---------------------------------------------------

FSageToolDispatch::FOutcome AddWidgetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString BPPath, ClassPath, Name, ParentName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint"), BPPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'blueprint'"));
    }
    if (!Args->TryGetStringField(TEXT("widget_class"), ClassPath) || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'widget_class'"));
    }
    Args->TryGetStringField(TEXT("name"), Name);
    Args->TryGetStringField(TEXT("parent"), ParentName);

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(BPPath);
    if (!WB || !WB->WidgetTree)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UWidgetBlueprint with a WidgetTree: %s"), *BPPath));
    }

    UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassPath);
    if (!Cls || !Cls->IsChildOf(UWidget::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("widget_class %s is not a UWidget subclass"), *ClassPath));
    }
    if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("widget_class %s is abstract/deprecated"), *Cls->GetName()));
    }

    FName WidgetFName = Name.IsEmpty() ? NAME_None : FName(*Name);
    if (!Name.IsEmpty() && WB->WidgetTree->FindWidget(WidgetFName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("widget '%s' already exists in tree"), *Name));
    }

    FScopedTransaction Tx(LOCTEXT("AddWidget", "Add Widget"));
    WB->Modify();
    WB->WidgetTree->Modify();

    UWidget* New = WB->WidgetTree->ConstructWidget<UWidget>(Cls, WidgetFName);
    if (!New)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("ConstructWidget failed for %s"), *Cls->GetName()));
    }

    FString AttachedTo;
    if (!ParentName.IsEmpty())
    {
        UWidget* P = WB->WidgetTree->FindWidget(FName(*ParentName));
        UPanelWidget* Panel = Cast<UPanelWidget>(P);
        if (!Panel)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("parent '%s' not a UPanelWidget"), *ParentName));
        }
        Panel->AddChild(New);
        AttachedTo = Panel->GetName();
    }
    else
    {
        if (!WB->WidgetTree->RootWidget)
        {
            WB->WidgetTree->RootWidget = New;
            AttachedTo = TEXT("(root)");
        }
        else if (UPanelWidget* RootPanel = Cast<UPanelWidget>(WB->WidgetTree->RootWidget))
        {
            RootPanel->AddChild(New);
            AttachedTo = RootPanel->GetName();
        }
        else
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("root widget is not a UPanelWidget — supply 'parent' explicitly"));
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WB);
    WB->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"),    WB->GetPathName());
    R->SetStringField(TEXT("name"),         New->GetName());
    R->SetStringField(TEXT("class"),        New->GetClass()->GetName());
    R->SetStringField(TEXT("attached_to"),  AttachedTo);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.remove_widget ------------------------------------------------

FSageToolDispatch::FOutcome RemoveWidgetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString BPPath, Name;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint"), BPPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'blueprint'"));
    }
    if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(BPPath);
    if (!WB || !WB->WidgetTree)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UWidgetBlueprint: %s"), *BPPath));
    }
    UWidget* Target = WB->WidgetTree->FindWidget(FName(*Name));
    if (!Target)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("widget '%s' not found in tree"), *Name));
    }

    FScopedTransaction Tx(LOCTEXT("RemoveWidget", "Remove Widget"));
    WB->Modify();
    WB->WidgetTree->Modify();

    int32 ChildIndex = INDEX_NONE;
    UPanelWidget* Parent = UWidgetTree::FindWidgetParent(Target, ChildIndex);
    bool bRemoved = false;
    if (Parent)
    {
        bRemoved = Parent->RemoveChild(Target);
    }
    else if (Target == WB->WidgetTree->RootWidget)
    {
        WB->WidgetTree->RootWidget = nullptr;
        bRemoved = true;
    }
    if (!bRemoved)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("RemoveChild failed for '%s'"), *Name));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(WB);
    WB->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), WB->GetPathName());
    R->SetStringField(TEXT("removed"),   Name);
    R->SetStringField(TEXT("parent"),    Parent ? Parent->GetName() : TEXT("(root)"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.set_property -------------------------------------------------

FSageToolDispatch::FOutcome SetWidgetPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString BPPath, Name, PropName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("blueprint"), BPPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'blueprint'"));
    }
    if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));
    }
    if (!Args->TryGetStringField(TEXT("property"), PropName) || PropName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'property'"));
    }
    const TSharedPtr<FJsonValue> Value = Args->Values.FindRef(TEXT("value"));
    if (!Value.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));
    }

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(BPPath);
    if (!WB || !WB->WidgetTree)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UWidgetBlueprint: %s"), *BPPath));
    }
    UWidget* Target = WB->WidgetTree->FindWidget(FName(*Name));
    if (!Target)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("widget '%s' not found"), *Name));
    }
    FProperty* P = Target->GetClass()->FindPropertyByName(*PropName);
    if (!P)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("property '%s' not on %s"),
                            *PropName, *Target->GetClass()->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("SetWidgetProperty", "Set Widget Property"));
    Target->Modify();
    WB->Modify();

    if (!detail::SetUPropertyFromJson(Target, P, Value))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("could not set %s.%s from given value"),
                            *Target->GetClass()->GetName(), *PropName));
    }
    Target->PostEditChange();
    FBlueprintEditorUtils::MarkBlueprintAsModified(WB);
    WB->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("blueprint"), WB->GetPathName());
    R->SetStringField(TEXT("widget"),    Name);
    R->SetStringField(TEXT("property"),  PropName);
    R->SetField(TEXT("new_value"),
        detail::GetUPropertyAsJson(Target, P).IsValid()
            ? detail::GetUPropertyAsJson(Target, P)
            : MakeShared<FJsonValueNull>());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.read ---------------------------------------------------------

FSageToolDispatch::FOutcome ReadWidgetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid()
        || (!Args->TryGetStringField(TEXT("path"), Path)
            && !Args->TryGetStringField(TEXT("blueprint"), Path)
            && !Args->TryGetStringField(TEXT("asset"), Path)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB)
    {
        UObject* Obj = ResolveAsset(Path);
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UWidgetBlueprint: %s"),
                            Obj ? *Obj->GetClass()->GetName() : TEXT("<not found>")));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), WB->GetPathName());
    R->SetStringField(TEXT("name"), WB->GetName());
    R->SetStringField(TEXT("parent_class"),
        WB->ParentClass ? WB->ParentClass->GetPathName() : TEXT(""));

    if (WB->WidgetTree)
    {
        if (WB->WidgetTree->RootWidget)
        {
            R->SetField(TEXT("root"),
                MakeShared<FJsonValueObject>(WidgetToJson(WB->WidgetTree->RootWidget)));
        }

        // Flat enumeration of every widget under WidgetTree
        TArray<UWidget*> All;
        WB->WidgetTree->GetAllWidgets(All);
        TArray<TSharedPtr<FJsonValue>> Flat;
        for (UWidget* W : All)
        {
            if (!W) continue;
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"),  W->GetName());
            J->SetStringField(TEXT("class"), W->GetClass()->GetName());
            Flat.Add(MakeShared<FJsonValueObject>(J));
        }
        R->SetArrayField (TEXT("widgets"), Flat);
        R->SetNumberField(TEXT("widget_count"), Flat.Num());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.get_details ----------------------------------------------------

FSageToolDispatch::FOutcome GetWidgetDetailsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("WidgetBlueprint not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Props;
    for (TFieldIterator<FProperty> It(WB->GetClass()); It; ++It)
    {
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), It->GetName());
        TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(WB, *It);
        if (Val) J->SetField(TEXT("value"), Val);
        Props.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       WB->GetPathName());
    R->SetArrayField (TEXT("properties"), Props);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.read_animations ------------------------------------------------

FSageToolDispatch::FOutcome ReadWidgetAnimationsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("WidgetBlueprint not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Anims;
    // Animations stored as UWidgetAnimation objects in the generated class
    for (TFieldIterator<FProperty> It(WB->GetClass()); It; ++It)
    {
        FObjectProperty* ObjProp = CastField<FObjectProperty>(*It);
        if (!ObjProp) continue;
        if (ObjProp->PropertyClass && ObjProp->PropertyClass->GetName().Contains(TEXT("WidgetAnimation")))
        {
            auto J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("name"), It->GetName());
            Anims.Add(MakeShared<FJsonValueObject>(J));
        }
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),       WB->GetPathName());
    R->SetArrayField (TEXT("animations"), Anims);
    R->SetNumberField(TEXT("count"),      Anims.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.create_utility_widget ------------------------------------------

FSageToolDispatch::FOutcome CreateUtilityWidgetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* EUWCls = UEditorUtilityWidget::StaticClass();
    if (!EUWCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("EditorUtilityWidget not found — Blutility plugin required"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    // UEditorUtilityWidgetBlueprintFactory produces a UEditorUtilityWidgetBlueprint
    // (vs. UWidgetBlueprintFactory which makes a plain UWidgetBlueprint). The
    // distinction matters: only UEditorUtilityWidgetBlueprint surfaces in the
    // "Run Editor Utility Widget" right-click menu and is accepted by
    // UEditorUtilitySubsystem::SpawnAndRegisterTab().
    UEditorUtilityWidgetBlueprintFactory* Factory =
        NewObject<UEditorUtilityWidgetBlueprintFactory>();
    Factory->ParentClass = EUWCls;

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, nullptr, Factory);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         NewObj->GetPathName());
    R->SetStringField(TEXT("class"),        NewObj->GetClass()->GetName());
    R->SetStringField(TEXT("parent_class"), TEXT("EditorUtilityWidget"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.run_utility_widget ---------------------------------------------

FSageToolDispatch::FOutcome RunUtilityWidgetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UObject* Asset = ResolveAsset(Path);
    UEditorUtilityWidgetBlueprint* EUWBP = Cast<UEditorUtilityWidgetBlueprint>(Asset);
    if (!EUWBP)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("not a UEditorUtilityWidgetBlueprint: %s "
                                  "(create via widget.create_utility_widget)"),
                Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));
    }

    if (!GEditor)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("GEditor null — cannot spawn editor utility widget"));
    }
    UEditorUtilitySubsystem* Subsys = GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>();
    if (!Subsys)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("UEditorUtilitySubsystem unavailable"));
    }

    FName TabId = NAME_None;
    UEditorUtilityWidget* Spawned = Subsys->SpawnAndRegisterTabAndGetID(EUWBP, TabId);
    if (!Spawned)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("SpawnAndRegisterTab returned null for %s"), *Path));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),    EUWBP->GetPathName());
    R->SetStringField(TEXT("tab_id"),  TabId.ToString());
    R->SetStringField(TEXT("widget"),  Spawned->GetName());
    R->SetStringField(TEXT("class"),   Spawned->GetClass()->GetName());
    R->SetBoolField  (TEXT("spawned"), true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.create_utility_blueprint ---------------------------------------

FSageToolDispatch::FOutcome CreateUtilityBlueprintImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UClass* EUBCls = FindObject<UClass>(nullptr,
        TEXT("/Script/Blutility.GlobalEditorUtilityBase"));
    if (!EUBCls) EUBCls = LoadObject<UClass>(nullptr,
        TEXT("/Script/Blutility.GlobalEditorUtilityBase"));
    if (!EUBCls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("GlobalEditorUtilityBase not found — Blutility plugin required"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid path"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, EUBCls, nullptr);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         NewObj->GetPathName());
    R->SetStringField(TEXT("parent_class"), TEXT("GlobalEditorUtilityBase"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.run_utility_blueprint ------------------------------------------

FSageToolDispatch::FOutcome RunUtilityBlueprintImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.TryLoad();
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("asset not found: %s"), *Path));

    // Try to find and call Run() method via reflection
    UFunction* RunFn = Obj->FindFunction(TEXT("Run"));
    if (!RunFn) RunFn = Obj->FindFunction(TEXT("Execute"));
    if (RunFn)
    {
        Obj->ProcessEvent(RunFn, nullptr);
        auto R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("path"),    Obj->GetPathName());
        R->SetBoolField  (TEXT("invoked"), true);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetStringField(TEXT("note"), TEXT("No Run/Execute function found; call via editor context menu or UEditorUtilityLibrary"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.move_widget ----------------------------------------------------

FSageToolDispatch::FOutcome MoveWidgetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, WidgetName, NewParentName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"),           Path)
        || !Args->TryGetStringField(TEXT("widget_name"),    WidgetName)
        || !Args->TryGetStringField(TEXT("new_parent"),     NewParentName))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'widget_name', or 'new_parent'"));

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("WidgetBlueprint not found: %s"), *Path));

    if (!WB->WidgetTree)
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("WidgetTree missing"));

    UWidget* Target = nullptr;
    UPanelWidget* NewParent = nullptr;
    TArray<UWidget*> All;
    WB->WidgetTree->GetAllWidgets(All);
    for (UWidget* W : All)
    {
        if (!Target && W->GetName() == WidgetName) Target = W;
        if (!NewParent && W->GetName() == NewParentName) NewParent = Cast<UPanelWidget>(W);
    }

    if (!Target) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("widget not found: %s"), *WidgetName));
    if (!NewParent) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("parent not found or not a panel: %s"), *NewParentName));

    FScopedTransaction Tx(LOCTEXT("MoveWidget", "Move Widget"));
    WB->Modify();
    WB->WidgetTree->Modify();

    // Remove from current parent
    if (UPanelWidget* OldParent = Target->GetParent())
    {
        OldParent->Modify();
        OldParent->RemoveChild(Target);
    }
    NewParent->Modify();
    NewParent->AddChild(Target);
    WB->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("widget"),     WidgetName);
    R->SetStringField(TEXT("new_parent"), NewParentName);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.list_classes ---------------------------------------------------

FSageToolDispatch::FOutcome ListWidgetClassesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Filter;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("filter"), Filter);

    TArray<TSharedPtr<FJsonValue>> Classes;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Cls = *It;
        if (!Cls || !Cls->IsChildOf(UWidget::StaticClass())) continue;
        if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
        FString Name = Cls->GetName();
        if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase)) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),  Name);
        J->SetStringField(TEXT("path"),  FSoftObjectPath(Cls).ToString());
        Classes.Add(MakeShared<FJsonValueObject>(J));
        if (Classes.Num() >= 200) break;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("classes"), Classes);
    R->SetNumberField(TEXT("count"),   Classes.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.list_runtime ---------------------------------------------------

FSageToolDispatch::FOutcome ListRuntimeWidgetsImpl(const TSharedPtr<FJsonObject>& Args)
{
    // List widgets active in PIE world
    UWorld* PieWorld = nullptr;
    if (GEngine)
    {
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
            {
                PieWorld = Ctx.World(); break;
            }
        }
    }

    auto R = MakeShared<FJsonObject>();
    if (!PieWorld)
    {
        R->SetStringField(TEXT("note"), TEXT("no PIE world active; widget.list_runtime requires active Play session"));
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    // Query via player controller viewport client
    TArray<TSharedPtr<FJsonValue>> Widgets;
    for (TObjectIterator<UUserWidget> It; It; ++It)
    {
        UUserWidget* W = *It;
        if (!W->IsInViewport()) continue;
        if (W->GetWorld() != PieWorld) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),  W->GetName());
        J->SetStringField(TEXT("class"), W->GetClass()->GetName());
        Widgets.Add(MakeShared<FJsonValueObject>(J));
    }

    R->SetArrayField (TEXT("widgets"), Widgets);
    R->SetNumberField(TEXT("count"),   Widgets.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.get_runtime ----------------------------------------------------

FSageToolDispatch::FOutcome GetRuntimeWidgetImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString WidgetName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("name"), WidgetName))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'name'"));

    UUserWidget* Found = nullptr;
    for (TObjectIterator<UUserWidget> It; It; ++It)
    {
        UUserWidget* Candidate = *It;
        if (Candidate->GetName() == WidgetName)
        {
            Found = Candidate; break;
        }
    }

    if (!Found) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("runtime widget not found: %s"), *WidgetName));

    TArray<TSharedPtr<FJsonValue>> Props;
    for (TFieldIterator<FProperty> It(Found->GetClass()); It; ++It)
    {
        if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), It->GetName());
        TSharedPtr<FJsonValue> Val = detail::GetUPropertyAsJson(Found, *It);
        if (Val) J->SetField(TEXT("value"), Val);
        Props.Add(MakeShared<FJsonValueObject>(J));
        if (Props.Num() >= 100) break;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("name"),        Found->GetName());
    R->SetStringField(TEXT("class"),       Found->GetClass()->GetName());
    R->SetBoolField  (TEXT("in_viewport"), Found->IsInViewport());
    R->SetArrayField (TEXT("properties"),  Props);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.get_runtime_delegates ------------------------------------------

FSageToolDispatch::FOutcome GetRuntimeDelegatesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("WidgetBlueprint not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Delegates;
    UClass* SearchClass = WB->GeneratedClass
        ? (UClass*)WB->GeneratedClass
        : WB->GetClass();
    for (TFieldIterator<FProperty> It(SearchClass); It; ++It)
    {
        FMulticastDelegateProperty* Prop = CastField<FMulticastDelegateProperty>(*It);
        if (!Prop) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"), Prop->GetName());
        FString ExtendedType;
        J->SetStringField(TEXT("cpp_type"), Prop->GetCPPType(&ExtendedType, 0));
        Delegates.Add(MakeShared<FJsonValueObject>(J));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),      WB->GetPathName());
    R->SetArrayField (TEXT("delegates"), Delegates);
    R->SetNumberField(TEXT("count"),     Delegates.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- widget.anim.* (Phase 4.11-r3) -----------------------------------------
//
// CommonAIExport parity: 4 authoring tools for UMG animations. Storage is
// UWidgetBlueprint::Animations (TArray<UWidgetAnimation*>). Each animation
// owns a UMovieScene + a TArray<FWidgetAnimationBinding> mapping widget
// names → MovieScene possessable GUIDs.

UWidgetAnimation* FindWidgetAnimation(UWidgetBlueprint* WB, const FString& AnimName)
{
    if (!WB) return nullptr;
    // DoS guard: pathological assets with thousands of animation slots should not
    // burn handler time on a linear scan. UMG editor tops out far below 1024 in
    // practice; anything beyond is corrupt or hostile input.
    constexpr int32 MAX_ANIMATIONS = 1024;
    const int32 Limit = FMath::Min(WB->Animations.Num(), MAX_ANIMATIONS);
    for (int32 I = 0; I < Limit; ++I)
    {
        UWidgetAnimation* A = WB->Animations[I];
        // Case-insensitive: editor allows mixed-case rename, but client callers
        // often canonicalize to lowercase. Mirror that tolerance.
        if (A && A->GetName().Equals(AnimName, ESearchCase::IgnoreCase)) return A;
    }
    return nullptr;
}

FSageToolDispatch::FOutcome AnimCreateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, AnimName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("name"), AnimName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'name'"));
    }
    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("WidgetBlueprint not found: %s"), *Path));
    if (FindWidgetAnimation(WB, AnimName) != nullptr)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("animation '%s' already exists on %s"),
                            *AnimName, *Path));
    }

    int32 FrameRate = 60;
    double Duration = 5.0;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("frame_rate"), N))
            FrameRate = FMath::Clamp(static_cast<int32>(N), 1, 480);
        if (Args->TryGetNumberField(TEXT("duration_seconds"), N))
            Duration = FMath::Clamp(N, 0.001, 3600.0);
    }

    FScopedTransaction Tx(LOCTEXT("WidgetAnimCreate", "Create Widget Animation"));
    WB->Modify();

    const FName UniqueName(*AnimName);
    UWidgetAnimation* Anim = NewObject<UWidgetAnimation>(WB, UniqueName,
        RF_Public | RF_Transactional);
    Anim->MovieScene = NewObject<UMovieScene>(Anim, UniqueName,
        RF_Public | RF_Transactional);

    Anim->MovieScene->SetDisplayRate(FFrameRate(FrameRate, 1));
    const FFrameRate Tick = Anim->MovieScene->GetTickResolution();
    const FFrameNumber EndFrame = Tick.AsFrameTime(Duration).RoundToFrame();
    Anim->MovieScene->SetPlaybackRange(
        TRange<FFrameNumber>(FFrameNumber(0), EndFrame));

    WB->Animations.Add(Anim);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),             WB->GetPathName());
    R->SetStringField(TEXT("name"),             Anim->GetName());
    R->SetNumberField(TEXT("frame_rate"),       FrameRate);
    R->SetNumberField(TEXT("duration_seconds"), Duration);
    R->SetNumberField(TEXT("animation_count"),  WB->Animations.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AnimBindImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, AnimName, WidgetName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("anim_name"), AnimName)
        || !Args->TryGetStringField(TEXT("widget_name"), WidgetName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'anim_name' or 'widget_name'"));
    }
    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("WidgetBlueprint not found: %s"), *Path));
    UWidgetAnimation* Anim = FindWidgetAnimation(WB, AnimName);
    if (!Anim) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("animation not found: %s"), *AnimName));

    UWidget* Target = WB->WidgetTree
        ? WB->WidgetTree->FindWidget(FName(*WidgetName))
        : nullptr;
    if (!Target) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("widget '%s' not found in WidgetTree"), *WidgetName));

    // Skip if a binding already exists for this widget.
    for (const FWidgetAnimationBinding& B : Anim->AnimationBindings)
    {
        if (B.WidgetName == FName(*WidgetName))
        {
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("path"),         WB->GetPathName());
            R->SetStringField(TEXT("anim_name"),    AnimName);
            R->SetStringField(TEXT("widget_name"),  WidgetName);
            R->SetStringField(TEXT("binding_guid"), B.AnimationGuid.ToString());
            R->SetBoolField  (TEXT("already_bound"), true);
            return FSageToolDispatch::FOutcome::MakeSuccess(R);
        }
    }

    FScopedTransaction Tx(LOCTEXT("WidgetAnimBind", "Bind Widget Animation"));
    WB->Modify();
    Anim->Modify();

    // FWidgetAnimationBinding::WidgetName resolution at runtime:
    // FWidgetAnimationBinding::FindRuntimeObject(...) calls
    //   WidgetTree.FindWidget(*WidgetName.ToString())
    // which compares against UWidget::GetName(). For BP-generated widgets the
    // object name == the property/variable name on the WidgetTree, so passing
    // Target->GetName() is correct — verified against UE 5.7
    // Runtime/UMG/Private/Animation/WidgetAnimationBinding.cpp.
    const FGuid Guid = Anim->MovieScene->AddPossessable(
        Target->GetName(), Target->GetClass());

    FWidgetAnimationBinding Binding;
    Binding.WidgetName     = FName(*Target->GetName());
    Binding.SlotWidgetName = NAME_None;
    Binding.AnimationGuid  = Guid;
    Binding.bIsRootWidget  = (WB->WidgetTree && Target == WB->WidgetTree->RootWidget);
    Anim->AnimationBindings.Add(Binding);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),         WB->GetPathName());
    R->SetStringField(TEXT("anim_name"),    AnimName);
    R->SetStringField(TEXT("widget_name"),  WidgetName);
    R->SetStringField(TEXT("binding_guid"), Guid.ToString());
    R->SetBoolField  (TEXT("is_root"),      Binding.bIsRootWidget);
    R->SetNumberField(TEXT("binding_count"),Anim->AnimationBindings.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AnimAddTrackImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, AnimName, GuidStr, PropName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("anim_name"), AnimName)
        || !Args->TryGetStringField(TEXT("binding_guid"), GuidStr)
        || !Args->TryGetStringField(TEXT("property_name"), PropName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'anim_name', 'binding_guid' or 'property_name'"));
    }
    FString TrackType = TEXT("float");
    Args->TryGetStringField(TEXT("track_type"), TrackType);
    if (!TrackType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("track_type '%s' not supported (only 'float')"),
                            *TrackType));
    }

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("WidgetBlueprint not found: %s"), *Path));
    UWidgetAnimation* Anim = FindWidgetAnimation(WB, AnimName);
    if (!Anim || !Anim->MovieScene)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("animation not found: %s"), *AnimName));

    FGuid Guid;
    if (!FGuid::Parse(GuidStr, Guid))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid binding_guid: %s"), *GuidStr));

    // Reject orphan tracks: GUID must already correspond to a possessable on the
    // MovieScene (added via widget.anim.bind). Otherwise AddTrack succeeds but the
    // track has no binding, which is invisible to the runtime/editor.
    if (!Anim->MovieScene->FindPossessable(Guid))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("binding_guid %s is not a possessable on animation '%s' "
                                  "(call widget.anim.bind first)"),
                            *GuidStr, *AnimName));
    }

    FScopedTransaction Tx(LOCTEXT("WidgetAnimAddTrack", "Add Widget Animation Track"));
    Anim->MovieScene->Modify();

    UMovieSceneFloatTrack* Track = Anim->MovieScene->AddTrack<UMovieSceneFloatTrack>(Guid);
    if (!Track) return FSageToolDispatch::FOutcome::MakeError(-32000,
        TEXT("AddTrack<UMovieSceneFloatTrack> returned null"));
    Track->SetPropertyNameAndPath(FName(*PropName), PropName);

    UMovieSceneSection* Section = Track->CreateNewSection();
    if (Section)
    {
        Section->SetRange(TRange<FFrameNumber>::All());
        Track->AddSection(*Section);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),          WB->GetPathName());
    R->SetStringField(TEXT("anim_name"),     AnimName);
    R->SetStringField(TEXT("binding_guid"),  GuidStr);
    R->SetStringField(TEXT("property_name"), PropName);
    R->SetStringField(TEXT("track_type"),    TEXT("float"));
    R->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AnimAddKeyframeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, AnimName, GuidStr, PropName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("anim_name"), AnimName)
        || !Args->TryGetStringField(TEXT("binding_guid"), GuidStr)
        || !Args->TryGetStringField(TEXT("property_name"), PropName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'anim_name', 'binding_guid' or 'property_name'"));
    }
    double TimeSeconds = 0.0;
    double Value = 0.0;
    if (!Args->TryGetNumberField(TEXT("time_seconds"), TimeSeconds))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'time_seconds'"));
    }
    if (!Args->TryGetNumberField(TEXT("value"), Value))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'value'"));
    }

    UWidgetBlueprint* WB = ResolveWidgetBlueprint(Path);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("WidgetBlueprint not found: %s"), *Path));
    UWidgetAnimation* Anim = FindWidgetAnimation(WB, AnimName);
    if (!Anim || !Anim->MovieScene)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("animation not found: %s"), *AnimName));

    FGuid Guid;
    if (!FGuid::Parse(GuidStr, Guid))
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("invalid binding_guid: %s"), *GuidStr));

    // Locate the float track on the binding matching property_name.
    UMovieSceneFloatTrack* Track = nullptr;
    for (UMovieSceneTrack* T : Anim->MovieScene->FindTracks(
            UMovieSceneFloatTrack::StaticClass(), Guid))
    {
        UMovieSceneFloatTrack* FT = Cast<UMovieSceneFloatTrack>(T);
        if (FT && FT->GetPropertyName() == FName(*PropName))
        {
            Track = FT;
            break;
        }
    }
    if (!Track) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("no float track for property '%s' on binding %s"),
                        *PropName, *GuidStr));

    UMovieSceneFloatSection* Section = nullptr;
    for (UMovieSceneSection* S : Track->GetAllSections())
    {
        if (UMovieSceneFloatSection* FS = Cast<UMovieSceneFloatSection>(S))
        {
            Section = FS;
            break;
        }
    }
    if (!Section)
    {
        Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
        if (!Section) return FSageToolDispatch::FOutcome::MakeError(-32000,
            TEXT("CreateNewSection returned null"));
        Section->SetRange(TRange<FFrameNumber>::All());
        Track->AddSection(*Section);
    }

    FScopedTransaction Tx(LOCTEXT("WidgetAnimAddKey", "Add Widget Animation Keyframe"));
    Section->Modify();

    const FFrameRate Tick = Anim->MovieScene->GetTickResolution();
    const FFrameNumber Frame = Tick.AsFrameTime(TimeSeconds).RoundToFrame();

    TArrayView<FMovieSceneFloatChannel*> Channels =
        Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
    if (Channels.Num() == 0)
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            TEXT("section has no float channels"));
    Channels[0]->AddCubicKey(Frame, static_cast<float>(Value));
    int32 KeyCount = Channels[0]->GetData().GetTimes().Num();

    // Expand playback range if the new key is past the current end.
    TRange<FFrameNumber> Range = Anim->MovieScene->GetPlaybackRange();
    if (Range.HasUpperBound() && Frame > Range.GetUpperBoundValue())
    {
        Anim->MovieScene->SetPlaybackRange(
            TRange<FFrameNumber>(Range.GetLowerBoundValue(), Frame));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),          WB->GetPathName());
    R->SetStringField(TEXT("anim_name"),     AnimName);
    R->SetStringField(TEXT("binding_guid"),  GuidStr);
    R->SetStringField(TEXT("property_name"), PropName);
    R->SetNumberField(TEXT("time_seconds"),  TimeSeconds);
    R->SetNumberField(TEXT("value"),         Value);
    R->SetNumberField(TEXT("key_count"),     KeyCount);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

static constexpr int32 kWidgetUnsupportedCode = -32005;

FSageToolDispatch::FOutcome WidgetUnsupported(const FString& Tool, const FString& Reason)
{
    return FSageToolDispatch::FOutcome::MakeError(kWidgetUnsupportedCode,
        FString::Printf(TEXT("%s is not exposed as a safe WidgetBlueprint mutation yet: %s"),
                        *Tool, *Reason));
}

FString FirstWidgetStringArg(const TSharedPtr<FJsonObject>& Args,
                             std::initializer_list<const TCHAR*> Fields)
{
    if (!Args.IsValid()) return FString();
    FString Value;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetStringField(Field, Value) && !Value.IsEmpty()) return Value;
    }
    return FString();
}

bool FirstWidgetBoolArg(const TSharedPtr<FJsonObject>& Args,
                        std::initializer_list<const TCHAR*> Fields,
                        bool DefaultValue = false)
{
    if (!Args.IsValid()) return DefaultValue;
    bool Value = DefaultValue;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetBoolField(Field, Value)) return Value;
    }
    return DefaultValue;
}

UWidgetBlueprint* ResolveWidgetBlueprintFromArgs(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstWidgetStringArg(Args, {TEXT("blueprint"), TEXT("path"), TEXT("asset")});
    return Path.IsEmpty() ? nullptr : ResolveWidgetBlueprint(Path);
}

UWidget* ResolveWidgetFromArgs(const TSharedPtr<FJsonObject>& Args, UWidgetBlueprint*& OutBlueprint)
{
    OutBlueprint = ResolveWidgetBlueprintFromArgs(Args);
    if (!OutBlueprint || !OutBlueprint->WidgetTree) return nullptr;
    const FString Name = FirstWidgetStringArg(Args, {TEXT("widget"), TEXT("name"), TEXT("widget_name")});
    return Name.IsEmpty()
        ? OutBlueprint->WidgetTree->RootWidget.Get()
        : OutBlueprint->WidgetTree->FindWidget(FName(*Name));
}

void SaveWidgetIfRequested(UWidgetBlueprint* WB, const TSharedPtr<FJsonObject>& Args, const TSharedRef<FJsonObject>& R)
{
    if (!WB || !FirstWidgetBoolArg(Args, {TEXT("save")}, false) || !GEditor) return;
    if (UEditorAssetSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
    {
        R->SetBoolField(TEXT("saved"), Sub->SaveLoadedAsset(WB));
    }
}

FSageToolDispatch::FOutcome DumpUiSpecSchemaImpl(const TSharedPtr<FJsonObject>& Args)
{
    TSharedRef<FJsonObject> Widget = MakeShared<FJsonObject>();
    Widget->SetStringField(TEXT("name"), TEXT("TitleText"));
    Widget->SetStringField(TEXT("class"), TEXT("/Script/UMG.TextBlock"));
    Widget->SetStringField(TEXT("parent"), TEXT("RootCanvas"));
    Widget->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
    TArray<TSharedPtr<FJsonValue>> Widgets{MakeShared<FJsonValueObject>(Widget)};

    TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("path"), TEXT("/Game/UI/WBP_Name"));
    Schema->SetStringField(TEXT("parent_class"), TEXT("/Script/UMG.UserWidget"));
    Schema->SetArrayField(TEXT("widgets"), Widgets);

    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("format"), TEXT("SageWidgetSpec.v1"));
    R->SetObjectField(TEXT("schema"), Schema);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome DumpUiSpecImpl(const TSharedPtr<FJsonObject>& Args)
{
    return ReadWidgetImpl(Args);
}

FSageToolDispatch::FOutcome BuildUiFromSpecImpl(const TSharedPtr<FJsonObject>& Args)
{
    const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
    TSharedPtr<FJsonObject> Spec = Args;
    if (Args.IsValid() && Args->TryGetObjectField(TEXT("spec"), SpecPtr) && SpecPtr && SpecPtr->IsValid())
    {
        Spec = *SpecPtr;
    }
    FString Path;
    if (!Spec.IsValid() || !Spec->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("spec.path is required"));
    }

    TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
    CreateArgs->SetStringField(TEXT("path"), Path);
    FString ParentClass;
    if (Spec->TryGetStringField(TEXT("parent_class"), ParentClass) && !ParentClass.IsEmpty())
    {
        CreateArgs->SetStringField(TEXT("parent_class"), ParentClass);
    }
    FSageToolDispatch::FOutcome Created = CreateWidgetImpl(CreateArgs);
    if (!Created.bSuccess) return Created;

    TArray<TSharedPtr<FJsonValue>> Added;
    const TArray<TSharedPtr<FJsonValue>>* Widgets = nullptr;
    if (Spec->TryGetArrayField(TEXT("widgets"), Widgets) && Widgets)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Widgets)
        {
            const TSharedPtr<FJsonObject>* WidgetSpec = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(WidgetSpec) || !WidgetSpec || !WidgetSpec->IsValid()) continue;
            FString ClassPath;
            if (!(*WidgetSpec)->TryGetStringField(TEXT("class"), ClassPath))
            {
                (*WidgetSpec)->TryGetStringField(TEXT("widget_class"), ClassPath);
            }
            if (ClassPath.IsEmpty()) continue;
            FString Name, Parent;
            TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
            AddArgs->SetStringField(TEXT("blueprint"), Path);
            AddArgs->SetStringField(TEXT("widget_class"), ClassPath);
            if ((*WidgetSpec)->TryGetStringField(TEXT("name"), Name)) AddArgs->SetStringField(TEXT("name"), Name);
            if ((*WidgetSpec)->TryGetStringField(TEXT("parent"), Parent)) AddArgs->SetStringField(TEXT("parent"), Parent);
            FSageToolDispatch::FOutcome AddedOne = AddWidgetImpl(AddArgs);
            if (!AddedOne.bSuccess) return AddedOne;
            Added.Add(MakeShared<FJsonValueObject>(AddedOne.Result.ToSharedRef()));

            const TSharedPtr<FJsonObject>* Props = nullptr;
            if (!Name.IsEmpty() && (*WidgetSpec)->TryGetObjectField(TEXT("properties"), Props) && Props && Props->IsValid())
            {
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Props)->Values)
                {
                    TSharedPtr<FJsonObject> SetArgs = MakeShared<FJsonObject>();
                    SetArgs->SetStringField(TEXT("blueprint"), Path);
                    SetArgs->SetStringField(TEXT("name"), Name);
                    SetArgs->SetStringField(TEXT("property"), Pair.Key);
                    SetArgs->SetField(TEXT("value"), Pair.Value);
                    FSageToolDispatch::FOutcome Set = SetWidgetPropertyImpl(SetArgs);
                    if (!Set.bSuccess) return Set;
                }
            }
        }
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Path);
    R->SetObjectField(TEXT("created"), Created.Result.ToSharedRef());
    R->SetArrayField(TEXT("added"), Added);
    R->SetNumberField(TEXT("added_count"), Added.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RenameWidgetParityImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (FSageToolDispatch::FOutcome Reject; detail::RejectIfPie(Reject)) return Reject;
    UWidgetBlueprint* WB = nullptr;
    UWidget* Widget = ResolveWidgetFromArgs(Args, WB);
    if (!WB || !Widget) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("widget not found"));
    const FString NewName = FirstWidgetStringArg(Args, {TEXT("new_name"), TEXT("to")});
    if (NewName.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'new_name'"));
    if (WB->WidgetTree->FindWidget(FName(*NewName)))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("target widget name already exists"));
    }
    const FString OldName = Widget->GetName();
    FScopedTransaction Tx(LOCTEXT("SageRenameWidget", "Sage: Rename Widget"));
    Widget->Modify();
    Widget->Rename(*NewName, WB->WidgetTree, REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("old_name"), OldName);
    R->SetStringField(TEXT("new_name"), Widget->GetName());
    SaveWidgetIfRequested(WB, Args, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetWidgetIsVariableImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (FSageToolDispatch::FOutcome Reject; detail::RejectIfPie(Reject)) return Reject;
    UWidgetBlueprint* WB = nullptr;
    UWidget* Widget = ResolveWidgetFromArgs(Args, WB);
    if (!WB || !Widget) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("widget not found"));
    const bool bValue = FirstWidgetBoolArg(Args, {TEXT("is_variable"), TEXT("value")}, true);
    FScopedTransaction Tx(LOCTEXT("SageSetWidgetIsVariable", "Sage: Set Widget Is Variable"));
    Widget->Modify();
    Widget->bIsVariable = bValue;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("widget"), Widget->GetName());
    R->SetBoolField(TEXT("is_variable"), bValue);
    SaveWidgetIfRequested(WB, Args, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AuditFocusChainImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWidgetBlueprint* WB = ResolveWidgetBlueprintFromArgs(Args);
    if (!WB || !WB->WidgetTree) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("WidgetBlueprint not found"));
    TArray<UWidget*> All;
    WB->WidgetTree->GetAllWidgets(All);
    TArray<TSharedPtr<FJsonValue>> Rows;
    TArray<TSharedPtr<FJsonValue>> Issues;
    for (UWidget* Widget : All)
    {
        if (!Widget) continue;
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Widget->GetName());
        Row->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
        Row->SetBoolField(TEXT("is_enabled"), Widget->GetIsEnabled());
        Row->SetBoolField(TEXT("is_variable"), Widget->bIsVariable != 0);
        Row->SetBoolField(TEXT("has_navigation"), Widget->Navigation != nullptr);
#if WITH_EDITORONLY_DATA
        Row->SetBoolField(TEXT("override_accessibility"), Widget->bOverrideAccessibleDefaults != 0);
        Row->SetStringField(TEXT("accessible_text"), Widget->GetAccessibleText().ToString());
#endif
        if (Widget->bIsVariable && !Widget->Navigation)
        {
            TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
            Issue->SetStringField(TEXT("widget"), Widget->GetName());
            Issue->SetStringField(TEXT("code"), TEXT("variable_without_navigation"));
            Issues.Add(MakeShared<FJsonValueObject>(Issue));
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("widgets"), Rows);
    R->SetArrayField(TEXT("issues"), Issues);
    R->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ApplyTokenBindingImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWidgetBlueprint* WB = nullptr;
    UWidget* Widget = ResolveWidgetFromArgs(Args, WB);
    if (!WB || !Widget) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("widget not found"));
    const FString Property = FirstWidgetStringArg(Args, {TEXT("property")});
    const FString Token = FirstWidgetStringArg(Args, {TEXT("token")});
    const TSharedPtr<FJsonObject>* Tokens = nullptr;
    if (Property.IsEmpty() || Token.IsEmpty() ||
        !Args.IsValid() || !Args->TryGetObjectField(TEXT("tokens"), Tokens) || !Tokens || !Tokens->IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing property/token/tokens"));
    }
    TSharedPtr<FJsonValue> Value = (*Tokens)->TryGetField(Token);
    if (!Value.IsValid()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("token not found"));
    TSharedPtr<FJsonObject> SetArgs = MakeShared<FJsonObject>();
    SetArgs->SetStringField(TEXT("blueprint"), WB->GetPathName());
    SetArgs->SetStringField(TEXT("name"), Widget->GetName());
    SetArgs->SetStringField(TEXT("property"), Property);
    SetArgs->SetField(TEXT("value"), Value);
    return SetWidgetPropertyImpl(SetArgs);
}

FSageToolDispatch::FOutcome ListWidgetPropertyEnumsImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString ClassPath = FirstWidgetStringArg(Args, {TEXT("widget_class"), TEXT("class")});
    UClass* Cls = ClassPath.IsEmpty() ? UWidget::StaticClass() : FindObject<UClass>(nullptr, *ClassPath);
    if (!Cls && !ClassPath.IsEmpty()) Cls = LoadObject<UClass>(nullptr, *ClassPath);
    if (!Cls || !Cls->IsChildOf(UWidget::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("class is not a UWidget subclass"));
    }
    TArray<TSharedPtr<FJsonValue>> Items;
    for (TFieldIterator<FProperty> It(Cls); It; ++It)
    {
        FProperty* Prop = *It;
        UEnum* Enum = nullptr;
        if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop)) Enum = EnumProp->GetEnum();
        else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop)) Enum = ByteProp->Enum;
        if (!Enum) continue;
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("property"), Prop->GetName());
        J->SetStringField(TEXT("enum"), Enum->GetPathName());
        TArray<TSharedPtr<FJsonValue>> Values;
        for (int32 I = 0; I < Enum->NumEnums() - 1; ++I)
        {
            Values.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(I)));
        }
        J->SetArrayField(TEXT("values"), Values);
        Items.Add(MakeShared<FJsonValueObject>(J));
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("class"), Cls->GetPathName());
    R->SetArrayField(TEXT("properties"), Items);
    R->SetNumberField(TEXT("count"), Items.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool ParseUiNavigation(const FString& Text, EUINavigation& Out)
{
    if (Text.Equals(TEXT("left"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Left; return true; }
    if (Text.Equals(TEXT("right"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Right; return true; }
    if (Text.Equals(TEXT("up"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Up; return true; }
    if (Text.Equals(TEXT("down"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Down; return true; }
    if (Text.Equals(TEXT("next"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Next; return true; }
    if (Text.Equals(TEXT("previous"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Previous; return true; }
    return false;
}

bool ParseUiNavigationRule(const FString& Text, EUINavigationRule& Out)
{
    if (Text.Equals(TEXT("escape"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::Escape; return true; }
    if (Text.Equals(TEXT("explicit"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::Explicit; return true; }
    if (Text.Equals(TEXT("wrap"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::Wrap; return true; }
    if (Text.Equals(TEXT("stop"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::Stop; return true; }
    if (Text.Equals(TEXT("custom"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::Custom; return true; }
    if (Text.Equals(TEXT("custom_boundary"), ESearchCase::IgnoreCase)) { Out = EUINavigationRule::CustomBoundary; return true; }
    return false;
}

FSageToolDispatch::FOutcome DumpWidgetNavigationImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWidgetBlueprint* WB = ResolveWidgetBlueprintFromArgs(Args);
    if (!WB || !WB->WidgetTree) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("WidgetBlueprint not found"));
    TArray<UWidget*> All;
    WB->WidgetTree->GetAllWidgets(All);
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (UWidget* Widget : All)
    {
        if (!Widget) continue;
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Widget->GetName());
        Row->SetBoolField(TEXT("has_navigation"), Widget->Navigation != nullptr);
        if (Widget->Navigation)
        {
            TSharedRef<FJsonObject> Nav = MakeShared<FJsonObject>();
            for (TFieldIterator<FProperty> It(Widget->Navigation->GetClass()); It; ++It)
            {
                FProperty* P = *It;
                if (TSharedPtr<FJsonValue> V = detail::GetUPropertyAsJson(Widget->Navigation, P))
                {
                    Nav->SetField(P->GetName(), V);
                }
            }
            Row->SetObjectField(TEXT("navigation"), Nav);
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("widgets"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetWidgetNavigationBulkImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (FSageToolDispatch::FOutcome Reject; detail::RejectIfPie(Reject)) return Reject;
    UWidgetBlueprint* WB = ResolveWidgetBlueprintFromArgs(Args);
    if (!WB || !WB->WidgetTree) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("WidgetBlueprint not found"));
    const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
    if (!Args.IsValid() || !Args->TryGetArrayField(TEXT("rules"), Rules) || !Rules)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'rules' array"));
    }
    FScopedTransaction Tx(LOCTEXT("SageSetWidgetNavigationBulk", "Sage: Set Widget Navigation Bulk"));
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const TSharedPtr<FJsonValue>& V : *Rules)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!V.IsValid() || !V->TryGetObject(Obj) || !Obj || !Obj->IsValid()) continue;
        FString WidgetName, DirectionText, RuleText, TargetName;
        (*Obj)->TryGetStringField(TEXT("widget"), WidgetName);
        (*Obj)->TryGetStringField(TEXT("direction"), DirectionText);
        (*Obj)->TryGetStringField(TEXT("rule"), RuleText);
        (*Obj)->TryGetStringField(TEXT("target"), TargetName);
        UWidget* Widget = WB->WidgetTree->FindWidget(FName(*WidgetName));
        EUINavigation Direction;
        EUINavigationRule Rule;
        if (!Widget || !ParseUiNavigation(DirectionText, Direction) || !ParseUiNavigationRule(RuleText, Rule))
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid navigation rule entry"));
        }
        Widget->Modify();
        if (Rule == EUINavigationRule::Explicit)
        {
            UWidget* Target = WB->WidgetTree->FindWidget(FName(*TargetName));
            if (!Target)
            {
                Tx.Cancel();
                return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("explicit navigation target not found"));
            }
            Widget->SetNavigationRuleExplicit(Direction, Target);
        }
        else if (Rule == EUINavigationRule::Custom || Rule == EUINavigationRule::CustomBoundary)
        {
            Tx.Cancel();
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("custom navigation rules require delegate binding and are not safe through set_widget_navigation_bulk"));
        }
        else
        {
            Widget->SetNavigationRuleBase(Direction, Rule);
        }
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("widget"), WidgetName);
        Row->SetStringField(TEXT("direction"), DirectionText);
        Row->SetStringField(TEXT("rule"), RuleText);
        Results.Add(MakeShared<FJsonValueObject>(Row));
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("changed"), Results.Num());
    SaveWidgetIfRequested(WB, Args, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetActionBarButtonClassImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (FSageToolDispatch::FOutcome Reject; detail::RejectIfPie(Reject)) return Reject;
    UWidgetBlueprint* WB = nullptr;
    UWidget* Widget = ResolveWidgetFromArgs(Args, WB);
    if (!WB || !Widget) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("widget not found"));
    const FString ClassPath = FirstWidgetStringArg(Args, {TEXT("button_class"), TEXT("class"), TEXT("value")});
    if (ClassPath.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'button_class'"));
    FProperty* Prop = Widget->GetClass()->FindPropertyByName(TEXT("ActionButtonClass"));
    if (!Prop) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("ActionButtonClass property not found on widget"));
    FScopedTransaction Tx(LOCTEXT("SageSetActionBarButtonClass", "Sage: Set Action Bar Button Class"));
    Widget->Modify();
    if (!detail::SetUPropertyFromJson(Widget, Prop, MakeShared<FJsonValueString>(ClassPath)))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("failed to set ActionButtonClass"));
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(WB);
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("widget"), Widget->GetName());
    R->SetStringField(TEXT("button_class"), ClassPath);
    SaveWidgetIfRequested(WB, Args, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ReparentWidgetRootImpl(const TSharedPtr<FJsonObject>& Args)
{
    if (FSageToolDispatch::FOutcome Reject; detail::RejectIfPie(Reject)) return Reject;
    UWidgetBlueprint* WB = ResolveWidgetBlueprintFromArgs(Args);
    if (!WB || !WB->WidgetTree) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("WidgetBlueprint not found"));
    const FString RootName = FirstWidgetStringArg(Args, {TEXT("root"), TEXT("widget"), TEXT("name")});
    if (RootName.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'root'"));
    UWidget* NewRoot = WB->WidgetTree->FindWidget(FName(*RootName));
    if (!NewRoot) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("root widget not found"));
    FScopedTransaction Tx(LOCTEXT("SageReparentWidgetRoot", "Sage: Reparent Widget Root"));
    WB->WidgetTree->Modify();
    WB->WidgetTree->RootWidget = NewRoot;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WB);
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("root"), NewRoot->GetName());
    SaveWidgetIfRequested(WB, Args, R);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome DumpBlueprintCompileLogImpl(const TSharedPtr<FJsonObject>& Args)
{
    UWidgetBlueprint* WB = ResolveWidgetBlueprintFromArgs(Args);
    if (!WB) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("WidgetBlueprint not found"));
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), WB->GetPathName());
    R->SetNumberField(TEXT("status"), static_cast<int32>(WB->Status));
    R->SetBoolField(TEXT("has_generated_class"), WB->GeneratedClass != nullptr);
    R->SetStringField(TEXT("generated_class"), WB->GeneratedClass ? WB->GeneratedClass->GetPathName() : FString());
    R->SetBoolField(TEXT("compile_log_available"), false);
    R->SetStringField(TEXT("compile_log_source"), TEXT("editor message log is not captured without triggering a compile"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome WidgetEditImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Action = FirstWidgetStringArg(Args, {TEXT("action"), TEXT("op")});
    if (Action == TEXT("set_property")) return SetWidgetPropertyImpl(Args);
    if (Action == TEXT("add_widget")) return AddWidgetImpl(Args);
    if (Action == TEXT("remove_widget")) return RemoveWidgetImpl(Args);
    if (Action == TEXT("rename_widget")) return RenameWidgetParityImpl(Args);
    if (Action == TEXT("set_navigation")) return SetWidgetNavigationBulkImpl(Args);
    return WidgetUnsupported(TEXT("widget_edit"), TEXT("unknown action; supported: set_property/add_widget/remove_widget/rename_widget/set_navigation"));
}

FSageToolDispatch::FOutcome DispatchWidgetParityTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("build_ui_from_spec")) return BuildUiFromSpecImpl(Args);
    if (Tool == TEXT("dump_ui_spec_schema")) return DumpUiSpecSchemaImpl(Args);
    if (Tool == TEXT("dump_ui_spec")) return DumpUiSpecImpl(Args);
    if (Tool == TEXT("rename_widget")) return RenameWidgetParityImpl(Args);
    if (Tool == TEXT("add_widget_variable")) return SetWidgetIsVariableImpl(Args);
    if (Tool == TEXT("audit_focus_chain")) return AuditFocusChainImpl(Args);
    if (Tool == TEXT("apply_token_binding")) return ApplyTokenBindingImpl(Args);
    if (Tool == TEXT("list_widget_property_enums")) return ListWidgetPropertyEnumsImpl(Args);
    if (Tool == TEXT("set_action_bar_button_class")) return SetActionBarButtonClassImpl(Args);
    if (Tool == TEXT("dump_blueprint_compile_log")) return DumpBlueprintCompileLogImpl(Args);
    if (Tool == TEXT("reparent_widget_root")) return ReparentWidgetRootImpl(Args);
    if (Tool == TEXT("set_widget_is_variable")) return SetWidgetIsVariableImpl(Args);
    if (Tool == TEXT("set_widget_navigation_bulk")) return SetWidgetNavigationBulkImpl(Args);
    if (Tool == TEXT("dump_widget_navigation")) return DumpWidgetNavigationImpl(Args);
    if (Tool == TEXT("widget_inspect")) return ReadWidgetImpl(Args);
    if (Tool == TEXT("widget_edit")) return WidgetEditImpl(Args);
    if (Tool == TEXT("convert_textblock_to_common") || Tool == TEXT("convert_border_to_common"))
    {
        return WidgetUnsupported(Tool, TEXT("class conversion must clone slots, bindings, animations, and named references; safe subtree conversion is not mapped yet"));
    }
    return WidgetUnsupported(Tool, TEXT("no dispatch mapping"));
}

}  // namespace (anonymous)

void RegisterWidgetTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("widget.create"),        GT(&CreateWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.list"),          GT(&ListWidgetsImpl));
    Dispatch.RegisterHandler(TEXT("widget.read"),          GT(&ReadWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.add_widget"),    GT(&AddWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.remove_widget"), GT(&RemoveWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.set_property"),  GT(&SetWidgetPropertyImpl));

    // Trailing widget tools
    Dispatch.RegisterHandler(TEXT("widget.get_details"),            GT(&GetWidgetDetailsImpl));
    Dispatch.RegisterHandler(TEXT("widget.read_animations"),        GT(&ReadWidgetAnimationsImpl));
    Dispatch.RegisterHandler(TEXT("widget.create_utility_widget"),  GT(&CreateUtilityWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.run_utility_widget"),     GT(&RunUtilityWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.create_utility_blueprint"),GT(&CreateUtilityBlueprintImpl));
    Dispatch.RegisterHandler(TEXT("widget.run_utility_blueprint"),  GT(&RunUtilityBlueprintImpl));
    Dispatch.RegisterHandler(TEXT("widget.move_widget"),            GT(&MoveWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.list_classes"),           GT(&ListWidgetClassesImpl));
    Dispatch.RegisterHandler(TEXT("widget.list_runtime"),           GT(&ListRuntimeWidgetsImpl));
    Dispatch.RegisterHandler(TEXT("widget.get_runtime"),            GT(&GetRuntimeWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.get_runtime_delegates"),  GT(&GetRuntimeDelegatesImpl));

    // Widget animation authoring (Phase 4.11-r3 — CommonAIExport parity)
    Dispatch.RegisterHandler(TEXT("widget.anim.create"),       GT(&AnimCreateImpl));
    Dispatch.RegisterHandler(TEXT("widget.anim.bind"),         GT(&AnimBindImpl));
    Dispatch.RegisterHandler(TEXT("widget.anim.add_track"),    GT(&AnimAddTrackImpl));
    Dispatch.RegisterHandler(TEXT("widget.anim.add_keyframe"), GT(&AnimAddKeyframeImpl));

    static const TCHAR* WidgetParityTools[] = {
        TEXT("build_ui_from_spec"),
        TEXT("dump_ui_spec_schema"),
        TEXT("dump_ui_spec"),
        TEXT("rename_widget"),
        TEXT("add_widget_variable"),
        TEXT("audit_focus_chain"),
        TEXT("apply_token_binding"),
        TEXT("list_widget_property_enums"),
        TEXT("convert_textblock_to_common"),
        TEXT("convert_border_to_common"),
        TEXT("set_action_bar_button_class"),
        TEXT("dump_blueprint_compile_log"),
        TEXT("reparent_widget_root"),
        TEXT("set_widget_is_variable"),
        TEXT("set_widget_navigation_bulk"),
        TEXT("dump_widget_navigation"),
        TEXT("widget_inspect"),
        TEXT("widget_edit"),
    };
    for (const TCHAR* ToolName : WidgetParityTools)
    {
        Dispatch.RegisterHandler(ToolName,
            [Tool = FString(ToolName)](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
            {
                return detail::RunOnGameThread([&]() -> FSageToolDispatch::FOutcome
                {
                    return DispatchWidgetParityTool(Tool, Args);
                });
            });
    }
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
