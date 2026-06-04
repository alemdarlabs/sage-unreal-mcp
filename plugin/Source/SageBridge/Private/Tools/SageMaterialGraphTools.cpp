#include "Tools/SageMaterialGraphTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionConstantBiasScale.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstance.h"
#include "MaterialEditingLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/Base64.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "SageMatGraph"

namespace sage::tools
{
namespace
{

UMaterial* ResolveMaterial(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Cast<UMaterial>(Obj);
}

UMaterialInstanceConstant* ResolveMaterialInstance(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Cast<UMaterialInstanceConstant>(Obj);
}

UMaterialInterface* ResolveMaterialInterface(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Cast<UMaterialInterface>(Obj);
}

UMaterialExpression* FindExpressionByGuid(UMaterial* M, const FString& GuidStr)
{
    if (!M || !M->GetEditorOnlyData()) return nullptr;
    FGuid Guid;
    if (!FGuid::Parse(GuidStr, Guid)) return nullptr;
    for (UMaterialExpression* E : M->GetEditorOnlyData()->ExpressionCollection.Expressions)
    {
        if (E && E->MaterialExpressionGuid == Guid) return E;
    }
    return nullptr;
}

const TCHAR* DomainName(EMaterialDomain D)
{
    switch (D)
    {
    case MD_Surface:           return TEXT("Surface");
    case MD_DeferredDecal:     return TEXT("DeferredDecal");
    case MD_LightFunction:     return TEXT("LightFunction");
    case MD_Volume:            return TEXT("Volume");
    case MD_PostProcess:       return TEXT("PostProcess");
    case MD_UI:                return TEXT("UI");
    default:                   return TEXT("Unknown");
    }
}

const TCHAR* BlendModeName(EBlendMode B)
{
    switch (B)
    {
    case BLEND_Opaque:        return TEXT("Opaque");
    case BLEND_Masked:        return TEXT("Masked");
    case BLEND_Translucent:   return TEXT("Translucent");
    case BLEND_Additive:      return TEXT("Additive");
    case BLEND_Modulate:      return TEXT("Modulate");
    case BLEND_AlphaComposite:return TEXT("AlphaComposite");
    case BLEND_AlphaHoldout:  return TEXT("AlphaHoldout");
    default:                  return TEXT("Unknown");
    }
}

const TCHAR* ShadingModelName(EMaterialShadingModel M)
{
    switch (M)
    {
    case MSM_Unlit:                return TEXT("Unlit");
    case MSM_DefaultLit:           return TEXT("DefaultLit");
    case MSM_Subsurface:           return TEXT("Subsurface");
    case MSM_PreintegratedSkin:    return TEXT("PreintegratedSkin");
    case MSM_ClearCoat:            return TEXT("ClearCoat");
    case MSM_SubsurfaceProfile:    return TEXT("SubsurfaceProfile");
    case MSM_TwoSidedFoliage:      return TEXT("TwoSidedFoliage");
    case MSM_Hair:                 return TEXT("Hair");
    case MSM_Cloth:                return TEXT("Cloth");
    case MSM_Eye:                  return TEXT("Eye");
    case MSM_SingleLayerWater:     return TEXT("SingleLayerWater");
    case MSM_ThinTranslucent:      return TEXT("ThinTranslucent");
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
    // UE 5.7 renamed Strata display name to "Substrate" (enum still MSM_Strata, hidden).
    case MSM_Strata:               return TEXT("Substrate");
#else
    case MSM_Strata:               return TEXT("Strata");
#endif
    default:                       return TEXT("Unknown");
    }
}

EMaterialShadingModel ParseShadingModel(const FString& Name)
{
    if (Name == TEXT("Unlit"))             return MSM_Unlit;
    if (Name == TEXT("DefaultLit"))        return MSM_DefaultLit;
    if (Name == TEXT("Subsurface"))        return MSM_Subsurface;
    if (Name == TEXT("PreintegratedSkin")) return MSM_PreintegratedSkin;
    if (Name == TEXT("ClearCoat"))         return MSM_ClearCoat;
    if (Name == TEXT("SubsurfaceProfile")) return MSM_SubsurfaceProfile;
    if (Name == TEXT("TwoSidedFoliage"))   return MSM_TwoSidedFoliage;
    if (Name == TEXT("Hair"))              return MSM_Hair;
    if (Name == TEXT("Cloth"))             return MSM_Cloth;
    if (Name == TEXT("Eye"))               return MSM_Eye;
    if (Name == TEXT("SingleLayerWater"))  return MSM_SingleLayerWater;
    if (Name == TEXT("ThinTranslucent"))   return MSM_ThinTranslucent;
    if (Name == TEXT("Substrate") || Name == TEXT("Strata")) return MSM_Strata;
    return MSM_DefaultLit;
}

// ---- mat.read -------------------------------------------------------------

FSageToolDispatch::FOutcome MatReadImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UMaterialInterface* MI = ResolveMaterialInterface(Path);
    if (!MI) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("name"), MI->GetName());
    R->SetStringField(TEXT("path"), FSoftObjectPath(MI).ToString());
    R->SetStringField(TEXT("class"), MI->GetClass()->GetName());
    R->SetBoolField  (TEXT("is_instance"), MI->IsA<UMaterialInstance>());

    if (UMaterial* M = MI->GetMaterial())
    {
        R->SetStringField(TEXT("base_material"), M->GetName());
        R->SetStringField(TEXT("domain"),       DomainName((EMaterialDomain)M->MaterialDomain));
        R->SetStringField(TEXT("blend_mode"),   BlendModeName((EBlendMode)M->BlendMode));
        R->SetStringField(TEXT("shading_model"),
            ShadingModelName(M->GetShadingModels().GetFirstShadingModel()));
        R->SetBoolField  (TEXT("two_sided"),    M->TwoSided);
        if (M->GetEditorOnlyData())
        {
            R->SetNumberField(TEXT("expression_count"),
                M->GetEditorOnlyData()->ExpressionCollection.Expressions.Num());
        }
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.list_parameters --------------------------------------------------

FSageToolDispatch::FOutcome MatListParamsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UMaterialInterface* MI = ResolveMaterialInterface(Path);
    if (!MI) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));

    TArray<FMaterialParameterInfo> Scalars, Vectors, Textures, StaticSwitches;
    TArray<FGuid> SG;
    MI->GetAllScalarParameterInfo(Scalars, SG);
    MI->GetAllVectorParameterInfo(Vectors, SG);
    MI->GetAllTextureParameterInfo(Textures, SG);
    MI->GetAllStaticSwitchParameterInfo(StaticSwitches, SG);

    auto Add = [&](const FMaterialParameterInfo& I, const TCHAR* Kind, TArray<TSharedPtr<FJsonValue>>& Out) {
        auto Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), I.Name.ToString());
        Obj->SetStringField(TEXT("kind"), Kind);
        Out.Add(MakeShared<FJsonValueObject>(Obj));
    };

    TArray<TSharedPtr<FJsonValue>> Out;
    for (const auto& I : Scalars)        Add(I, TEXT("scalar"), Out);
    for (const auto& I : Vectors)        Add(I, TEXT("vector"), Out);
    for (const auto& I : Textures)       Add(I, TEXT("texture"), Out);
    for (const auto& I : StaticSwitches) Add(I, TEXT("static_switch"), Out);

    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("parameters"), Out);
    R->SetNumberField(TEXT("count"), Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.list_expressions / mat.read_graph -------------------------------

TSharedRef<FJsonObject> ExpressionToJson(const UMaterialExpression* E)
{
    auto Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("id"),    E->MaterialExpressionGuid.ToString());
    Obj->SetStringField(TEXT("class"), E->GetClass()->GetName());
    Obj->SetNumberField(TEXT("x"),     E->MaterialExpressionEditorX);
    Obj->SetNumberField(TEXT("y"),     E->MaterialExpressionEditorY);
    if (!E->Desc.IsEmpty()) Obj->SetStringField(TEXT("desc"), E->Desc);

    if (auto* SP = Cast<UMaterialExpressionScalarParameter>(E))
    {
        Obj->SetStringField(TEXT("parameter"), SP->ParameterName.ToString());
        Obj->SetNumberField(TEXT("default"),   SP->DefaultValue);
    }
    else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(E))
    {
        Obj->SetStringField(TEXT("parameter"), VP->ParameterName.ToString());
        TArray<TSharedPtr<FJsonValue>> RGBA;
        RGBA.Add(MakeShared<FJsonValueNumber>(VP->DefaultValue.R));
        RGBA.Add(MakeShared<FJsonValueNumber>(VP->DefaultValue.G));
        RGBA.Add(MakeShared<FJsonValueNumber>(VP->DefaultValue.B));
        RGBA.Add(MakeShared<FJsonValueNumber>(VP->DefaultValue.A));
        Obj->SetArrayField(TEXT("default"), RGBA);
    }
    else if (auto* C = Cast<UMaterialExpressionConstant>(E))
    {
        Obj->SetNumberField(TEXT("value"), C->R);
    }
    else if (auto* C3 = Cast<UMaterialExpressionConstant3Vector>(E))
    {
        TArray<TSharedPtr<FJsonValue>> RGB;
        RGB.Add(MakeShared<FJsonValueNumber>(C3->Constant.R));
        RGB.Add(MakeShared<FJsonValueNumber>(C3->Constant.G));
        RGB.Add(MakeShared<FJsonValueNumber>(C3->Constant.B));
        Obj->SetArrayField(TEXT("value"), RGB);
    }
    return Obj;
}

FSageToolDispatch::FOutcome MatListExpressionsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("base material not found"));
    if (!M->GetEditorOnlyData())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("material has no editor data"));
    }

    TArray<TSharedPtr<FJsonValue>> Out;
    for (UMaterialExpression* E : M->GetEditorOnlyData()->ExpressionCollection.Expressions)
    {
        if (E) Out.Add(MakeShared<FJsonValueObject>(ExpressionToJson(E)));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("expressions"), Out);
    R->SetNumberField(TEXT("count"), Out.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.create_instance --------------------------------------------------

FSageToolDispatch::FOutcome MatCreateInstanceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Parent, Dest;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("parent"), Parent)
        || !Args->TryGetStringField(TEXT("destination"), Dest))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'parent' or 'destination'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterialInterface* P = ResolveMaterialInterface(Parent);
    if (!P) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("parent not found"));

    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = P;
    FAssetToolsModule& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    const FString BaseName = FPaths::GetBaseFilename(Dest);
    const FString PkgPath  = FPaths::GetPath(Dest);
    UObject* NewObj = AT.Get().CreateAsset(BaseName, PkgPath,
        UMaterialInstanceConstant::StaticClass(), Factory);
    UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(NewObj);
    if (!MIC) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("CreateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("instance"), FSoftObjectPath(MIC).ToString());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.set_shading_model -----------------------------------------------

FSageToolDispatch::FOutcome MatSetShadingModelImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, ModelName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("model"), ModelName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'model'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("base material not found"));

    FScopedTransaction Tx(LOCTEXT("MatShading", "Sage: Set Shading Model"));
    M->PreEditChange(nullptr);
    if (UMaterialEditorOnlyData* EOD = M->GetEditorOnlyData()) EOD->Modify();
    M->Modify();
    M->SetShadingModel(ParseShadingModel(ModelName));
    M->PostEditChange();
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"), M->GetName());
    R->SetStringField(TEXT("shading_model"), ModelName);
    R->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.set_base_color ---------------------------------------------------

