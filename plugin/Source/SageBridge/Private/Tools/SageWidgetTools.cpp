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

    Dispatch.RegisterHandler(TEXT("widget.create"), GT(&CreateWidgetImpl));
    Dispatch.RegisterHandler(TEXT("widget.list"),   GT(&ListWidgetsImpl));
    Dispatch.RegisterHandler(TEXT("widget.read"),   GT(&ReadWidgetImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
