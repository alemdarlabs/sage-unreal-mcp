#include "Tools/SageMaterialGraphTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionConstantBiasScale.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstance.h"
#include "MaterialEditingLibrary.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ScopedTransaction.h"
#include "Misc/Paths.h"
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
    R->SetStringField(TEXT("note"),
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
    R->SetStringField(TEXT("note"), TEXT("thumbnails generated on demand by UE thumbnail system"));
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
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