FSageToolDispatch::FOutcome MatSetBaseColorImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    const TArray<TSharedPtr<FJsonValue>>* ColArr = nullptr;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetArrayField(TEXT("color"), ColArr)
        || ColArr->Num() < 3)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'color' [r,g,b,a?]"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));
    if (!M->GetEditorOnlyData())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("material has no editor data"));
    }

    const float R = (*ColArr)[0]->AsNumber();
    const float G = (*ColArr)[1]->AsNumber();
    const float B = (*ColArr)[2]->AsNumber();
    const float A = ColArr->Num() > 3 ? (*ColArr)[3]->AsNumber() : 1.0f;

    FScopedTransaction Tx(LOCTEXT("MatBaseColor", "Sage: Set Material Base Color"));
    // Modify discipline: editor-only data won't transact unless explicitly marked.
    M->PreEditChange(nullptr);
    if (UMaterialEditorOnlyData* EOD = M->GetEditorOnlyData()) EOD->Modify();
    M->Modify();

    UMaterialExpressionConstant3Vector* C3 = NewObject<UMaterialExpressionConstant3Vector>(M);
    C3->Constant = FLinearColor(R, G, B, A);
    C3->MaterialExpressionEditorX = -300;
    C3->MaterialExpressionEditorY = 0;
    C3->MaterialExpressionGuid = FGuid::NewGuid();
    M->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(C3);

    M->GetEditorOnlyData()->BaseColor.Expression = C3;
    M->GetEditorOnlyData()->BaseColor.OutputIndex = 0;
    M->PostEditChange();
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("expression_id"), C3->MaterialExpressionGuid.ToString());
    Out->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
}

// ---- mat.add_expression ---------------------------------------------------

FSageToolDispatch::FOutcome MatAddExpressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, ExprClassName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("expression_class"), ExprClassName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'expression_class'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));

    UClass* ExprClass = FindObject<UClass>(nullptr, *ExprClassName);
    if (!ExprClass)
    {
        FSoftObjectPath SP(ExprClassName);
        ExprClass = Cast<UClass>(SP.TryLoad());
    }
    if (!ExprClass || !ExprClass->IsChildOf(UMaterialExpression::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("expression class invalid: %s"), *ExprClassName));
    }

    int32 X = 0, Y = 0;
    Args->TryGetNumberField(TEXT("x"), X);
    Args->TryGetNumberField(TEXT("y"), Y);

    FScopedTransaction Tx(LOCTEXT("MatAddExpr", "Sage: Add Material Expression"));
    M->Modify();
    UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(M, ExprClass, X, Y);
    if (!Expr) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("CreateMaterialExpression failed"));

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("expression_id"), Expr->MaterialExpressionGuid.ToString());
    Out->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
}

// ---- mat.delete_expression -----------------------------------------------

FSageToolDispatch::FOutcome MatDeleteExpressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, NodeId;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("expression_id"), NodeId))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'expression_id'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));
    UMaterialExpression* E = FindExpressionByGuid(M, NodeId);
    if (!E) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("expression not found"));

    FScopedTransaction Tx(LOCTEXT("MatDelExpr", "Sage: Delete Material Expression"));
    M->Modify();
    UMaterialEditingLibrary::DeleteMaterialExpression(M, E);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("expression_id"), NodeId);
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
}

// ---- mat.connect_expressions ---------------------------------------------

FSageToolDispatch::FOutcome MatConnectExprsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FromId, ToId, FromOut, ToIn;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("from_id"), FromId)
        || !Args->TryGetStringField(TEXT("to_id"),   ToId)
        || !Args->TryGetStringField(TEXT("from_output"), FromOut)
        || !Args->TryGetStringField(TEXT("to_input"),    ToIn))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing connection arguments"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));
    UMaterialExpression* From = FindExpressionByGuid(M, FromId);
    UMaterialExpression* To   = FindExpressionByGuid(M, ToId);
    if (!From || !To) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("expression(s) not found"));

    FScopedTransaction Tx(LOCTEXT("MatConnect", "Sage: Connect Material Expressions"));
    M->Modify();
    if (!UMaterialEditingLibrary::ConnectMaterialExpressions(From, FromOut, To, ToIn))
    {
        // ConnectMaterialExpressions performs internal type-compat checks; failure
        // typically means the named output/input doesn't exist or types collide.
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("connect failed: output/input name mismatch or incompatible types"));
    }
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
}

// ---- mat.connect_to_property ---------------------------------------------

FSageToolDispatch::FOutcome MatConnectToPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, FromId, FromOut, PropName;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("from_id"), FromId)
        || !Args->TryGetStringField(TEXT("from_output"), FromOut)
        || !Args->TryGetStringField(TEXT("property"), PropName))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'from_id', 'from_output', or 'property'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));
    UMaterialExpression* From = FindExpressionByGuid(M, FromId);
    if (!From) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("expression not found"));

    EMaterialProperty Prop = MP_BaseColor;
    if      (PropName == TEXT("BaseColor"))   Prop = MP_BaseColor;
    else if (PropName == TEXT("Metallic"))    Prop = MP_Metallic;
    else if (PropName == TEXT("Roughness"))   Prop = MP_Roughness;
    else if (PropName == TEXT("Specular"))    Prop = MP_Specular;
    else if (PropName == TEXT("Emissive") || PropName == TEXT("EmissiveColor"))
                                              Prop = MP_EmissiveColor;
    else if (PropName == TEXT("Normal"))      Prop = MP_Normal;
    else if (PropName == TEXT("Opacity"))     Prop = MP_Opacity;
    else if (PropName == TEXT("OpacityMask")) Prop = MP_OpacityMask;
    else if (PropName == TEXT("WorldPositionOffset"))
                                              Prop = MP_WorldPositionOffset;
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unknown material property: %s"), *PropName));
    }

    FScopedTransaction Tx(LOCTEXT("MatConnectProp", "Sage: Connect Material to Property"));
    M->Modify();
    if (!UMaterialEditingLibrary::ConnectMaterialProperty(From, FromOut, Prop))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("connect failed"));
    }
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("property"), PropName);
    Out->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
}

// ---- mat.set_expression_value (constant scalar / 3vec / 4vec) ------------

FSageToolDispatch::FOutcome MatSetExpressionValueImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, NodeId;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("expression_id"), NodeId))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path' or 'expression_id'"));
    }
    auto It = Args->Values.Find(TEXT("value"));
    if (!It) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));
    UMaterialExpression* E = FindExpressionByGuid(M, NodeId);
    if (!E) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("expression not found"));

    FScopedTransaction Tx(LOCTEXT("MatSetExprVal", "Sage: Set Material Expression Value"));
    M->PreEditChange(nullptr);
    if (UMaterialEditorOnlyData* EOD = M->GetEditorOnlyData()) EOD->Modify();
    M->Modify();
    E->Modify();

    auto AsLinearColor = [](const TSharedPtr<FJsonValue>& V, FLinearColor& Out, int32 MinComponents) -> bool
    {
        if (V->Type != EJson::Array) return false;
        const auto& A = V->AsArray();
        if (A.Num() < MinComponents) return false;
        Out = FLinearColor(
            static_cast<float>(A[0]->AsNumber()),
            static_cast<float>(A[1]->AsNumber()),
            static_cast<float>(A[2]->AsNumber()),
            A.Num() > 3 ? static_cast<float>(A[3]->AsNumber()) : 1.0f);
        return true;
    };

    if (auto* C = Cast<UMaterialExpressionConstant>(E))
    {
        C->R = static_cast<float>((*It)->AsNumber());
    }
    else if (auto* C3 = Cast<UMaterialExpressionConstant3Vector>(E))
    {
        FLinearColor Col;
        if (!AsLinearColor(*It, Col, 3))
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("Constant3Vector requires array [r,g,b]"));
        C3->Constant = Col;
    }
    else if (auto* C4 = Cast<UMaterialExpressionConstant4Vector>(E))
    {
        FLinearColor Col;
        if (!AsLinearColor(*It, Col, 4))
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("Constant4Vector requires array [r,g,b,a]"));
        C4->Constant = Col;
    }
    else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(E))
    {
        FLinearColor Col;
        if (!AsLinearColor(*It, Col, 3))
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("VectorParameter requires array [r,g,b] or [r,g,b,a]"));
        VP->DefaultValue = Col;
    }
    else if (auto* SP = Cast<UMaterialExpressionScalarParameter>(E))
    {
        SP->DefaultValue = static_cast<float>((*It)->AsNumber());
    }
    else if (auto* CBS = Cast<UMaterialExpressionConstantBiasScale>(E))
    {
        // Accepts {bias:N, scale:M} object or array [bias, scale].
        if ((*It)->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject>& O = (*It)->AsObject();
            double Bias = CBS->Bias, Scale = CBS->Scale;
            O->TryGetNumberField(TEXT("bias"),  Bias);
            O->TryGetNumberField(TEXT("scale"), Scale);
            CBS->Bias  = static_cast<float>(Bias);
            CBS->Scale = static_cast<float>(Scale);
        }
        else if ((*It)->Type == EJson::Array)
        {
            const auto& A = (*It)->AsArray();
            if (A.Num() < 2) return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("ConstantBiasScale requires [bias, scale]"));
            CBS->Bias  = static_cast<float>(A[0]->AsNumber());
            CBS->Scale = static_cast<float>(A[1]->AsNumber());
        }
        else
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                TEXT("ConstantBiasScale value must be {bias,scale} object or [bias,scale] array"));
        }
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unsupported expression class for value set: %s"),
                            *E->GetClass()->GetName()));
    }
    E->PostEditChange();
    M->PostEditChange();
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("expression_id"), NodeId);
    Out->SetField(TEXT("value"), *It);
    Out->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
}

// ---- mat.connect_texture (texture sample expression with a UTexture asset)

FSageToolDispatch::FOutcome MatConnectTextureImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path, NodeId, TexPath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"), Path)
        || !Args->TryGetStringField(TEXT("expression_id"), NodeId)
        || !Args->TryGetStringField(TEXT("texture"), TexPath))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("missing 'path', 'expression_id', or 'texture'"));
    }
    FSageToolDispatch::FOutcome PieErr;
    if (detail::RejectIfPie(PieErr)) return PieErr;
    UMaterial* M = ResolveMaterial(Path);
    if (!M) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));
    UMaterialExpression* E = FindExpressionByGuid(M, NodeId);
    if (!E) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("expression not found"));

    UTexture* Tex = Cast<UTexture>(FSoftObjectPath(TexPath).TryLoad());
    if (!Tex) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("texture not found"));

    FScopedTransaction Tx(LOCTEXT("MatConnectTex", "Sage: Connect Material Texture"));
    M->Modify();
    auto* Sampler = Cast<UMaterialExpressionTextureBase>(E);
    if (!Sampler) return FSageToolDispatch::FOutcome::MakeError(-32602,
        TEXT("expression is not a TextureSample / TextureBase"));
    Sampler->Texture = Tex;
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("expression_id"), NodeId);
    Out->SetStringField(TEXT("texture"), Tex->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
}

