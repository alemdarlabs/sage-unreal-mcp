#include "Tools/SageWidgetTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

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
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
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
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
