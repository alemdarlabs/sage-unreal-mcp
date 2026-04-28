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
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstance.h"
#include "MaterialEditingLibrary.h"
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
    case MSM_Strata:               return TEXT("Strata");
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
    M->Modify();
    M->SetShadingModel(ParseShadingModel(ModelName));
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("material"), M->GetName());
    R->SetStringField(TEXT("shading_model"), ModelName);
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
    M->Modify();

    UMaterialExpressionConstant3Vector* C3 = NewObject<UMaterialExpressionConstant3Vector>(M);
    C3->Constant = FLinearColor(R, G, B, A);
    C3->MaterialExpressionEditorX = -300;
    C3->MaterialExpressionEditorY = 0;
    C3->MaterialExpressionGuid = FGuid::NewGuid();
    M->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(C3);

    M->GetEditorOnlyData()->BaseColor.Expression = C3;
    M->GetEditorOnlyData()->BaseColor.OutputIndex = 0;
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("expression_id"), C3->MaterialExpressionGuid.ToString());
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
        return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("connect failed"));
    }
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
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
    M->Modify();

    if (auto* C = Cast<UMaterialExpressionConstant>(E))
    {
        C->R = (*It)->AsNumber();
    }
    else if (auto* C3 = Cast<UMaterialExpressionConstant3Vector>(E))
    {
        if ((*It)->Type != EJson::Array) return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("Constant3Vector requires array [r,g,b]"));
        const auto& A = (*It)->AsArray();
        if (A.Num() < 3) return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("array too short"));
        C3->Constant = FLinearColor(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber(),
                                     A.Num() > 3 ? A[3]->AsNumber() : 1.0f);
    }
    else if (auto* SP = Cast<UMaterialExpressionScalarParameter>(E))
    {
        SP->DefaultValue = (*It)->AsNumber();
    }
    else
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unsupported expression class for value set: %s"),
                            *E->GetClass()->GetName()));
    }
    UMaterialEditingLibrary::RecompileMaterial(M);

    auto Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("material"), M->GetName());
    Out->SetStringField(TEXT("expression_id"), NodeId);
    Out->SetField(TEXT("value"), *It);
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
    return FSageToolDispatch::FOutcome::MakeSuccess(Out);
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
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