// ---- mat.validate (recompile + return stats) ------------------------------

FSageToolDispatch::FOutcome MatValidateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UMaterialInterface* MI = ResolveMaterialInterface(Path);
    if (!MI) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));

    if (UMaterial* M = Cast<UMaterial>(MI))
    {
        UMaterialEditingLibrary::RecompileMaterial(M);
    }
    else if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(MI))
    {
        UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
    }

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), MI->GetName());
    Out->SetBoolField  (TEXT("recompiled"), true);
    Out->SetStringField(TEXT("_status"),
        TEXT("queued; UE shader compile is async, validation result not synchronous"));
    Out->SetStringField(TEXT("_compile_warning"),
        TEXT("poll mat.get_shader_stats or watch editor log for compilation completion"));
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
}

// ---- mat.create ------------------------------------------------------------

FSageToolDispatch::FOutcome MatCreateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("path must be /Folder/Name form"));

    if (FindPackage(nullptr, *(PackagePath / AssetName)))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset already exists"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

    // Resolve UMaterialFactoryNew explicitly; if the engine module hasn't loaded
    // it yet (rare in headless/build-machine flows) we surface a descriptive
    // error rather than letting CreateAsset return null with no clue.
    UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
    if (!Factory)
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("UMaterialFactoryNew unavailable (engine material module not loaded)"));

    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath,
        UMaterial::StaticClass(), Factory);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("CreateAsset failed"));

    UMaterial* Mat = Cast<UMaterial>(NewObj);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32603,
        TEXT("CreateAsset returned non-UMaterial (factory mismatch)"));

    FString ShadingStr;
    if (Args->TryGetStringField(TEXT("shading_model"), ShadingStr))
    {
        if      (ShadingStr == TEXT("Unlit"))      Mat->SetShadingModel(MSM_Unlit);
        else if (ShadingStr == TEXT("DefaultLit"))  Mat->SetShadingModel(MSM_DefaultLit);
        else if (ShadingStr == TEXT("Subsurface"))  Mat->SetShadingModel(MSM_Subsurface);
        else if (ShadingStr == TEXT("TwoSidedFoliage")) Mat->SetShadingModel(MSM_TwoSidedFoliage);
    }

    FString BlendStr;
    if (Args->TryGetStringField(TEXT("blend_mode"), BlendStr))
    {
        if      (BlendStr == TEXT("Masked"))       Mat->BlendMode = BLEND_Masked;
        else if (BlendStr == TEXT("Translucent"))  Mat->BlendMode = BLEND_Translucent;
        else if (BlendStr == TEXT("Additive"))     Mat->BlendMode = BLEND_Additive;
    }

    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Mat->GetPathName());
    R->SetStringField(TEXT("name"),  Mat->GetName());
    R->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.set_blend_mode ----------------------------------------------------

FSageToolDispatch::FOutcome MatSetBlendModeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, BlendStr;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    // Schema field is 'mode'; legacy alias 'blend_mode' kept for compatibility.
    if (!Args->TryGetStringField(TEXT("mode"), BlendStr) &&
        !Args->TryGetStringField(TEXT("blend_mode"), BlendStr))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'mode'"));

    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));

    EBlendMode Mode = BLEND_Opaque;
    if      (BlendStr == TEXT("Masked"))       Mode = BLEND_Masked;
    else if (BlendStr == TEXT("Translucent"))  Mode = BLEND_Translucent;
    else if (BlendStr == TEXT("Additive"))     Mode = BLEND_Additive;
    else if (BlendStr == TEXT("Modulate"))     Mode = BLEND_Modulate;
    else if (BlendStr == TEXT("AlphaComposite")) Mode = BLEND_AlphaComposite;
    else if (BlendStr != TEXT("Opaque")) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("unknown blend_mode: %s"), *BlendStr));

    FScopedTransaction Tx(LOCTEXT("SetBlend", "Set Blend Mode"));
    Mat->PreEditChange(nullptr);
    if (UMaterialEditorOnlyData* EOD = Mat->GetEditorOnlyData()) EOD->Modify();
    Mat->Modify();
    Mat->BlendMode = Mode;
    Mat->PostEditChange();
    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"),   Mat->GetName());
    R->SetStringField(TEXT("blend_mode"), BlendStr);
    R->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.disconnect --------------------------------------------------------
// Schema: { path, expression?, property? }
//  - If 'property' set: disconnect the named material output (BaseColor/Metallic/...)
//    on the base material. Highest-level disconnect.
//  - If 'expression' set (GUID): disconnect ALL inputs on that expression.
//  - Both may be set together; both honored.
// Returns count of inputs cleared.

FSageToolDispatch::FOutcome MatDisconnectImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));
    if (!Mat->GetEditorOnlyData())
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("material has no editor data"));

    FString ExprId, PropName;
    Args->TryGetStringField(TEXT("expression"), ExprId);
    Args->TryGetStringField(TEXT("property"),   PropName);
    if (ExprId.IsEmpty() && PropName.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("must provide 'expression' (guid) or 'property' (BaseColor/Metallic/...)"));

    FScopedTransaction Tx(LOCTEXT("DisconnectExpr", "Sage: Disconnect Material"));
    Mat->PreEditChange(nullptr);
    if (UMaterialEditorOnlyData* EOD = Mat->GetEditorOnlyData()) EOD->Modify();
    Mat->Modify();

    int32 InputsCleared = 0;
    bool PropertyCleared = false;

    // 1) Disconnect a top-level material property (BaseColor, Metallic, etc.)
    if (!PropName.IsEmpty())
    {
        UMaterialEditorOnlyData* EOD = Mat->GetEditorOnlyData();
        FExpressionInput* Input = nullptr;
        if      (PropName == TEXT("BaseColor"))            Input = &EOD->BaseColor;
        else if (PropName == TEXT("Metallic"))             Input = &EOD->Metallic;
        else if (PropName == TEXT("Roughness"))            Input = &EOD->Roughness;
        else if (PropName == TEXT("Specular"))             Input = &EOD->Specular;
        else if (PropName == TEXT("Emissive") ||
                 PropName == TEXT("EmissiveColor"))        Input = &EOD->EmissiveColor;
        else if (PropName == TEXT("Normal"))               Input = &EOD->Normal;
        else if (PropName == TEXT("Opacity"))              Input = &EOD->Opacity;
        else if (PropName == TEXT("OpacityMask"))          Input = &EOD->OpacityMask;
        else if (PropName == TEXT("WorldPositionOffset"))  Input = &EOD->WorldPositionOffset;
        else if (PropName == TEXT("AmbientOcclusion"))     Input = &EOD->AmbientOcclusion;
        else if (PropName == TEXT("Refraction"))           Input = &EOD->Refraction;
        else
        {
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("unknown material property: %s"), *PropName));
        }
        if (Input && Input->Expression != nullptr)
        {
            Input->Expression  = nullptr;
            Input->OutputIndex = 0;
            Input->Mask = Input->MaskR = Input->MaskG = Input->MaskB = Input->MaskA = 0;
            PropertyCleared = true;
        }
    }

    // 2) Disconnect all inputs on a specific expression node (by GUID).
    if (!ExprId.IsEmpty())
    {
        UMaterialExpression* Expr = FindExpressionByGuid(Mat, ExprId);
        if (!Expr)
            return FSageToolDispatch::FOutcome::MakeError(-32602,
                FString::Printf(TEXT("expression not found: %s"), *ExprId));
        Expr->Modify();
        // FExpressionInputIterator (UE 5.5+ canonical traversal API).
        for (FExpressionInputIterator It{Expr}; It; ++It)
        {
            if (It.Input && It.Input->Expression != nullptr)
            {
                It.Input->Expression  = nullptr;
                It.Input->OutputIndex = 0;
                It.Input->Mask = It.Input->MaskR = It.Input->MaskG = It.Input->MaskB = It.Input->MaskA = 0;
                ++InputsCleared;
            }
        }
        Expr->PostEditChange();
    }

    Mat->PostEditChange();
    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"),         Mat->GetName());
    R->SetBoolField  (TEXT("property_cleared"), PropertyCleared);
    R->SetNumberField(TEXT("inputs_cleared"),   InputsCleared);
    R->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.list_expression_types ---------------------------------------------

FSageToolDispatch::FOutcome MatListExpressionTypesImpl(const TSharedPtr<FJsonObject>& /*Args*/)
{
    TArray<TSharedPtr<FJsonValue>> Types;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Cls = *It;
        if (!Cls || !Cls->IsChildOf(UMaterialExpression::StaticClass())) continue;
        if (Cls->HasAnyClassFlags(CLASS_Abstract)) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),  Cls->GetName());
        J->SetStringField(TEXT("path"),  Cls->GetPathName());
        Types.Add(MakeShared<FJsonValueObject>(J));
        if (Types.Num() >= 500) break;
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField (TEXT("types"), Types);
    R->SetNumberField(TEXT("count"), Types.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.recompile ---------------------------------------------------------

FSageToolDispatch::FOutcome MatRecompileImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));

    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"),   Mat->GetName());
    R->SetBoolField  (TEXT("recompiled"), true);
    R->SetStringField(TEXT("_compile_warning"),
        TEXT("shader compilation runs async; preview may take seconds to update"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.duplicate ---------------------------------------------------------

FSageToolDispatch::FOutcome MatDuplicateImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Source, Destination;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("source"), Source))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'source'"));
    if (!Args->TryGetStringField(TEXT("destination"), Destination))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'destination'"));

    UMaterial* Mat = ResolveMaterial(Source);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("source material not found: %s"), *Source));

    FString DestPackagePath, DestAssetName;
    if (!Destination.Split(TEXT("/"), &DestPackagePath, &DestAssetName,
        ESearchCase::IgnoreCase, ESearchDir::FromEnd))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("destination must be /Folder/Name form"));

    if (FindPackage(nullptr, *(DestPackagePath / DestAssetName)))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("destination already exists"));

    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UObject* NewObj = AT.DuplicateAsset(DestAssetName, DestPackagePath, Mat);
    if (!NewObj) return FSageToolDispatch::FOutcome::MakeError(-32000, TEXT("DuplicateAsset failed"));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("source"),      Source);
    R->SetStringField(TEXT("destination"), NewObj->GetPathName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.get_shader_stats --------------------------------------------------

FSageToolDispatch::FOutcome MatGetShaderStatsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"),        Mat->GetName());
    TArray<UMaterialExpression*> AllExprs;
    for (UMaterialExpression* E : Mat->GetExpressions()) AllExprs.Add(E);
    R->SetNumberField(TEXT("num_expressions"), AllExprs.Num());
    R->SetBoolField(TEXT("instruction_count_available"), false);
    R->SetStringField(TEXT("instruction_count_status"),
        TEXT("FMaterialResource instruction count API changed in UE 5.7; "
             "use editor shader complexity viewport for detailed stats"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.export_graph ------------------------------------------------------

FSageToolDispatch::FOutcome MatExportGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));

    TArray<TSharedPtr<FJsonValue>> Nodes;
    TArray<UMaterialExpression*> Exprs;
    for (UMaterialExpression* E : Mat->GetExpressions()) Exprs.Add(E);
    for (int32 I = 0; I < Exprs.Num(); ++I)
    {
        UMaterialExpression* E = Exprs[I];
        if (!E) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("id"),    I);
        J->SetStringField(TEXT("class"), E->GetClass()->GetName());
        J->SetNumberField(TEXT("x"),     E->MaterialExpressionEditorX);
        J->SetNumberField(TEXT("y"),     E->MaterialExpressionEditorY);
        J->SetStringField(TEXT("desc"),  E->Desc);
        Nodes.Add(MakeShared<FJsonValueObject>(J));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"), Mat->GetName());
    R->SetArrayField (TEXT("nodes"),    Nodes);
    R->SetNumberField(TEXT("count"),    Nodes.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.import_graph ------------------------------------------------------
// Rebuilds a material graph from a JSON spec (nodes + connections).
// This is intentionally a thin wrapper: add_expression + connect_expressions.

FSageToolDispatch::FOutcome MatImportGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));

    const TArray<TSharedPtr<FJsonValue>>* Nodes;
    if (!Args->TryGetArrayField(TEXT("nodes"), Nodes))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'nodes'"));

    FScopedTransaction Tx(LOCTEXT("ImportGraph", "Import Material Graph"));
    Mat->Modify();
    int32 Added = 0;
    for (const TSharedPtr<FJsonValue>& NV : *Nodes)
    {
        const TSharedPtr<FJsonObject>* NObj;
        if (!NV->TryGetObject(NObj)) continue;
        FString ClsPath;
        if (!(*NObj)->TryGetStringField(TEXT("class"), ClsPath)) continue;
        UClass* Cls = FindObject<UClass>(nullptr, *ClsPath);
        if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClsPath);
        if (!Cls || !Cls->IsChildOf(UMaterialExpression::StaticClass())) continue;
        UMaterialExpression* E = UMaterialEditingLibrary::CreateMaterialExpression(
            Mat, Cls, 0, 0);
        if (E) ++Added;
    }
    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"),   Mat->GetName());
    R->SetNumberField(TEXT("nodes_added"), Added);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.build_graph -------------------------------------------------------
// High-level declarative builder: {base_color, metallic, roughness, emissive}
// as constant values → connects them to material properties.

FSageToolDispatch::FOutcome MatBuildGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));

    FScopedTransaction Tx(LOCTEXT("BuildGraph", "Build Material Graph"));
    Mat->Modify();

    auto ConnectConst3 = [&](const FString& FieldName, EMaterialProperty MatProp, int32 X, int32 Y)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr;
        if (!Args->TryGetArrayField(FieldName, Arr) || Arr->Num() < 3) return;
        float R2 = static_cast<float>((*Arr)[0]->AsNumber());
        float G  = static_cast<float>((*Arr)[1]->AsNumber());
        float B  = static_cast<float>((*Arr)[2]->AsNumber());
        UMaterialExpressionConstant3Vector* C3 = Cast<UMaterialExpressionConstant3Vector>(
            UMaterialEditingLibrary::CreateMaterialExpression(
                Mat, UMaterialExpressionConstant3Vector::StaticClass(), X, Y));
        if (!C3) return;
        C3->Constant = FLinearColor(R2, G, B);
        UMaterialEditingLibrary::ConnectMaterialProperty(C3, TEXT(""), MatProp);
    };
    auto ConnectConst = [&](const FString& FieldName, EMaterialProperty MatProp, int32 X, int32 Y)
    {
        double Val;
        if (!Args->TryGetNumberField(FieldName, Val)) return;
        UMaterialExpressionConstant* Const = Cast<UMaterialExpressionConstant>(
            UMaterialEditingLibrary::CreateMaterialExpression(
                Mat, UMaterialExpressionConstant::StaticClass(), X, Y));
        if (!Const) return;
        Const->R = static_cast<float>(Val);
        UMaterialEditingLibrary::ConnectMaterialProperty(Const, TEXT(""), MatProp);
    };

    ConnectConst3(TEXT("base_color"), MP_BaseColor,     -400, -100);
    ConnectConst (TEXT("metallic"),   MP_Metallic,      -400,  100);
    ConnectConst (TEXT("roughness"),  MP_Roughness,     -400,  300);
    ConnectConst3(TEXT("emissive"),   MP_EmissiveColor, -400,  500);

    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"), Mat->GetName());
    R->SetBoolField  (TEXT("built"),    true);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.render_preview ----------------------------------------------------

FSageToolDispatch::FOutcome MatRenderPreviewImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));

    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("material not found: %s"), *Path));

    // Material thumbnail rendering is async in UE — we capture thumbnail path
    const FString ThumbPath = FPaths::ProjectSavedDir() / TEXT("Thumbnails") /
        (Mat->GetName() + TEXT(".png"));

    // Note: full async GPU thumbnail bake not available synchronously without
    // SceneCapture2D actor. Return the thumbnail directory so the caller knows
    // where previews land when UE generates them on demand.
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"),       Mat->GetName());
    R->SetStringField(TEXT("thumbnail_dir"),  FPaths::ProjectSavedDir() / TEXT("Thumbnails"));
    R->SetBoolField(TEXT("synchronous_preview_available"), false);
    R->SetStringField(TEXT("preview_status"), TEXT("thumbnails generated on demand by UE thumbnail system"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- mat.begin_transaction / mat.end_transaction ---------------------------
//
// Per-material scoped transactions keyed by UMaterial*. Multi-editor + concurrent
// authoring on different materials (or two clients hitting the same editor)
// previously stomped a single process-global TOptional<FScopedTransaction>,
// causing one client's commit to silently swallow the other's edits.
//
// Lookup by Material* on commit; if the caller only passes a description (legacy
// single-active path) the most-recent transaction is used. Reset() must run on
// GameThread (FScopedTransaction dtor touches GUndo) — already guaranteed by
// the GT(...) wrapper at registration.

static TMap<TWeakObjectPtr<UMaterial>, TUniquePtr<FScopedTransaction>>& MatTransactions()
{
    static TMap<TWeakObjectPtr<UMaterial>, TUniquePtr<FScopedTransaction>> Map;
    return Map;
}

FSageToolDispatch::FOutcome MatBeginTransactionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Desc = TEXT("Material Graph Edit");
    FString Path;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("description"), Desc);
        Args->TryGetStringField(TEXT("path"),        Path);
    }

    UMaterial* Mat = Path.IsEmpty() ? nullptr : ResolveMaterial(Path);
    if (!Path.IsEmpty() && !Mat)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("material not found: %s"), *Path));

    auto& Map = MatTransactions();
    // Garbage-collect dead WeakObjectPtrs so the map doesn't leak across editor
    // sessions / GC sweeps.
    for (auto It = Map.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid()) It.RemoveCurrent();
    }

    TWeakObjectPtr<UMaterial> Key = Mat;  // null is a valid "global" key
    if (Map.Contains(Key))
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("transaction already active for this material; commit or rollback first"));

    Map.Add(Key, MakeUnique<FScopedTransaction>(FText::FromString(Desc)));

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("started"),     true);
    R->SetStringField(TEXT("description"), Desc);
    if (Mat) R->SetStringField(TEXT("material"), Mat->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome MatEndTransactionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("path"), Path);

    UMaterial* Mat = Path.IsEmpty() ? nullptr : ResolveMaterial(Path);
    if (!Path.IsEmpty() && !Mat)
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("material not found: %s"), *Path));

    auto& Map = MatTransactions();
    TWeakObjectPtr<UMaterial> Key = Mat;
    int32 Removed = Map.Remove(Key);

    auto R = MakeShared<FJsonObject>();
    R->SetBoolField  (TEXT("committed"), Removed > 0);
    R->SetNumberField(TEXT("removed"),   Removed);
    if (Removed == 0)
        R->SetStringField(TEXT("_warn"),
            TEXT("no active transaction for this material/key; nothing committed"));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

static constexpr int32 kMaterialUnsupportedCode = -32005;

FSageToolDispatch::FOutcome MaterialUnsupported(const FString& Tool, const FString& Reason)
{
    return FSageToolDispatch::FOutcome::MakeError(kMaterialUnsupportedCode,
        FString::Printf(TEXT("%s is not exposed as a safe synchronous material operation: %s"),
                        *Tool, *Reason));
}

FString FirstMaterialStringArg(const TSharedPtr<FJsonObject>& Args,
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

bool FirstMaterialBoolArg(const TSharedPtr<FJsonObject>& Args,
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

UMaterialFunction* ResolveMaterialFunction(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Cast<UMaterialFunction>(Obj);
}

UObject* ResolveAnyAsset(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj) Obj = Soft.TryLoad();
    return Obj;
}

TSharedPtr<FJsonValue> ColorToJson(const FLinearColor& Color)
{
    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Add(MakeShared<FJsonValueNumber>(Color.R));
    Values.Add(MakeShared<FJsonValueNumber>(Color.G));
    Values.Add(MakeShared<FJsonValueNumber>(Color.B));
    Values.Add(MakeShared<FJsonValueNumber>(Color.A));
    return MakeShared<FJsonValueArray>(Values);
}

bool JsonToLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& Out)
{
    if (!Value.IsValid() || Value->Type != EJson::Array) return false;
    const TArray<TSharedPtr<FJsonValue>>& A = Value->AsArray();
    if (A.Num() < 3) return false;
    Out = FLinearColor(
        static_cast<float>(A[0]->AsNumber()),
        static_cast<float>(A[1]->AsNumber()),
        static_cast<float>(A[2]->AsNumber()),
        A.Num() > 3 ? static_cast<float>(A[3]->AsNumber()) : 1.0f);
    return true;
}

ECustomMaterialOutputType ParseCustomOutputType(const FString& Text)
{
    if (Text.Equals(TEXT("float2"), ESearchCase::IgnoreCase)) return CMOT_Float2;
    if (Text.Equals(TEXT("float3"), ESearchCase::IgnoreCase)) return CMOT_Float3;
    if (Text.Equals(TEXT("float4"), ESearchCase::IgnoreCase)) return CMOT_Float4;
    if (Text.Equals(TEXT("material_attributes"), ESearchCase::IgnoreCase) ||
        Text.Equals(TEXT("attributes"), ESearchCase::IgnoreCase))
    {
        return CMOT_MaterialAttributes;
    }
    return CMOT_Float1;
}

void ApplyCustomExpressionFields(UMaterialExpressionCustom* Custom,
                                 const TSharedPtr<FJsonObject>& Args,
                                 bool bRequireCode)
{
    FString Text;
    if (Args.IsValid() && Args->TryGetStringField(TEXT("code"), Text))
    {
        Custom->Code = Text;
    }
    else if (bRequireCode)
    {
        Custom->Code = TEXT("return 0;");
    }
    if (Args.IsValid() && Args->TryGetStringField(TEXT("description"), Text))
    {
        Custom->Description = Text;
        Custom->Desc = Text;
    }
    if (Args.IsValid() && Args->TryGetStringField(TEXT("output_type"), Text))
    {
        Custom->OutputType = ParseCustomOutputType(Text);
    }
    const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
    if (Args.IsValid() && Args->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs)
    {
        Custom->Inputs.Empty();
        for (const TSharedPtr<FJsonValue>& V : *Inputs)
        {
            FString Name;
            if (V.IsValid() && V->Type == EJson::String)
            {
                Name = V->AsString();
            }
            else if (V.IsValid() && V->Type == EJson::Object)
            {
                V->AsObject()->TryGetStringField(TEXT("name"), Name);
            }
            if (!Name.IsEmpty())
            {
                FCustomInput& NewInput = Custom->Inputs.AddDefaulted_GetRef();
                NewInput.InputName = FName(*Name);
            }
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* Includes = nullptr;
    if (Args.IsValid() && Args->TryGetArrayField(TEXT("include_paths"), Includes) && Includes)
    {
        Custom->IncludeFilePaths.Empty();
        for (const TSharedPtr<FJsonValue>& V : *Includes)
        {
            if (V.IsValid() && V->Type == EJson::String)
            {
                Custom->IncludeFilePaths.Add(V->AsString());
            }
        }
    }
    Custom->RebuildOutputs();
}

FSageToolDispatch::FOutcome CreateCustomHlslNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("material")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    UMaterial* Mat = ResolveMaterial(Path);
    if (!Mat) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));
    int32 X = 0, Y = 0;
    Args->TryGetNumberField(TEXT("x"), X);
    Args->TryGetNumberField(TEXT("y"), Y);
    FScopedTransaction Tx(LOCTEXT("CreateCustomHlsl", "Sage: Create Custom HLSL Node"));
    Mat->Modify();
    UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(
        UMaterialEditingLibrary::CreateMaterialExpression(Mat, UMaterialExpressionCustom::StaticClass(), X, Y));
    if (!Custom) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("CreateMaterialExpression(Custom) failed"));
    Custom->Modify();
    ApplyCustomExpressionFields(Custom, Args, true);
    Custom->PostEditChange();
    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"), Mat->GetPathName());
    R->SetStringField(TEXT("expression_id"), Custom->MaterialExpressionGuid.ToString());
    R->SetStringField(TEXT("class"), Custom->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome UpdateCustomHlslNodeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("material")});
    const FString Id = FirstMaterialStringArg(Args, {TEXT("expression_id"), TEXT("node_id")});
    if (Path.IsEmpty() || Id.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path' or 'expression_id'"));
    UMaterial* Mat = ResolveMaterial(Path);
    UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(FindExpressionByGuid(Mat, Id));
    if (!Custom) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("custom expression not found"));
    FScopedTransaction Tx(LOCTEXT("UpdateCustomHlsl", "Sage: Update Custom HLSL Node"));
    Mat->Modify();
    Custom->Modify();
    ApplyCustomExpressionFields(Custom, Args, false);
    Custom->PostEditChange();
    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"), Mat->GetPathName());
    R->SetStringField(TEXT("expression_id"), Custom->MaterialExpressionGuid.ToString());
    R->SetNumberField(TEXT("input_count"), Custom->Inputs.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome MoveExpressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("material")});
    const FString Id = FirstMaterialStringArg(Args, {TEXT("expression_id"), TEXT("node_id")});
    int32 X = 0, Y = 0;
    if (Path.IsEmpty() || Id.IsEmpty() ||
        !Args->TryGetNumberField(TEXT("x"), X) ||
        !Args->TryGetNumberField(TEXT("y"), Y))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing path/expression_id/x/y"));
    }
    UMaterial* Mat = ResolveMaterial(Path);
    UMaterialExpression* Expr = FindExpressionByGuid(Mat, Id);
    if (!Expr) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("expression not found"));
    FScopedTransaction Tx(LOCTEXT("MoveMaterialExpression", "Sage: Move Material Expression"));
    Expr->Modify();
    Expr->MaterialExpressionEditorX = X;
    Expr->MaterialExpressionEditorY = Y;
    Mat->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("expression_id"), Id);
    R->SetNumberField(TEXT("x"), X);
    R->SetNumberField(TEXT("y"), Y);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome RenameExpressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("material")});
    const FString Id = FirstMaterialStringArg(Args, {TEXT("expression_id"), TEXT("node_id")});
    const FString NewName = FirstMaterialStringArg(Args, {TEXT("new_name"), TEXT("name"), TEXT("to")});
    if (Path.IsEmpty() || Id.IsEmpty() || NewName.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing path/expression_id/new_name"));
    }
    UMaterial* Mat = ResolveMaterial(Path);
    UMaterialExpression* Expr = FindExpressionByGuid(Mat, Id);
    if (!Expr) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("expression not found"));
    FScopedTransaction Tx(LOCTEXT("RenameMaterialExpression", "Sage: Rename Material Expression"));
    Expr->Modify();
    FString RenamedField = TEXT("desc");
    if (UMaterialExpressionParameter* Param = Cast<UMaterialExpressionParameter>(Expr))
    {
        Param->ParameterName = FName(*NewName);
        RenamedField = TEXT("parameter");
    }
    else if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr))
    {
        Custom->Description = NewName;
        Custom->Desc = NewName;
        RenamedField = TEXT("description");
    }
    else
    {
        Expr->Desc = NewName;
    }
    Expr->PostEditChange();
    Mat->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("expression_id"), Id);
    R->SetStringField(TEXT("renamed_field"), RenamedField);
    R->SetStringField(TEXT("name"), NewName);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome DuplicateExpressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("material")});
    const FString Id = FirstMaterialStringArg(Args, {TEXT("expression_id"), TEXT("node_id")});
    UMaterial* Mat = ResolveMaterial(Path);
    UMaterialExpression* Expr = FindExpressionByGuid(Mat, Id);
    if (!Mat || !Expr) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material/expression not found"));
    FScopedTransaction Tx(LOCTEXT("DuplicateMaterialExpression", "Sage: Duplicate Material Expression"));
    Mat->Modify();
    UMaterialExpression* Copy = UMaterialEditingLibrary::DuplicateMaterialExpression(Mat, nullptr, Expr);
    if (!Copy) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("DuplicateMaterialExpression failed"));
    double OffsetX = 160.0, OffsetY = 0.0;
    Args->TryGetNumberField(TEXT("offset_x"), OffsetX);
    Args->TryGetNumberField(TEXT("offset_y"), OffsetY);
    Copy->MaterialExpressionEditorX = Expr->MaterialExpressionEditorX + static_cast<int32>(OffsetX);
    Copy->MaterialExpressionEditorY = Expr->MaterialExpressionEditorY + static_cast<int32>(OffsetY);
    Mat->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("source_expression_id"), Id);
    R->SetStringField(TEXT("expression_id"), Copy->MaterialExpressionGuid.ToString());
    R->SetStringField(TEXT("class"), Copy->GetClass()->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

int32 ReplaceMaterialInputReferences(UMaterial* Mat, UMaterialExpression* Old, UMaterialExpression* NewExpr)
{
    int32 Replaced = 0;
    if (!Mat || !Old || !NewExpr || !Mat->GetEditorOnlyData()) return Replaced;
    for (UMaterialExpression* Expr : Mat->GetEditorOnlyData()->ExpressionCollection.Expressions)
    {
        if (!Expr || Expr == Old) continue;
        Expr->Modify();
        for (FExpressionInputIterator It{Expr}; It; ++It)
        {
            if (It.Input && It.Input->Expression == Old)
            {
                It.Input->Expression = NewExpr;
                ++Replaced;
            }
        }
    }
    UMaterialEditorOnlyData* EOD = Mat->GetEditorOnlyData();
    auto ReplaceTopLevel = [&](FExpressionInput& Input)
    {
        if (Input.Expression == Old)
        {
            Input.Expression = NewExpr;
            ++Replaced;
        }
    };
    ReplaceTopLevel(EOD->BaseColor);
    ReplaceTopLevel(EOD->Metallic);
    ReplaceTopLevel(EOD->Specular);
    ReplaceTopLevel(EOD->Roughness);
    ReplaceTopLevel(EOD->EmissiveColor);
    ReplaceTopLevel(EOD->Opacity);
    ReplaceTopLevel(EOD->OpacityMask);
    ReplaceTopLevel(EOD->Normal);
    ReplaceTopLevel(EOD->WorldPositionOffset);
    ReplaceTopLevel(EOD->AmbientOcclusion);
    ReplaceTopLevel(EOD->Refraction);
    return Replaced;
}

FSageToolDispatch::FOutcome ReplaceExpressionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("material")});
    const FString Id = FirstMaterialStringArg(Args, {TEXT("expression_id"), TEXT("node_id"), TEXT("old_expression_id")});
    const FString ClassPath = FirstMaterialStringArg(Args, {TEXT("new_class"), TEXT("expression_class"), TEXT("class")});
    if (Path.IsEmpty() || Id.IsEmpty() || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing path/expression_id/new_class"));
    }
    UMaterial* Mat = ResolveMaterial(Path);
    UMaterialExpression* Old = FindExpressionByGuid(Mat, Id);
    UClass* NewClass = FindObject<UClass>(nullptr, *ClassPath);
    if (!NewClass) NewClass = LoadObject<UClass>(nullptr, *ClassPath);
    if (!Mat || !Old || !NewClass || !NewClass->IsChildOf(UMaterialExpression::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("invalid material/expression/new_class"));
    }
    FScopedTransaction Tx(LOCTEXT("ReplaceMaterialExpression", "Sage: Replace Material Expression"));
    Mat->Modify();
    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(
        Mat, NewClass, Old->MaterialExpressionEditorX, Old->MaterialExpressionEditorY);
    if (!NewExpr) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("CreateMaterialExpression replacement failed"));
    NewExpr->Desc = Old->Desc;
    const int32 Rewired = ReplaceMaterialInputReferences(Mat, Old, NewExpr);
    UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Old);
    UMaterialEditingLibrary::RecompileMaterial(Mat);
    Mat->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("old_expression_id"), Id);
    R->SetStringField(TEXT("expression_id"), NewExpr->MaterialExpressionGuid.ToString());
    R->SetStringField(TEXT("class"), NewExpr->GetClass()->GetName());
    R->SetNumberField(TEXT("rewired_inputs"), Rewired);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CreateMaterialFunctionImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("destination")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    FString PackagePath, AssetName;
    if (!Path.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("path must be /Folder/Name form"));
    }
    IAssetTools& AT = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
    UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
    UObject* NewObj = AT.CreateAsset(AssetName, PackagePath, UMaterialFunction::StaticClass(), Factory);
    UMaterialFunction* Function = Cast<UMaterialFunction>(NewObj);
    if (!Function) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("CreateAsset(MaterialFunction) failed"));
    FString Description, Caption;
    if (Args->TryGetStringField(TEXT("description"), Description)) Function->Description = Description;
    if (Args->TryGetStringField(TEXT("caption"), Caption)) Function->UserExposedCaption = Caption;
    Function->bExposeToLibrary = FirstMaterialBoolArg(Args, {TEXT("expose_to_library")}, Function->bExposeToLibrary != 0);
    Function->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Function->GetPathName());
    R->SetStringField(TEXT("name"), Function->GetName());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BuildFunctionGraphImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("function")});
    UMaterialFunction* Function = ResolveMaterialFunction(Path);
    if (!Function) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material function not found"));
    const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
    if (!Args.IsValid() || !Args->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'nodes' array"));
    }
    FScopedTransaction Tx(LOCTEXT("BuildMaterialFunctionGraph", "Sage: Build Material Function Graph"));
    Function->Modify();
    if (FirstMaterialBoolArg(Args, {TEXT("clear_existing")}, false))
    {
        UMaterialEditingLibrary::DeleteAllMaterialExpressionsInFunction(Function);
    }
    TArray<TSharedPtr<FJsonValue>> Added;
    for (const TSharedPtr<FJsonValue>& V : *Nodes)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!V.IsValid() || !V->TryGetObject(Obj) || !Obj || !Obj->IsValid()) continue;
        FString ClassPath;
        if (!(*Obj)->TryGetStringField(TEXT("class"), ClassPath)) continue;
        UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
        if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassPath);
        if (!Cls || !Cls->IsChildOf(UMaterialExpression::StaticClass())) continue;
        int32 X = 0, Y = 0;
        (*Obj)->TryGetNumberField(TEXT("x"), X);
        (*Obj)->TryGetNumberField(TEXT("y"), Y);
        UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, Cls, X, Y);
        if (!Expr) continue;
        if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr))
        {
            ApplyCustomExpressionFields(Custom, *Obj, false);
        }
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("expression_id"), Expr->MaterialExpressionGuid.ToString());
        Row->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
        Added.Add(MakeShared<FJsonValueObject>(Row));
    }
    UMaterialEditingLibrary::UpdateMaterialFunction(Function);
    Function->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Function->GetPathName());
    R->SetArrayField(TEXT("added"), Added);
    R->SetNumberField(TEXT("added_count"), Added.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GetFunctionInfoImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("function")});
    UMaterialFunction* Function = ResolveMaterialFunction(Path);
    if (!Function) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material function not found"));
    TArray<TSharedPtr<FJsonValue>> Expressions;
    for (TObjectPtr<UMaterialExpression> ExprPtr : Function->GetExpressions())
    {
        UMaterialExpression* Expr = ExprPtr.Get();
        if (Expr) Expressions.Add(MakeShared<FJsonValueObject>(ExpressionToJson(Expr)));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Function->GetPathName());
    R->SetStringField(TEXT("description"), Function->Description);
    R->SetStringField(TEXT("caption"), Function->UserExposedCaption);
    R->SetBoolField(TEXT("expose_to_library"), Function->bExposeToLibrary != 0);
    R->SetArrayField(TEXT("expressions"), Expressions);
    R->SetNumberField(TEXT("expression_count"), Expressions.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool HasScalarOverride(UMaterialInstanceConstant* MIC, FName Name)
{
    return MIC && MIC->ScalarParameterValues.ContainsByPredicate([&](const FScalarParameterValue& P) { return P.ParameterInfo.Name == Name; });
}

bool HasVectorOverride(UMaterialInstanceConstant* MIC, FName Name)
{
    return MIC && MIC->VectorParameterValues.ContainsByPredicate([&](const FVectorParameterValue& P) { return P.ParameterInfo.Name == Name; });
}

bool HasTextureOverride(UMaterialInstanceConstant* MIC, FName Name)
{
    return MIC && MIC->TextureParameterValues.ContainsByPredicate([&](const FTextureParameterValue& P) { return P.ParameterInfo.Name == Name; });
}

FSageToolDispatch::FOutcome GetInstanceParametersImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("instance"), TEXT("asset")});
    UMaterialInstanceConstant* MIC = ResolveMaterialInstance(Path);
    if (!MIC) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material instance not found"));
    TArray<TSharedPtr<FJsonValue>> Rows;
    auto Add = [&](const FMaterialParameterInfo& Info, const FString& Kind, TSharedPtr<FJsonValue> Value, bool bOverridden)
    {
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Info.Name.ToString());
        Row->SetStringField(TEXT("kind"), Kind);
        Row->SetBoolField(TEXT("overridden"), bOverridden);
        Row->SetField(TEXT("value"), Value);
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    };
    TArray<FMaterialParameterInfo> Infos;
    TArray<FGuid> Guids;
    MIC->GetAllScalarParameterInfo(Infos, Guids);
    for (const FMaterialParameterInfo& Info : Infos)
    {
        Add(Info, TEXT("scalar"),
            MakeShared<FJsonValueNumber>(UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(MIC, Info.Name)),
            HasScalarOverride(MIC, Info.Name));
    }
    Infos.Reset(); Guids.Reset();
    MIC->GetAllVectorParameterInfo(Infos, Guids);
    for (const FMaterialParameterInfo& Info : Infos)
    {
        Add(Info, TEXT("vector"),
            ColorToJson(UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(MIC, Info.Name)),
            HasVectorOverride(MIC, Info.Name));
    }
    Infos.Reset(); Guids.Reset();
    MIC->GetAllTextureParameterInfo(Infos, Guids);
    for (const FMaterialParameterInfo& Info : Infos)
    {
        UTexture* Tex = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(MIC, Info.Name);
        Add(Info, TEXT("texture"),
            MakeShared<FJsonValueString>(Tex ? FSoftObjectPath(Tex).ToString() : FString()),
            HasTextureOverride(MIC, Info.Name));
    }
    Infos.Reset(); Guids.Reset();
    MIC->GetAllStaticSwitchParameterInfo(Infos, Guids);
    for (const FMaterialParameterInfo& Info : Infos)
    {
        Add(Info, TEXT("static_switch"),
            MakeShared<FJsonValueBoolean>(UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(MIC, Info.Name)),
            false);
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), MIC->GetPathName());
    R->SetStringField(TEXT("parent"), MIC->Parent ? FSoftObjectPath(MIC->Parent).ToString() : FString());
    R->SetArrayField(TEXT("parameters"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

bool SetMICParameterFromJson(UMaterialInstanceConstant* MIC,
                             const FString& Name,
                             const TSharedPtr<FJsonValue>& Value,
                             const FString& KindHint,
                             FString& OutKind,
                             FString& OutError)
{
    if (!MIC || Name.IsEmpty() || !Value.IsValid())
    {
        OutError = TEXT("missing instance/name/value");
        return false;
    }
    const FName ParamName(*Name);
    if (KindHint.Equals(TEXT("scalar"), ESearchCase::IgnoreCase) ||
        (KindHint.IsEmpty() && (Value->Type == EJson::Number || Value->Type == EJson::Boolean)))
    {
        UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, ParamName, static_cast<float>(Value->AsNumber()));
        OutKind = TEXT("scalar");
        return true;
    }
    if (KindHint.Equals(TEXT("vector"), ESearchCase::IgnoreCase) ||
        (KindHint.IsEmpty() && Value->Type == EJson::Array))
    {
        FLinearColor Color;
        if (!JsonToLinearColor(Value, Color))
        {
            OutError = TEXT("vector value must be [r,g,b,a?]");
            return false;
        }
        UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, ParamName, Color);
        OutKind = TEXT("vector");
        return true;
    }
    if (KindHint.Equals(TEXT("texture"), ESearchCase::IgnoreCase) ||
        (KindHint.IsEmpty() && Value->Type == EJson::String))
    {
        UTexture* Tex = Cast<UTexture>(FSoftObjectPath(Value->AsString()).TryLoad());
        if (!Tex)
        {
            OutError = TEXT("texture value must be a valid UTexture asset path");
            return false;
        }
        UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, ParamName, Tex);
        OutKind = TEXT("texture");
        return true;
    }
    if (KindHint.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
    {
        if (Value->Type != EJson::Boolean)
        {
            OutError = TEXT("static_switch value must be boolean");
            return false;
        }
        UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MIC, ParamName, Value->AsBool());
        OutKind = TEXT("static_switch");
        return true;
    }
    OutError = TEXT("unsupported parameter kind; use scalar/vector/texture/static_switch");
    return false;
}

FSageToolDispatch::FOutcome SetInstanceParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("instance"), TEXT("asset")});
    const FString Name = FirstMaterialStringArg(Args, {TEXT("parameter"), TEXT("name")});
    FString Kind;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("kind"), Kind);
    TSharedPtr<FJsonValue> Value = Args.IsValid() ? Args->Values.FindRef(TEXT("value")) : nullptr;
    UMaterialInstanceConstant* MIC = ResolveMaterialInstance(Path);
    if (!MIC) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material instance not found"));
    FScopedTransaction Tx(LOCTEXT("SetInstanceParameter", "Sage: Set Material Instance Parameter"));
    MIC->Modify();
    FString OutKind, Error;
    if (!SetMICParameterFromJson(MIC, Name, Value, Kind, OutKind, Error))
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602, Error);
    }
    MIC->PostEditChange();
    MIC->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("instance"), MIC->GetPathName());
    R->SetStringField(TEXT("parameter"), Name);
    R->SetStringField(TEXT("kind"), OutKind);
    R->SetField(TEXT("value"), Value);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetInstanceParametersImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("instance"), TEXT("asset")});
    UMaterialInstanceConstant* MIC = ResolveMaterialInstance(Path);
    if (!MIC) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material instance not found"));
    FScopedTransaction Tx(LOCTEXT("SetInstanceParameters", "Sage: Set Material Instance Parameters"));
    MIC->Modify();
    TArray<TSharedPtr<FJsonValue>> Applied;
    const TSharedPtr<FJsonObject>* Obj = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Args->TryGetObjectField(TEXT("parameters"), Obj) && Obj && Obj->IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Obj)->Values)
        {
            FString Kind, Error;
            if (!SetMICParameterFromJson(MIC, Pair.Key, Pair.Value, FString(), Kind, Error))
            {
                Tx.Cancel();
                return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("%s: %s"), *Pair.Key, *Error));
            }
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("parameter"), Pair.Key);
            Row->SetStringField(TEXT("kind"), Kind);
            Applied.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    else if (Args->TryGetArrayField(TEXT("parameters"), Arr) && Arr)
    {
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            const TSharedPtr<FJsonObject>* P = nullptr;
            if (!V.IsValid() || !V->TryGetObject(P) || !P || !P->IsValid()) continue;
            FString Name, Kind;
            (*P)->TryGetStringField(TEXT("name"), Name);
            (*P)->TryGetStringField(TEXT("parameter"), Name);
            (*P)->TryGetStringField(TEXT("kind"), Kind);
            TSharedPtr<FJsonValue> Value = (*P)->Values.FindRef(TEXT("value"));
            FString OutKind, Error;
            if (!SetMICParameterFromJson(MIC, Name, Value, Kind, OutKind, Error))
            {
                Tx.Cancel();
                return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("%s: %s"), *Name, *Error));
            }
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("parameter"), Name);
            Row->SetStringField(TEXT("kind"), OutKind);
            Applied.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'parameters' object or array"));
    }
    UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
    MIC->PostEditChange();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("instance"), MIC->GetPathName());
    R->SetArrayField(TEXT("applied"), Applied);
    R->SetNumberField(TEXT("count"), Applied.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetInstanceParentImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("instance"), TEXT("asset")});
    const FString ParentPath = FirstMaterialStringArg(Args, {TEXT("parent"), TEXT("parent_path")});
    UMaterialInstanceConstant* MIC = ResolveMaterialInstance(Path);
    UMaterialInterface* Parent = ResolveMaterialInterface(ParentPath);
    if (!MIC || !Parent) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("instance or parent not found"));
    FScopedTransaction Tx(LOCTEXT("SetInstanceParent", "Sage: Set Material Instance Parent"));
    MIC->Modify();
    UMaterialEditingLibrary::SetMaterialInstanceParent(MIC, Parent);
    MIC->PostEditChange();
    MIC->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("instance"), MIC->GetPathName());
    R->SetStringField(TEXT("parent"), FSoftObjectPath(Parent).ToString());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome ClearInstanceParameterImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("instance"), TEXT("asset")});
    const FString Name = FirstMaterialStringArg(Args, {TEXT("parameter"), TEXT("name")});
    FString Kind;
    if (Args.IsValid()) Args->TryGetStringField(TEXT("kind"), Kind);
    UMaterialInstanceConstant* MIC = ResolveMaterialInstance(Path);
    if (!MIC || Name.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing instance or parameter"));
    if (Kind.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
    {
        return MaterialUnsupported(TEXT("clear_instance_parameter"),
            TEXT("UE public API exposes static-switch set/update but no isolated clear for one static switch override"));
    }
    const FName ParamName(*Name);
    FScopedTransaction Tx(LOCTEXT("ClearInstanceParameter", "Sage: Clear Material Instance Parameter"));
    MIC->Modify();
    int32 Removed = 0;
    if (Kind.IsEmpty() || Kind.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
    {
        Removed += MIC->ScalarParameterValues.RemoveAll([&](const FScalarParameterValue& P) { return P.ParameterInfo.Name == ParamName; });
    }
    if (Kind.IsEmpty() || Kind.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
    {
        Removed += MIC->VectorParameterValues.RemoveAll([&](const FVectorParameterValue& P) { return P.ParameterInfo.Name == ParamName; });
    }
    if (Kind.IsEmpty() || Kind.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
    {
        Removed += MIC->TextureParameterValues.RemoveAll([&](const FTextureParameterValue& P) { return P.ParameterInfo.Name == ParamName; });
    }
    UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
    MIC->PostEditChange();
    MIC->MarkPackageDirty();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("instance"), MIC->GetPathName());
    R->SetStringField(TEXT("parameter"), Name);
    R->SetNumberField(TEXT("removed"), Removed);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

TArray<FString> MaterialPathArrayArg(const TSharedPtr<FJsonObject>& Args)
{
    TArray<FString> Paths;
    if (!Args.IsValid()) return Paths;
    FString Single = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("asset")});
    if (!Single.IsEmpty()) Paths.Add(Single);
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (Args->TryGetArrayField(TEXT("paths"), Arr) || Args->TryGetArrayField(TEXT("assets"), Arr))
    {
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            if (V.IsValid() && V->Type == EJson::String) Paths.Add(V->AsString());
        }
    }
    return Paths;
}

FSageToolDispatch::FOutcome ListMaterialInstancesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    FString Query, ParentPath;
    int32 MaxResults = 250;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), SearchPath);
        Args->TryGetStringField(TEXT("query"), Query);
        Args->TryGetStringField(TEXT("parent"), ParentPath);
        Args->TryGetNumberField(TEXT("max_results"), MaxResults);
    }
    UMaterialInterface* Parent = ParentPath.IsEmpty() ? nullptr : ResolveMaterialInterface(ParentPath);
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = true;
    TArray<FAssetData> Assets;
    ARM.Get().GetAssets(Filter, Assets);
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FAssetData& Data : Assets)
    {
        if (Rows.Num() >= MaxResults) break;
        if (!Query.IsEmpty() && !Data.AssetName.ToString().Contains(Query)) continue;
        UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Data.GetAsset());
        if (Parent && (!MIC || MIC->Parent != Parent)) continue;
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Data.AssetName.ToString());
        Row->SetStringField(TEXT("path"), Data.GetObjectPathString());
        if (MIC && MIC->Parent) Row->SetStringField(TEXT("parent"), FSoftObjectPath(MIC->Parent).ToString());
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("instances"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BatchSetMaterialPropertyImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    const FString Property = FirstMaterialStringArg(Args, {TEXT("property")});
    TSharedPtr<FJsonValue> Value = Args.IsValid() ? Args->Values.FindRef(TEXT("value")) : nullptr;
    if (Property.IsEmpty() || !Value.IsValid())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing property/value"));
    }
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const FString& Path : MaterialPathArrayArg(Args))
    {
        UMaterialInterface* MI = ResolveMaterialInterface(Path);
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), Path);
        if (!MI)
        {
            Row->SetBoolField(TEXT("ok"), false);
            Row->SetStringField(TEXT("error"), TEXT("material not found"));
        }
        else if (FProperty* Prop = MI->GetClass()->FindPropertyByName(*Property))
        {
            MI->Modify();
            const bool bOk = detail::SetUPropertyFromJson(MI, Prop, Value);
            MI->PostEditChange();
            MI->MarkPackageDirty();
            Row->SetBoolField(TEXT("ok"), bOk);
        }
        else
        {
            Row->SetBoolField(TEXT("ok"), false);
            Row->SetStringField(TEXT("error"), TEXT("property not found"));
        }
        Results.Add(MakeShared<FJsonValueObject>(Row));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("count"), Results.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome BatchRecompileImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;
    TArray<TSharedPtr<FJsonValue>> Results;
    for (const FString& Path : MaterialPathArrayArg(Args))
    {
        UMaterialInterface* MI = ResolveMaterialInterface(Path);
        TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("path"), Path);
        if (UMaterial* Mat = Cast<UMaterial>(MI))
        {
            UMaterialEditingLibrary::RecompileMaterial(Mat);
            Row->SetBoolField(TEXT("queued"), true);
            Row->SetStringField(TEXT("kind"), TEXT("material"));
        }
        else if (UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(MI))
        {
            UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
            Row->SetBoolField(TEXT("queued"), true);
            Row->SetStringField(TEXT("kind"), TEXT("instance"));
        }
        else
        {
            Row->SetBoolField(TEXT("queued"), false);
            Row->SetStringField(TEXT("error"), TEXT("material not found"));
        }
        Results.Add(MakeShared<FJsonValueObject>(Row));
    }
    auto R = MakeShared<FJsonObject>();
    R->SetArrayField(TEXT("results"), Results);
    R->SetNumberField(TEXT("count"), Results.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome GetThumbnailImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("material")});
    UObject* Obj = ResolveAnyAsset(Path);
    if (!Obj) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("asset not found"));
    FObjectThumbnail* Thumb = ThumbnailTools::GenerateThumbnailForObjectToSaveToDisk(Obj);
    if (!Thumb) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("thumbnail generation returned null"));
    const TArray<uint8>& Data = Thumb->GetUncompressedImageData();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Obj->GetPathName());
    R->SetNumberField(TEXT("width"), Thumb->GetImageWidth());
    R->SetNumberField(TEXT("height"), Thumb->GetImageHeight());
    R->SetStringField(TEXT("format"), TEXT("BGRA8_sRGB"));
    R->SetNumberField(TEXT("byte_count"), Data.Num());
    if (FirstMaterialBoolArg(Args, {TEXT("include_data"), TEXT("base64")}, false))
    {
        R->SetStringField(TEXT("data_base64"), FBase64::Encode(Data));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome CaptureMaterialGridImpl(const TSharedPtr<FJsonObject>& Args)
{
    return MaterialUnsupported(TEXT("capture_material_grid"),
        TEXT("side-by-side material grid capture needs a composed render target or viewport path; use get_thumbnail per asset until capture infrastructure lands"));
}

FSageToolDispatch::FOutcome CaptureWithOverlayImpl(const TSharedPtr<FJsonObject>& Args)
{
    return MaterialUnsupported(TEXT("capture_with_overlay"),
        TEXT("debug overlays such as wireframe, shader complexity, UV density, and normals require viewport show-flag capture state"));
}

FSageToolDispatch::FOutcome InspectMaterialPbrImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("material"), TEXT("asset")});
    UMaterialInterface* MI = ResolveMaterialInterface(Path);
    if (!MI) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("material not found"));
    TArray<TSharedPtr<FJsonValue>> Textures;
    UMaterial* Base = MI->GetMaterial();
    if (Base)
    {
        for (UTexture* Tex : UMaterialEditingLibrary::GetUsedTextures(Base))
        {
            if (!Tex) continue;
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            const FString Name = Tex->GetName();
            Row->SetStringField(TEXT("name"), Name);
            Row->SetStringField(TEXT("path"), FSoftObjectPath(Tex).ToString());
            FString Guess = TEXT("unknown");
            if (Name.Contains(TEXT("ORM")) || Name.Contains(TEXT("OcclusionRoughnessMetallic"))) Guess = TEXT("ORM");
            else if (Name.Contains(TEXT("ARM")) || Name.Contains(TEXT("AmbientRoughnessMetallic"))) Guess = TEXT("ARM");
            else if (Name.Contains(TEXT("MRA"))) Guess = TEXT("MRA");
            else if (Name.Contains(TEXT("_N")) || Name.Contains(TEXT("Normal"))) Guess = TEXT("normal");
            else if (Name.Contains(TEXT("BaseColor")) || Name.Contains(TEXT("Albedo"))) Guess = TEXT("base_color");
            Row->SetStringField(TEXT("packing_guess"), Guess);
            Textures.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), MI->GetPathName());
    R->SetStringField(TEXT("base_material"), Base ? Base->GetPathName() : FString());
    R->SetArrayField(TEXT("textures"), Textures);
    R->SetNumberField(TEXT("texture_count"), Textures.Num());
    R->SetBoolField(TEXT("has_texture_channel_packing_guess"), Textures.Num() > 0);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome InspectTextureChannelsImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Path = FirstMaterialStringArg(Args, {TEXT("path"), TEXT("texture"), TEXT("asset")});
    UTexture2D* Tex = Cast<UTexture2D>(ResolveAnyAsset(Path));
    if (!Tex) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("Texture2D not found"));
    const ETextureSourceFormat Format = Tex->Source.GetFormat();
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Tex->GetPathName());
    R->SetNumberField(TEXT("width"), Tex->Source.GetSizeX());
    R->SetNumberField(TEXT("height"), Tex->Source.GetSizeY());
    R->SetStringField(TEXT("source_format"), (Format >= 0 && Format < TSF_MAX) ? GTextureSourceFormats[Format].Name : TEXT("Unknown"));
    R->SetBoolField(TEXT("srgb"), Tex->SRGB);
    R->SetNumberField(TEXT("compression"), static_cast<int32>(Tex->CompressionSettings));
    TArray64<uint8> Mip;
    if (Format == TSF_BGRA8 && Tex->Source.GetMipData(Mip, 0) && Mip.Num() >= 4)
    {
        struct FChannelStats { uint8 Min = 255; uint8 Max = 0; uint64 Sum = 0; };
        FChannelStats Stats[4];
        const int64 PixelCount = Mip.Num() / 4;
        for (int64 I = 0; I + 3 < Mip.Num(); I += 4)
        {
            const uint8 Values[4] = {Mip[I + 2], Mip[I + 1], Mip[I + 0], Mip[I + 3]};
            for (int32 C = 0; C < 4; ++C)
            {
                Stats[C].Min = FMath::Min(Stats[C].Min, Values[C]);
                Stats[C].Max = FMath::Max(Stats[C].Max, Values[C]);
                Stats[C].Sum += Values[C];
            }
        }
        const TCHAR* Names[4] = {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")};
        TArray<TSharedPtr<FJsonValue>> Channels;
        for (int32 C = 0; C < 4; ++C)
        {
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("channel"), Names[C]);
            Row->SetNumberField(TEXT("min"), Stats[C].Min);
            Row->SetNumberField(TEXT("max"), Stats[C].Max);
            Row->SetNumberField(TEXT("avg"), PixelCount > 0 ? static_cast<double>(Stats[C].Sum) / static_cast<double>(PixelCount) : 0.0);
            Channels.Add(MakeShared<FJsonValueObject>(Row));
        }
        R->SetBoolField(TEXT("stats_available"), true);
        R->SetArrayField(TEXT("channels"), Channels);
    }
    else
    {
        R->SetBoolField(TEXT("stats_available"), false);
        R->SetStringField(TEXT("stats_reason"), TEXT("only TSF_BGRA8 source data is summarized without format conversion"));
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome AuditOrphanMaterialsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString SearchPath = TEXT("/Game");
    int32 MaxResults = 500;
    if (Args.IsValid())
    {
        Args->TryGetStringField(TEXT("path"), SearchPath);
        Args->TryGetNumberField(TEXT("max_results"), MaxResults);
    }
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = ARM.Get();
    FARFilter Filter;
    Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = true;
    TArray<FAssetData> Assets;
    Registry.GetAssets(Filter, Assets);
    TArray<TSharedPtr<FJsonValue>> Orphans;
    for (const FAssetData& Data : Assets)
    {
        if (Orphans.Num() >= MaxResults) break;
        TArray<FName> Referencers;
        Registry.GetReferencers(Data.PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
        if (Referencers.Num() == 0)
        {
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), Data.AssetName.ToString());
            Row->SetStringField(TEXT("path"), Data.GetObjectPathString());
            Orphans.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), SearchPath);
    R->SetNumberField(TEXT("checked"), Assets.Num());
    R->SetArrayField(TEXT("orphans"), Orphans);
    R->SetNumberField(TEXT("orphan_count"), Orphans.Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome DispatchMaterialParityTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("create_custom_hlsl_node")) return CreateCustomHlslNodeImpl(Args);
    if (Tool == TEXT("update_custom_hlsl_node")) return UpdateCustomHlslNodeImpl(Args);
    if (Tool == TEXT("move_expression")) return MoveExpressionImpl(Args);
    if (Tool == TEXT("rename_expression")) return RenameExpressionImpl(Args);
    if (Tool == TEXT("duplicate_expression")) return DuplicateExpressionImpl(Args);
    if (Tool == TEXT("replace_expression")) return ReplaceExpressionImpl(Args);
    if (Tool == TEXT("create_material_function")) return CreateMaterialFunctionImpl(Args);
    if (Tool == TEXT("build_function_graph")) return BuildFunctionGraphImpl(Args);
    if (Tool == TEXT("get_function_info")) return GetFunctionInfoImpl(Args);
    if (Tool == TEXT("get_instance_parameters")) return GetInstanceParametersImpl(Args);
    if (Tool == TEXT("set_instance_parameter")) return SetInstanceParameterImpl(Args);
    if (Tool == TEXT("set_instance_parameters")) return SetInstanceParametersImpl(Args);
    if (Tool == TEXT("set_instance_parent")) return SetInstanceParentImpl(Args);
    if (Tool == TEXT("clear_instance_parameter")) return ClearInstanceParameterImpl(Args);
    if (Tool == TEXT("list_material_instances")) return ListMaterialInstancesImpl(Args);
    if (Tool == TEXT("batch_set_material_property")) return BatchSetMaterialPropertyImpl(Args);
    if (Tool == TEXT("batch_recompile")) return BatchRecompileImpl(Args);
    if (Tool == TEXT("get_thumbnail")) return GetThumbnailImpl(Args);
    if (Tool == TEXT("capture_material_grid")) return CaptureMaterialGridImpl(Args);
    if (Tool == TEXT("capture_with_overlay")) return CaptureWithOverlayImpl(Args);
    if (Tool == TEXT("inspect_material_pbr")) return InspectMaterialPbrImpl(Args);
    if (Tool == TEXT("inspect_texture_channels")) return InspectTextureChannelsImpl(Args);
    if (Tool == TEXT("audit_orphan_materials")) return AuditOrphanMaterialsImpl(Args);
    return MaterialUnsupported(Tool, TEXT("no dispatch mapping"));
}

}  // namespace (anonymous)

void RegisterMaterialGraphTools(FSageToolDispatch& Dispatch)
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

    // Read
    Dispatch.RegisterHandler(TEXT("mat.read"),               GT(&MatReadImpl));
    Dispatch.RegisterHandler(TEXT("mat.list_parameters"),    GT(&MatListParamsImpl));
    Dispatch.RegisterHandler(TEXT("mat.list_expressions"),   GT(&MatListExpressionsImpl));

    // Write — instances
    Dispatch.RegisterHandler(TEXT("mat.create_instance"),    GT(&MatCreateInstanceImpl));

    // Write — graph
    Dispatch.RegisterHandler(TEXT("mat.add_expression"),     GT(&MatAddExpressionImpl));
    Dispatch.RegisterHandler(TEXT("mat.delete_expression"),  GT(&MatDeleteExpressionImpl));
    Dispatch.RegisterHandler(TEXT("mat.connect_expressions"),GT(&MatConnectExprsImpl));
    Dispatch.RegisterHandler(TEXT("mat.connect_to_property"),GT(&MatConnectToPropertyImpl));
    Dispatch.RegisterHandler(TEXT("mat.set_expression_value"),GT(&MatSetExpressionValueImpl));
    Dispatch.RegisterHandler(TEXT("mat.connect_texture"),    GT(&MatConnectTextureImpl));
    Dispatch.RegisterHandler(TEXT("mat.set_shading_model"),  GT(&MatSetShadingModelImpl));
    Dispatch.RegisterHandler(TEXT("mat.set_base_color"),     GT(&MatSetBaseColorImpl));

    // Validate
    Dispatch.RegisterHandler(TEXT("mat.validate"),           GT(&MatValidateImpl));

    // Phase 4.3 round 2
    Dispatch.RegisterHandler(TEXT("mat.create"),                GT(&MatCreateImpl));
    Dispatch.RegisterHandler(TEXT("mat.set_blend_mode"),        GT(&MatSetBlendModeImpl));
    Dispatch.RegisterHandler(TEXT("mat.disconnect"),            GT(&MatDisconnectImpl));
    Dispatch.RegisterHandler(TEXT("mat.list_expression_types"), GT(&MatListExpressionTypesImpl));
    Dispatch.RegisterHandler(TEXT("mat.recompile"),             GT(&MatRecompileImpl));
    Dispatch.RegisterHandler(TEXT("mat.duplicate"),             GT(&MatDuplicateImpl));
    Dispatch.RegisterHandler(TEXT("mat.get_shader_stats"),      GT(&MatGetShaderStatsImpl));
    Dispatch.RegisterHandler(TEXT("mat.export_graph"),          GT(&MatExportGraphImpl));
    Dispatch.RegisterHandler(TEXT("mat.import_graph"),          GT(&MatImportGraphImpl));
    Dispatch.RegisterHandler(TEXT("mat.build_graph"),           GT(&MatBuildGraphImpl));
    Dispatch.RegisterHandler(TEXT("mat.render_preview"),        GT(&MatRenderPreviewImpl));
    Dispatch.RegisterHandler(TEXT("mat.begin_transaction"),     GT(&MatBeginTransactionImpl));
    Dispatch.RegisterHandler(TEXT("mat.end_transaction"),       GT(&MatEndTransactionImpl));

    static const TCHAR* MaterialParityTools[] = {
        TEXT("create_custom_hlsl_node"),
        TEXT("update_custom_hlsl_node"),
        TEXT("move_expression"),
        TEXT("rename_expression"),
        TEXT("duplicate_expression"),
        TEXT("replace_expression"),
        TEXT("create_material_function"),
        TEXT("build_function_graph"),
        TEXT("get_function_info"),
        TEXT("get_instance_parameters"),
        TEXT("set_instance_parameter"),
        TEXT("set_instance_parameters"),
        TEXT("set_instance_parent"),
        TEXT("clear_instance_parameter"),
        TEXT("list_material_instances"),
        TEXT("batch_set_material_property"),
        TEXT("batch_recompile"),
        TEXT("get_thumbnail"),
        TEXT("capture_material_grid"),
        TEXT("capture_with_overlay"),
        TEXT("inspect_material_pbr"),
        TEXT("inspect_texture_channels"),
        TEXT("audit_orphan_materials"),
    };
    for (const TCHAR* ToolName : MaterialParityTools)
    {
        Dispatch.RegisterHandler(ToolName,
            [Tool = FString(ToolName)](const TSharedPtr<FJsonObject>& Args) -> FSageToolDispatch::FOutcome
            {
                return detail::RunOnGameThread([&]() -> FSageToolDispatch::FOutcome
                {
                    return DispatchMaterialParityTool(Tool, Args);
                });
            });
    }
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
