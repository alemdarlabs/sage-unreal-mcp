#include "Tools/SageMeshTools.h"

#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSourceData.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "ScopedTransaction.h"
#include "StaticMeshResources.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "SageMesh"

namespace sage::tools
{
namespace
{

using FOutcome = FSageToolDispatch::FOutcome;

static constexpr int32 kUnsupportedCode = -32005;

static FOutcome MissingArg(const TCHAR* Name)
{
    return FOutcome::MakeError(-32602, FString::Printf(TEXT("missing '%s'"), Name));
}

static FOutcome Unsupported(const FString& Tool, const FString& Reason)
{
    return FOutcome::MakeError(kUnsupportedCode,
        FString::Printf(TEXT("%s is not exposed as a safe public-editor mutation yet: %s"),
                        *Tool, *Reason));
}

static FString FirstStringArg(const TSharedPtr<FJsonObject>& Args,
                              std::initializer_list<const TCHAR*> Fields)
{
    if (!Args.IsValid()) return FString();
    FString Value;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetStringField(Field, Value) && !Value.IsEmpty())
        {
            return Value;
        }
    }
    return FString();
}

static int32 FirstIntArg(const TSharedPtr<FJsonObject>& Args,
                         std::initializer_list<const TCHAR*> Fields,
                         int32 DefaultValue = INDEX_NONE)
{
    if (!Args.IsValid()) return DefaultValue;
    int32 Value = DefaultValue;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetNumberField(Field, Value))
        {
            return Value;
        }
    }
    return DefaultValue;
}

static double FirstNumberArg(const TSharedPtr<FJsonObject>& Args,
                             std::initializer_list<const TCHAR*> Fields,
                             double DefaultValue = 0.0)
{
    if (!Args.IsValid()) return DefaultValue;
    double Value = DefaultValue;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetNumberField(Field, Value))
        {
            return Value;
        }
    }
    return DefaultValue;
}

static bool FirstBoolArg(const TSharedPtr<FJsonObject>& Args,
                         std::initializer_list<const TCHAR*> Fields,
                         bool DefaultValue = false)
{
    if (!Args.IsValid()) return DefaultValue;
    bool Value = DefaultValue;
    for (const TCHAR* Field : Fields)
    {
        if (Args->TryGetBoolField(Field, Value))
        {
            return Value;
        }
    }
    return DefaultValue;
}

static TArray<double> NumberArrayArg(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field)
{
    TArray<double> Out;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Args.IsValid() || !Args->TryGetArrayField(Field, Values) || !Values)
    {
        return Out;
    }
    for (const TSharedPtr<FJsonValue>& V : *Values)
    {
        double Number = 0.0;
        if (V.IsValid() && V->TryGetNumber(Number))
        {
            Out.Add(Number);
        }
    }
    return Out;
}

static UObject* LoadAsset(const FString& Path)
{
    if (Path.IsEmpty()) return nullptr;
    if (UObject* Existing = FindObject<UObject>(nullptr, *Path))
    {
        return Existing;
    }
    FSoftObjectPath Soft(Path);
    UObject* Obj = Soft.ResolveObject();
    if (!Obj)
    {
        Obj = Soft.TryLoad();
    }
    return Obj;
}

template <typename T>
static T* LoadAssetAs(const FString& Path)
{
    return Cast<T>(LoadAsset(Path));
}

static FString ObjectPath(const UObject* Obj)
{
    return Obj ? FSoftObjectPath(Obj).ToString() : FString();
}

static TSharedRef<FJsonObject> Vec3ObjectToJson(const FVector& V)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("x"), V.X);
    J->SetNumberField(TEXT("y"), V.Y);
    J->SetNumberField(TEXT("z"), V.Z);
    return J;
}

static TSharedRef<FJsonObject> BoundsToJson(const FBoxSphereBounds& Bounds)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetObjectField(TEXT("origin"), Vec3ObjectToJson(FVector(Bounds.Origin)));
    J->SetObjectField(TEXT("box_extent"), Vec3ObjectToJson(FVector(Bounds.BoxExtent)));
    J->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
    return J;
}

static TSharedRef<FJsonObject> ColorToJson(const FColor& Color)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("r"), Color.R);
    J->SetNumberField(TEXT("g"), Color.G);
    J->SetNumberField(TEXT("b"), Color.B);
    J->SetNumberField(TEXT("a"), Color.A);
    return J;
}

static TSharedRef<FJsonObject> Vec2ToJson(const FVector2f& V)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("x"), V.X);
    J->SetNumberField(TEXT("y"), V.Y);
    return J;
}

struct FResolvedMesh
{
    UObject* Asset = nullptr;
    UStaticMesh* StaticMesh = nullptr;
    USkeletalMesh* SkeletalMesh = nullptr;
};

static FOutcome ResolveMeshAsset(const TSharedPtr<FJsonObject>& Args,
                                 FResolvedMesh& Out,
                                 std::initializer_list<const TCHAR*> Fields = {TEXT("path"), TEXT("asset"), TEXT("mesh")})
{
    const FString Path = FirstStringArg(Args, Fields);
    if (Path.IsEmpty())
    {
        return MissingArg(TEXT("path"));
    }
    UObject* Asset = LoadAsset(Path);
    if (!Asset)
    {
        return FOutcome::MakeError(-32602, FString::Printf(TEXT("asset not found: %s"), *Path));
    }
    Out.Asset = Asset;
    Out.StaticMesh = Cast<UStaticMesh>(Asset);
    Out.SkeletalMesh = Cast<USkeletalMesh>(Asset);
    if (!Out.StaticMesh && !Out.SkeletalMesh)
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("asset is %s; expected UStaticMesh or USkeletalMesh"),
                            *Asset->GetClass()->GetName()));
    }
    return FOutcome::MakeSuccess(MakeShared<FJsonObject>());
}

static UBodySetup* BodySetupFor(const FResolvedMesh& Mesh)
{
    if (Mesh.StaticMesh) return Mesh.StaticMesh->GetBodySetup();
    if (Mesh.SkeletalMesh) return Mesh.SkeletalMesh->GetBodySetup();
    return nullptr;
}

static FString CollisionTraceFlagToString(ECollisionTraceFlag Flag)
{
    switch (Flag)
    {
    case CTF_UseDefault:          return TEXT("UseDefault");
    case CTF_UseSimpleAndComplex: return TEXT("UseSimpleAndComplex");
    case CTF_UseSimpleAsComplex:  return TEXT("UseSimpleAsComplex");
    case CTF_UseComplexAsSimple:  return TEXT("UseComplexAsSimple");
    default:                      return TEXT("Unknown");
    }
}

static bool ParseCollisionTraceFlag(const FString& Value, ECollisionTraceFlag& Out)
{
    if (Value.Equals(TEXT("default"), ESearchCase::IgnoreCase) ||
        Value.Equals(TEXT("UseDefault"), ESearchCase::IgnoreCase))
    {
        Out = CTF_UseDefault;
        return true;
    }
    if (Value.Equals(TEXT("simple_and_complex"), ESearchCase::IgnoreCase) ||
        Value.Equals(TEXT("UseSimpleAndComplex"), ESearchCase::IgnoreCase))
    {
        Out = CTF_UseSimpleAndComplex;
        return true;
    }
    if (Value.Equals(TEXT("simple_as_complex"), ESearchCase::IgnoreCase) ||
        Value.Equals(TEXT("UseSimpleAsComplex"), ESearchCase::IgnoreCase))
    {
        Out = CTF_UseSimpleAsComplex;
        return true;
    }
    if (Value.Equals(TEXT("complex_as_simple"), ESearchCase::IgnoreCase) ||
        Value.Equals(TEXT("UseComplexAsSimple"), ESearchCase::IgnoreCase))
    {
        Out = CTF_UseComplexAsSimple;
        return true;
    }
    return false;
}

static void SaveIfRequested(UObject* Asset, const TSharedPtr<FJsonObject>& Args, const TSharedRef<FJsonObject>& R)
{
    if (!Asset || !FirstBoolArg(Args, {TEXT("save")}, false) || !GEditor)
    {
        return;
    }
    if (UEditorAssetSubsystem* Sub = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
    {
        const bool bSaved = Sub->SaveLoadedAsset(Asset);
        R->SetBoolField(TEXT("saved"), bSaved);
    }
}

static TSharedRef<FJsonObject> StaticLodToJson(UStaticMesh* Mesh, int32 LODIndex)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("lod_index"), LODIndex);
    J->SetNumberField(TEXT("vertices"), Mesh ? Mesh->GetNumVertices(LODIndex) : 0);
    J->SetNumberField(TEXT("uv_channels"), Mesh ? Mesh->GetNumUVChannels(LODIndex) : 0);

    const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
    if (RenderData && RenderData->LODResources.IsValidIndex(LODIndex))
    {
        const FStaticMeshLODResources& Lod = RenderData->LODResources[LODIndex];
        J->SetNumberField(TEXT("render_vertices"), Lod.GetNumVertices());
        J->SetNumberField(TEXT("triangles"), Lod.GetNumTriangles());
        J->SetNumberField(TEXT("sections"), Lod.Sections.Num());
        J->SetNumberField(TEXT("render_uv_channels"), Lod.GetNumTexCoords());
        J->SetNumberField(TEXT("color_vertices"), Lod.VertexBuffers.ColorVertexBuffer.GetNumVertices());
    }

#if WITH_EDITOR
    if (Mesh && LODIndex < Mesh->GetNumSourceModels())
    {
        const FStaticMeshSourceModel& Source = Mesh->GetSourceModel(LODIndex);
        J->SetNumberField(TEXT("screen_size"), Source.ScreenSize.Default);
        J->SetNumberField(TEXT("reduction_percent_triangles"), Source.ReductionSettings.PercentTriangles);
        J->SetNumberField(TEXT("reduction_max_deviation"), Source.ReductionSettings.MaxDeviation);
        J->SetStringField(TEXT("source_import_filename"), Source.SourceImportFilename);
    }
#endif
    return J;
}

static TSharedRef<FJsonObject> SkeletalLodToJson(USkeletalMesh* Mesh, int32 LODIndex)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetNumberField(TEXT("lod_index"), LODIndex);

    FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
    if (RenderData && RenderData->LODRenderData.IsValidIndex(LODIndex))
    {
        const FSkeletalMeshLODRenderData& Lod = RenderData->LODRenderData[LODIndex];
        TArray<uint32> Indices;
        Lod.MultiSizeIndexContainer.GetIndexBuffer(Indices);
        J->SetNumberField(TEXT("vertices"), Lod.GetNumVertices());
        J->SetNumberField(TEXT("triangles"), Indices.Num() / 3);
        J->SetNumberField(TEXT("sections"), Lod.RenderSections.Num());
        J->SetNumberField(TEXT("uv_channels"), Lod.GetNumTexCoords());
        J->SetNumberField(TEXT("color_vertices"), Lod.StaticVertexBuffers.ColorVertexBuffer.GetNumVertices());
    }

    return J;
}

static FOutcome MeshLodsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedMesh Mesh;
    FOutcome Err = ResolveMeshAsset(Args, Mesh);
    if (!Err.bSuccess) return Err;

    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), ObjectPath(Mesh.Asset));
    R->SetStringField(TEXT("class"), Mesh.Asset->GetClass()->GetName());

    TArray<TSharedPtr<FJsonValue>> Lods;
    if (Mesh.StaticMesh)
    {
        const int32 Count = Mesh.StaticMesh->GetNumLODs();
        for (int32 I = 0; I < Count; ++I)
        {
            Lods.Add(MakeShared<FJsonValueObject>(StaticLodToJson(Mesh.StaticMesh, I)));
        }
        R->SetNumberField(TEXT("lod_count"), Count);
        R->SetNumberField(TEXT("source_model_count"), Mesh.StaticMesh->GetNumSourceModels());
        R->SetBoolField(TEXT("auto_compute_lod_screen_size"), Mesh.StaticMesh->GetAutoComputeLODScreenSize());
        R->SetObjectField(TEXT("bounds"), BoundsToJson(Mesh.StaticMesh->GetBounds()));
    }
    else if (Mesh.SkeletalMesh)
    {
        const int32 Count = Mesh.SkeletalMesh->GetLODNum();
        for (int32 I = 0; I < Count; ++I)
        {
            Lods.Add(MakeShared<FJsonValueObject>(SkeletalLodToJson(Mesh.SkeletalMesh, I)));
        }
        R->SetNumberField(TEXT("lod_count"), Count);
        R->SetNumberField(TEXT("source_model_count"), Mesh.SkeletalMesh->GetNumSourceModels());
        R->SetObjectField(TEXT("bounds"), BoundsToJson(Mesh.SkeletalMesh->GetBounds()));
    }
    R->SetArrayField(TEXT("lods"), Lods);
    return FOutcome::MakeSuccess(R);
}

static FOutcome MeshUvsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedMesh Mesh;
    FOutcome Err = ResolveMeshAsset(Args, Mesh);
    if (!Err.bSuccess) return Err;

    const int32 LODIndex = FMath::Max(0, FirstIntArg(Args, {TEXT("lod"), TEXT("lod_index")}, 0));
    const int32 SampleCount = FMath::Clamp(FirstIntArg(Args, {TEXT("sample_count"), TEXT("samples")}, 0), 0, 128);
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), ObjectPath(Mesh.Asset));
    R->SetNumberField(TEXT("lod_index"), LODIndex);

    TArray<TSharedPtr<FJsonValue>> Channels;
    if (Mesh.StaticMesh)
    {
        const FStaticMeshRenderData* RenderData = Mesh.StaticMesh->GetRenderData();
        if (!RenderData || !RenderData->LODResources.IsValidIndex(LODIndex))
        {
            return FOutcome::MakeError(-32602, TEXT("static mesh LOD not available"));
        }
        const FStaticMeshLODResources& Lod = RenderData->LODResources[LODIndex];
        const FStaticMeshVertexBuffer& Buffer = Lod.VertexBuffers.StaticMeshVertexBuffer;
        const uint32 NumChannels = Buffer.GetNumTexCoords();
        const uint32 NumVertices = Buffer.GetNumVertices();
        for (uint32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetNumberField(TEXT("channel"), Channel);
            C->SetNumberField(TEXT("vertex_count"), NumVertices);
            TArray<TSharedPtr<FJsonValue>> Samples;
            for (uint32 I = 0; I < static_cast<uint32>(SampleCount) && I < NumVertices; ++I)
            {
                TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
                S->SetNumberField(TEXT("vertex"), I);
                S->SetObjectField(TEXT("uv"), Vec2ToJson(Buffer.GetVertexUV(I, Channel)));
                Samples.Add(MakeShared<FJsonValueObject>(S));
            }
            C->SetArrayField(TEXT("samples"), Samples);
            Channels.Add(MakeShared<FJsonValueObject>(C));
        }
        R->SetNumberField(TEXT("uv_channel_count"), NumChannels);
        R->SetNumberField(TEXT("vertex_count"), NumVertices);
    }
    else
    {
        FSkeletalMeshRenderData* RenderData = Mesh.SkeletalMesh->GetResourceForRendering();
        if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
        {
            return FOutcome::MakeError(-32602, TEXT("skeletal mesh LOD not available"));
        }
        const FSkeletalMeshLODRenderData& Lod = RenderData->LODRenderData[LODIndex];
        const FStaticMeshVertexBuffer& Buffer = Lod.StaticVertexBuffers.StaticMeshVertexBuffer;
        const uint32 NumChannels = Buffer.GetNumTexCoords();
        const uint32 NumVertices = Buffer.GetNumVertices();
        for (uint32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetNumberField(TEXT("channel"), Channel);
            C->SetNumberField(TEXT("vertex_count"), NumVertices);
            TArray<TSharedPtr<FJsonValue>> Samples;
            for (uint32 I = 0; I < static_cast<uint32>(SampleCount) && I < NumVertices; ++I)
            {
                TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
                S->SetNumberField(TEXT("vertex"), I);
                S->SetObjectField(TEXT("uv"), Vec2ToJson(Buffer.GetVertexUV(I, Channel)));
                Samples.Add(MakeShared<FJsonValueObject>(S));
            }
            C->SetArrayField(TEXT("samples"), Samples);
            Channels.Add(MakeShared<FJsonValueObject>(C));
        }
        R->SetNumberField(TEXT("uv_channel_count"), NumChannels);
        R->SetNumberField(TEXT("vertex_count"), NumVertices);
    }
    R->SetArrayField(TEXT("channels"), Channels);
    return FOutcome::MakeSuccess(R);
}

static FOutcome SkeletalAnalysisImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path = FirstStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("mesh")});
    if (Path.IsEmpty()) return MissingArg(TEXT("path"));
    USkeletalMesh* Mesh = LoadAssetAs<USkeletalMesh>(Path);
    if (!Mesh)
    {
        return FOutcome::MakeError(-32602, FString::Printf(TEXT("not a USkeletalMesh: %s"), *Path));
    }

    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), ObjectPath(Mesh));
    R->SetObjectField(TEXT("bounds"), BoundsToJson(Mesh->GetBounds()));
    R->SetObjectField(TEXT("imported_bounds"), BoundsToJson(Mesh->GetImportedBounds()));
    R->SetNumberField(TEXT("lod_count"), Mesh->GetLODNum());
    R->SetNumberField(TEXT("source_model_count"), Mesh->GetNumSourceModels());
    R->SetNumberField(TEXT("material_count"), Mesh->GetMaterials().Num());
    R->SetBoolField(TEXT("has_vertex_colors"), Mesh->GetHasVertexColors());
    if (const USkeleton* Skeleton = Mesh->GetSkeleton())
    {
        R->SetStringField(TEXT("skeleton"), ObjectPath(Skeleton));
        R->SetNumberField(TEXT("bone_count"), Skeleton->GetReferenceSkeleton().GetNum());
    }
    else
    {
        R->SetStringField(TEXT("skeleton"), FString());
        R->SetNumberField(TEXT("bone_count"), 0);
    }

    FResolvedMesh Resolved;
    Resolved.Asset = Mesh;
    Resolved.SkeletalMesh = Mesh;
    TArray<TSharedPtr<FJsonValue>> Lods;
    for (int32 I = 0; I < Mesh->GetLODNum(); ++I)
    {
        Lods.Add(MakeShared<FJsonValueObject>(SkeletalLodToJson(Mesh, I)));
    }
    R->SetArrayField(TEXT("lods"), Lods);
    return FOutcome::MakeSuccess(R);
}

static FOutcome MeshQualityImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedMesh Mesh;
    FOutcome Err = ResolveMeshAsset(Args, Mesh);
    if (!Err.bSuccess) return Err;

    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), ObjectPath(Mesh.Asset));
    R->SetStringField(TEXT("class"), Mesh.Asset->GetClass()->GetName());

    int32 VertexCount = 0;
    int32 TriangleCount = 0;
    int32 UvChannels = 0;
    int32 SectionCount = 0;
    int32 MaterialCount = 0;
    bool bHasVertexColors = false;
    if (Mesh.StaticMesh)
    {
        MaterialCount = Mesh.StaticMesh->GetStaticMaterials().Num();
        if (const FStaticMeshRenderData* RenderData = Mesh.StaticMesh->GetRenderData();
            RenderData && RenderData->LODResources.Num() > 0)
        {
            const FStaticMeshLODResources& L0 = RenderData->LODResources[0];
            VertexCount = L0.GetNumVertices();
            TriangleCount = L0.GetNumTriangles();
            UvChannels = L0.GetNumTexCoords();
            SectionCount = L0.Sections.Num();
            bHasVertexColors = L0.VertexBuffers.ColorVertexBuffer.GetNumVertices() > 0;
        }
    }
    else
    {
        MaterialCount = Mesh.SkeletalMesh->GetMaterials().Num();
        bHasVertexColors = Mesh.SkeletalMesh->GetHasVertexColors();
        if (FSkeletalMeshRenderData* RenderData = Mesh.SkeletalMesh->GetResourceForRendering();
            RenderData && RenderData->LODRenderData.Num() > 0)
        {
            const FSkeletalMeshLODRenderData& L0 = RenderData->LODRenderData[0];
            TArray<uint32> Indices;
            L0.MultiSizeIndexContainer.GetIndexBuffer(Indices);
            VertexCount = L0.GetNumVertices();
            TriangleCount = Indices.Num() / 3;
            UvChannels = L0.GetNumTexCoords();
            SectionCount = L0.RenderSections.Num();
        }
    }

    UBodySetup* Body = BodySetupFor(Mesh);
    const int32 SimpleCollisionCount = Body ? Body->AggGeom.GetElementCount() : 0;
    TArray<TSharedPtr<FJsonValue>> Issues;
    auto AddIssue = [&Issues](const FString& Code, const FString& Detail)
    {
        TSharedRef<FJsonObject> Issue = MakeShared<FJsonObject>();
        Issue->SetStringField(TEXT("code"), Code);
        Issue->SetStringField(TEXT("detail"), Detail);
        Issues.Add(MakeShared<FJsonValueObject>(Issue));
    };
    if (VertexCount <= 0) AddIssue(TEXT("no_lod0_vertices"), TEXT("LOD0 render data has no vertices or is not loaded"));
    if (TriangleCount <= 0) AddIssue(TEXT("no_lod0_triangles"), TEXT("LOD0 render data has no triangles or is not loaded"));
    if (UvChannels <= 0) AddIssue(TEXT("no_uvs"), TEXT("mesh has no UV channel on LOD0"));
    if (MaterialCount <= 0) AddIssue(TEXT("no_materials"), TEXT("mesh has no material slots"));
    if (!Body) AddIssue(TEXT("no_body_setup"), TEXT("mesh has no UBodySetup"));
    if (Body && SimpleCollisionCount == 0 && Body->CollisionTraceFlag != CTF_UseComplexAsSimple)
    {
        AddIssue(TEXT("no_simple_collision"), TEXT("no simple collision primitives and collision is not complex-as-simple"));
    }
    const int32 HighVertexThreshold = FirstIntArg(Args, {TEXT("high_vertex_threshold")}, 100000);
    if (VertexCount > HighVertexThreshold)
    {
        AddIssue(TEXT("high_vertex_count"),
            FString::Printf(TEXT("LOD0 has %d vertices; threshold is %d"), VertexCount, HighVertexThreshold));
    }

    R->SetNumberField(TEXT("lod0_vertices"), VertexCount);
    R->SetNumberField(TEXT("lod0_triangles"), TriangleCount);
    R->SetNumberField(TEXT("lod0_uv_channels"), UvChannels);
    R->SetNumberField(TEXT("lod0_sections"), SectionCount);
    R->SetNumberField(TEXT("material_count"), MaterialCount);
    R->SetBoolField(TEXT("has_vertex_colors"), bHasVertexColors);
    R->SetNumberField(TEXT("simple_collision_primitive_count"), SimpleCollisionCount);
    R->SetStringField(TEXT("collision_complexity"), Body ? CollisionTraceFlagToString(Body->CollisionTraceFlag) : FString());
    R->SetArrayField(TEXT("issues"), Issues);
    R->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    return FOutcome::MakeSuccess(R);
}

static FOutcome CompareMeshesImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString APath = FirstStringArg(Args, {TEXT("a"), TEXT("source"), TEXT("source_path")});
    FString BPath = FirstStringArg(Args, {TEXT("b"), TEXT("target"), TEXT("target_path")});
    if (APath.IsEmpty() || BPath.IsEmpty())
    {
        return FOutcome::MakeError(-32602, TEXT("missing 'a'/'b' mesh paths"));
    }

    TSharedPtr<FJsonObject> AArgs = MakeShared<FJsonObject>();
    AArgs->SetStringField(TEXT("path"), APath);
    TSharedPtr<FJsonObject> BArgs = MakeShared<FJsonObject>();
    BArgs->SetStringField(TEXT("path"), BPath);

    FOutcome AQ = MeshQualityImpl(AArgs);
    if (!AQ.bSuccess) return AQ;
    FOutcome BQ = MeshQualityImpl(BArgs);
    if (!BQ.bSuccess) return BQ;

    auto R = MakeShared<FJsonObject>();
    R->SetObjectField(TEXT("a"), AQ.Result.ToSharedRef());
    R->SetObjectField(TEXT("b"), BQ.Result.ToSharedRef());
    R->SetStringField(TEXT("a_path"), APath);
    R->SetStringField(TEXT("b_path"), BPath);

    const int32 AV = static_cast<int32>(AQ.Result->GetNumberField(TEXT("lod0_vertices")));
    const int32 BV = static_cast<int32>(BQ.Result->GetNumberField(TEXT("lod0_vertices")));
    const int32 AT = static_cast<int32>(AQ.Result->GetNumberField(TEXT("lod0_triangles")));
    const int32 BT = static_cast<int32>(BQ.Result->GetNumberField(TEXT("lod0_triangles")));
    R->SetNumberField(TEXT("vertex_delta"), BV - AV);
    R->SetNumberField(TEXT("triangle_delta"), BT - AT);
    R->SetBoolField(TEXT("same_asset_class"),
        AQ.Result->GetStringField(TEXT("class")) == BQ.Result->GetStringField(TEXT("class")));
    return FOutcome::MakeSuccess(R);
}

static FOutcome VertexColorsImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedMesh Mesh;
    FOutcome Err = ResolveMeshAsset(Args, Mesh);
    if (!Err.bSuccess) return Err;

    const int32 LODIndex = FMath::Max(0, FirstIntArg(Args, {TEXT("lod"), TEXT("lod_index")}, 0));
    const int32 SampleCount = FMath::Clamp(FirstIntArg(Args, {TEXT("sample_count"), TEXT("samples")}, 16), 0, 256);
    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), ObjectPath(Mesh.Asset));
    R->SetNumberField(TEXT("lod_index"), LODIndex);

    TArray<TSharedPtr<FJsonValue>> Samples;
    if (Mesh.StaticMesh)
    {
        const FStaticMeshRenderData* RenderData = Mesh.StaticMesh->GetRenderData();
        if (!RenderData || !RenderData->LODResources.IsValidIndex(LODIndex))
        {
            return FOutcome::MakeError(-32602, TEXT("static mesh LOD not available"));
        }
        const FColorVertexBuffer& Buffer = RenderData->LODResources[LODIndex].VertexBuffers.ColorVertexBuffer;
        const uint32 Count = Buffer.GetNumVertices();
        for (uint32 I = 0; I < static_cast<uint32>(SampleCount) && I < Count; ++I)
        {
            TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
            S->SetNumberField(TEXT("vertex"), I);
            S->SetObjectField(TEXT("color"), ColorToJson(Buffer.VertexColor(I)));
            Samples.Add(MakeShared<FJsonValueObject>(S));
        }
        R->SetBoolField(TEXT("has_vertex_colors"), Count > 0);
        R->SetNumberField(TEXT("color_vertex_count"), Count);
    }
    else
    {
        const TMap<FVector3f, FColor> Data = Mesh.SkeletalMesh->GetVertexColorData(LODIndex);
        int32 I = 0;
        for (const TPair<FVector3f, FColor>& Pair : Data)
        {
            if (I >= SampleCount) break;
            TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
            TSharedRef<FJsonObject> Pos = MakeShared<FJsonObject>();
            Pos->SetNumberField(TEXT("x"), Pair.Key.X);
            Pos->SetNumberField(TEXT("y"), Pair.Key.Y);
            Pos->SetNumberField(TEXT("z"), Pair.Key.Z);
            S->SetObjectField(TEXT("position"), Pos);
            S->SetObjectField(TEXT("color"), ColorToJson(Pair.Value));
            Samples.Add(MakeShared<FJsonValueObject>(S));
            ++I;
        }
        R->SetBoolField(TEXT("has_vertex_colors"), Mesh.SkeletalMesh->GetHasVertexColors());
        R->SetNumberField(TEXT("color_vertex_count"), Data.Num());
    }
    R->SetArrayField(TEXT("samples"), Samples);
    return FOutcome::MakeSuccess(R);
}

static FOutcome CollisionSummaryImpl(const TSharedPtr<FJsonObject>& Args)
{
    FResolvedMesh Mesh;
    FOutcome Err = ResolveMeshAsset(Args, Mesh);
    if (!Err.bSuccess) return Err;

    UBodySetup* Body = BodySetupFor(Mesh);
    if (!Body)
    {
        return FOutcome::MakeError(-32602, TEXT("mesh has no UBodySetup"));
    }

    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), ObjectPath(Mesh.Asset));
    R->SetStringField(TEXT("collision_complexity"), CollisionTraceFlagToString(Body->CollisionTraceFlag));
    R->SetBoolField(TEXT("double_sided_geometry"), Body->bDoubleSidedGeometry);
    R->SetNumberField(TEXT("box_count"), Body->AggGeom.BoxElems.Num());
    R->SetNumberField(TEXT("sphere_count"), Body->AggGeom.SphereElems.Num());
    R->SetNumberField(TEXT("capsule_count"), Body->AggGeom.SphylElems.Num());
    R->SetNumberField(TEXT("convex_count"), Body->AggGeom.ConvexElems.Num());
    R->SetNumberField(TEXT("total_primitives"), Body->AggGeom.GetElementCount());
    R->SetStringField(TEXT("default_collision_profile"), Body->DefaultInstance.GetCollisionProfileName().ToString());
    return FOutcome::MakeSuccess(R);
}

static FOutcome SetCollisionImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    FResolvedMesh Mesh;
    FOutcome Err = ResolveMeshAsset(Args, Mesh);
    if (!Err.bSuccess) return Err;
    if (FOutcome PieErr; detail::RejectIfPie(PieErr)) return PieErr;

    UBodySetup* Body = BodySetupFor(Mesh);
    if (!Body)
    {
        return FOutcome::MakeError(-32602, TEXT("mesh has no UBodySetup"));
    }

    FString Complexity = FirstStringArg(Args, {TEXT("complexity"), TEXT("collision_complexity"), TEXT("mode")});
    if (Complexity.IsEmpty() && Tool == TEXT("generate_complex_collision"))
    {
        Complexity = TEXT("complex_as_simple");
    }

    FString Preset = FirstStringArg(Args, {TEXT("preset"), TEXT("collision_profile"), TEXT("profile")});
    bool bDoubleSided = Body->bDoubleSidedGeometry;
    const bool bHasDoubleSided = Args.IsValid() && Args->TryGetBoolField(TEXT("double_sided"), bDoubleSided);

    ECollisionTraceFlag NewFlag = Body->CollisionTraceFlag;
    if (!Complexity.IsEmpty() && !ParseCollisionTraceFlag(Complexity, NewFlag))
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("unknown collision complexity: %s"), *Complexity));
    }

    FScopedTransaction Tx(LOCTEXT("SageSetMeshCollision", "Sage: Set Mesh Collision"));
    Mesh.Asset->Modify();
    Body->Modify();
    Body->CollisionTraceFlag = NewFlag;
    if (bHasDoubleSided)
    {
        Body->bDoubleSidedGeometry = bDoubleSided;
    }
    if (!Preset.IsEmpty())
    {
        Body->DefaultInstance.SetCollisionProfileName(FName(*Preset));
    }
    Body->InvalidatePhysicsData();
    Body->CreatePhysicsMeshes();
    Mesh.Asset->MarkPackageDirty();
    Mesh.Asset->PostEditChange();

    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), ObjectPath(Mesh.Asset));
    R->SetStringField(TEXT("tool"), Tool);
    R->SetStringField(TEXT("collision_complexity"), CollisionTraceFlagToString(Body->CollisionTraceFlag));
    R->SetBoolField(TEXT("double_sided_geometry"), Body->bDoubleSidedGeometry);
    R->SetStringField(TEXT("default_collision_profile"), Body->DefaultInstance.GetCollisionProfileName().ToString());
    R->SetNumberField(TEXT("simple_collision_primitive_count"), Body->AggGeom.GetElementCount());
    SaveIfRequested(Mesh.Asset, Args, R);
    return FOutcome::MakeSuccess(R);
}

static FOutcome SetLodSettingsImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    FString Path = FirstStringArg(Args, {TEXT("path"), TEXT("asset"), TEXT("mesh")});
    if (Path.IsEmpty()) return MissingArg(TEXT("path"));
    UStaticMesh* Mesh = LoadAssetAs<UStaticMesh>(Path);
    if (!Mesh)
    {
        return FOutcome::MakeError(-32602, FString::Printf(TEXT("not a UStaticMesh: %s"), *Path));
    }
    if (FOutcome PieErr; detail::RejectIfPie(PieErr)) return PieErr;

    const int32 RequestedLods = FirstIntArg(Args, {TEXT("lod_count"), TEXT("num_lods"), TEXT("target_lod_count")}, INDEX_NONE);
    const double PercentTriangles = FirstNumberArg(Args, {TEXT("percent_triangles"), TEXT("reduction_percent_triangles")}, -1.0);
    const bool bAutoScreen = FirstBoolArg(Args, {TEXT("auto_screen_size"), TEXT("auto_compute_screen_size")}, false);
    const TArray<double> ScreenSizes = NumberArrayArg(Args, TEXT("screen_sizes"));

    FScopedTransaction Tx(LOCTEXT("SageSetMeshLodSettings", "Sage: Set Mesh LOD Settings"));
    Mesh->Modify();
    if (RequestedLods > 0)
    {
        Mesh->SetNumSourceModels(RequestedLods);
    }
    if (bAutoScreen)
    {
        Mesh->SetAutoComputeLODScreenSize(true);
    }
    else if (ScreenSizes.Num() > 0)
    {
        Mesh->SetAutoComputeLODScreenSize(false);
    }

    const int32 SourceCount = Mesh->GetNumSourceModels();
    for (int32 I = 0; I < SourceCount; ++I)
    {
        FStaticMeshSourceModel& Source = Mesh->GetSourceModel(I);
        if (ScreenSizes.IsValidIndex(I))
        {
            Source.ScreenSize.Default = static_cast<float>(ScreenSizes[I]);
        }
        if (PercentTriangles > 0.0 && I > 0)
        {
            Source.ReductionSettings.PercentTriangles = FMath::Clamp(static_cast<float>(PercentTriangles), 0.0f, 1.0f);
        }
    }

    Mesh->MarkPackageDirty();
    Mesh->PostEditChange();

    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("asset"), ObjectPath(Mesh));
    R->SetStringField(TEXT("tool"), Tool);
    R->SetNumberField(TEXT("source_model_count"), Mesh->GetNumSourceModels());
    R->SetBoolField(TEXT("auto_compute_lod_screen_size"), Mesh->GetAutoComputeLODScreenSize());
    TArray<TSharedPtr<FJsonValue>> Lods;
    for (int32 I = 0; I < Mesh->GetNumSourceModels(); ++I)
    {
        Lods.Add(MakeShared<FJsonValueObject>(StaticLodToJson(Mesh, I)));
    }
    R->SetArrayField(TEXT("lods"), Lods);
    SaveIfRequested(Mesh, Args, R);
    return FOutcome::MakeSuccess(R);
}

static TSharedPtr<FJsonValue> FindParamJsonValue(const TSharedPtr<FJsonObject>& Args, const FProperty* Property)
{
    if (!Args.IsValid() || !Property)
    {
        return nullptr;
    }
    const FString Name = Property->GetName();
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(Name))
    {
        return V;
    }
    FString Lower = Name;
    Lower[0] = FChar::ToLower(Lower[0]);
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(Lower))
    {
        return V;
    }
    FString Snake;
    for (int32 I = 0; I < Name.Len(); ++I)
    {
        const TCHAR Ch = Name[I];
        if (I > 0 && FChar::IsUpper(Ch))
        {
            Snake.AppendChar(TEXT('_'));
        }
        Snake.AppendChar(FChar::ToLower(Ch));
    }
    if (TSharedPtr<FJsonValue> V = Args->TryGetField(Snake))
    {
        return V;
    }
    if (Name == TEXT("SectionIndex")) return Args->TryGetField(TEXT("section_index"));
    if (Name == TEXT("bCreateCollision")) return Args->TryGetField(TEXT("create_collision"));
    if (Name == TEXT("bSRGBConversion")) return Args->TryGetField(TEXT("srgb_conversion"));
    return nullptr;
}

static FOutcome InvokeReflectedUFunction(UObject* Target, const TCHAR* FunctionName, const TSharedPtr<FJsonObject>& Args)
{
    if (!Target)
    {
        return FOutcome::MakeError(-32602, TEXT("target object is null"));
    }
    UFunction* Function = Target->FindFunction(FName(FunctionName));
    if (!Function)
    {
        return FOutcome::MakeError(-32602,
            FString::Printf(TEXT("%s has no function %s"),
                            *Target->GetClass()->GetName(), FunctionName));
    }

    FStructOnScope Params(Function);
    uint8* ParamMemory = Params.GetStructMemory();
    TArray<FString> Applied;
    for (TFieldIterator<FProperty> It(Function); It; ++It)
    {
        FProperty* Prop = *It;
        if (!Prop->HasAnyPropertyFlags(CPF_Parm) ||
            Prop->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
        {
            continue;
        }
        TSharedPtr<FJsonValue> Value = FindParamJsonValue(Args, Prop);
        if (!Value.IsValid())
        {
            continue;
        }
        void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(ParamMemory);
        if (!detail::SetPropertyValueAtPtr(Prop, ValuePtr, Value))
        {
            return FOutcome::MakeError(-32602,
                FString::Printf(TEXT("cannot assign parameter '%s' for %s"),
                                *Prop->GetName(), FunctionName));
        }
        Applied.Add(Prop->GetName());
    }

    Target->Modify();
    Target->ProcessEvent(Function, ParamMemory);

    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Name : Applied)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Name));
    }
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("target"), Target->GetPathName());
    R->SetStringField(TEXT("function"), FunctionName);
    R->SetArrayField(TEXT("applied_parameters"), AppliedJson);
    return FOutcome::MakeSuccess(R);
}

static UActorComponent* ResolveComponentByArgs(const TSharedPtr<FJsonObject>& Args, UClass* RequiredClass, FString& OutError)
{
    const FString ComponentPath = FirstStringArg(Args, {TEXT("component"), TEXT("component_path")});
    if (!ComponentPath.IsEmpty())
    {
        UObject* Obj = FindObject<UObject>(nullptr, *ComponentPath);
        if (!Obj)
        {
            FSoftObjectPath Soft(ComponentPath);
            Obj = Soft.ResolveObject();
        }
        UActorComponent* Comp = Cast<UActorComponent>(Obj);
        if (!Comp)
        {
            OutError = FString::Printf(TEXT("component not found: %s"), *ComponentPath);
            return nullptr;
        }
        if (RequiredClass && !Comp->IsA(RequiredClass))
        {
            OutError = FString::Printf(TEXT("component is %s; expected %s"),
                *Comp->GetClass()->GetName(), *RequiredClass->GetName());
            return nullptr;
        }
        return Comp;
    }

    const FString ActorId = FirstStringArg(Args, {TEXT("actor"), TEXT("actor_path"), TEXT("actor_label")});
    if (ActorId.IsEmpty())
    {
        OutError = TEXT("missing 'component' or 'actor'");
        return nullptr;
    }
    AActor* Actor = detail::ResolveActor(ActorId);
    if (!Actor)
    {
        OutError = FString::Printf(TEXT("actor not found: %s"), *ActorId);
        return nullptr;
    }

    const FString Name = FirstStringArg(Args, {TEXT("component_name"), TEXT("name")});
    TArray<UActorComponent*> Components;
    Actor->GetComponents(RequiredClass ? RequiredClass : UActorComponent::StaticClass(), Components);
    for (UActorComponent* Comp : Components)
    {
        if (!Comp) continue;
        if (Name.IsEmpty() || Comp->GetName() == Name || Comp->GetPathName() == Name)
        {
            return Comp;
        }
    }
    OutError = FString::Printf(TEXT("no matching component on actor: %s"), *ActorId);
    return nullptr;
}

static FOutcome ProceduralMeshImpl(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    UClass* ProcMeshClass = FindObject<UClass>(nullptr, TEXT("/Script/ProceduralMeshComponent.ProceduralMeshComponent"));
    if (!ProcMeshClass)
    {
        return Unsupported(Tool, TEXT("ProceduralMeshComponent plugin/module is not loaded"));
    }
    FString Error;
    UActorComponent* Comp = ResolveComponentByArgs(Args, ProcMeshClass, Error);
    if (!Comp)
    {
        return FOutcome::MakeError(-32602, Error);
    }

    FString Action = Tool;
    if (Tool == TEXT("procedural_mesh") || Tool == TEXT("UProceduralMeshComponent"))
    {
        Action = FirstStringArg(Args, {TEXT("action"), TEXT("op")});
        if (Action.IsEmpty())
        {
            TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("component"), Comp->GetPathName());
            R->SetStringField(TEXT("class"), Comp->GetClass()->GetPathName());
            R->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
            for (TFieldIterator<FProperty> It(Comp->GetClass()); It; ++It)
            {
                FProperty* Prop = *It;
                if (TSharedPtr<FJsonValue> Value = detail::GetUPropertyAsJson(Comp, Prop))
                {
                    R->GetObjectField(TEXT("properties"))->SetField(Prop->GetName(), Value);
                }
            }
            return FOutcome::MakeSuccess(R);
        }
    }

    if (Action == TEXT("create_section") || Action == TEXT("create_mesh_section"))
    {
        return InvokeReflectedUFunction(Comp, TEXT("CreateMeshSection_LinearColor"), Args);
    }
    if (Action == TEXT("update_section") || Action == TEXT("update_mesh_section"))
    {
        return InvokeReflectedUFunction(Comp, TEXT("UpdateMeshSection_LinearColor"), Args);
    }
    if (Action == TEXT("clear"))
    {
        if (Args.IsValid() && (Args->HasField(TEXT("section_index")) || Args->HasField(TEXT("SectionIndex"))))
        {
            return InvokeReflectedUFunction(Comp, TEXT("ClearMeshSection"), Args);
        }
        return InvokeReflectedUFunction(Comp, TEXT("ClearAllMeshSections"), Args);
    }
    return Unsupported(Tool, FString::Printf(TEXT("unknown procedural mesh action '%s'"), *Action));
}

static FOutcome SetMaterialImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Error;
    UActorComponent* Comp = ResolveComponentByArgs(Args, UMeshComponent::StaticClass(), Error);
    UMeshComponent* MeshComp = Cast<UMeshComponent>(Comp);
    if (!MeshComp)
    {
        return FOutcome::MakeError(-32602, Error.IsEmpty() ? TEXT("mesh component not found") : Error);
    }
    const FString MaterialPath = FirstStringArg(Args, {TEXT("material"), TEXT("material_path")});
    if (MaterialPath.IsEmpty()) return MissingArg(TEXT("material"));
    UMaterialInterface* Mat = LoadAssetAs<UMaterialInterface>(MaterialPath);
    if (!Mat)
    {
        return FOutcome::MakeError(-32602, FString::Printf(TEXT("material not found: %s"), *MaterialPath));
    }
    const int32 Index = FMath::Max(0, FirstIntArg(Args, {TEXT("index"), TEXT("slot"), TEXT("material_index")}, 0));
    MeshComp->Modify();
    MeshComp->SetMaterial(Index, Mat);
    TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("component"), MeshComp->GetPathName());
    R->SetStringField(TEXT("material"), ObjectPath(Mat));
    R->SetNumberField(TEXT("index"), Index);
    return FOutcome::MakeSuccess(R);
}

static FOutcome RuntimePluginUnsupported(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    const FString ClassPath = Tool.StartsWith(TEXT("realtime")) || Tool == TEXT("create_lod") ||
                             Tool == TEXT("create_section_group") || Tool == TEXT("update_mesh_data") ||
                             Tool == TEXT("set_material_slot") || Tool == TEXT("setup_collision")
        ? TEXT("/Script/RealtimeMeshComponent.RealtimeMeshComponent")
        : TEXT("/Script/GeometryCollectionEngine.GeometryCollection");
    UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
    if (!Class)
    {
        return Unsupported(Tool,
            FString::Printf(TEXT("optional class %s is not loaded; enable the related plugin first"), *ClassPath));
    }

    return FOutcome::MakeError(kUnsupportedCode,
        FString::Printf(TEXT("%s found %s, but mutation mapping is not yet safe/public"), *Tool, *Class->GetPathName()));
}

static FOutcome GeometryScriptUnsupported(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    return Unsupported(Tool,
        TEXT("GeometryScript/DynamicMesh write operations need explicit DynamicMesh conversion and asset-writeback mapping; Sage currently refuses a fake success"));
}

static FOutcome DispatchMeshTool(const FString& Tool, const TSharedPtr<FJsonObject>& Args)
{
    if (Tool == TEXT("get_mesh_lods")) return MeshLodsImpl(Args);
    if (Tool == TEXT("get_mesh_uvs")) return MeshUvsImpl(Args);
    if (Tool == TEXT("analyze_skeletal_mesh")) return SkeletalAnalysisImpl(Args);
    if (Tool == TEXT("analyze_mesh_quality")) return MeshQualityImpl(Args);
    if (Tool == TEXT("compare_meshes")) return CompareMeshesImpl(Args);
    if (Tool == TEXT("get_vertex_colors")) return VertexColorsImpl(Args);

    if (Tool == TEXT("set_mesh_collision") ||
        Tool == TEXT("set_collision_preset") ||
        Tool == TEXT("generate_collision") ||
        Tool == TEXT("generate_complex_collision") ||
        Tool == TEXT("simplify_collision"))
    {
        return SetCollisionImpl(Tool, Args);
    }
    if (Tool == TEXT("generate_lods") ||
        Tool == TEXT("auto_generate_lods") ||
        Tool == TEXT("set_lod_settings") ||
        Tool == TEXT("set_lod_screen_sizes"))
    {
        return SetLodSettingsImpl(Tool, Args);
    }
    if (Tool == TEXT("procedural_mesh") ||
        Tool == TEXT("create_section") ||
        Tool == TEXT("update_section") ||
        Tool == TEXT("clear") ||
        Tool == TEXT("UProceduralMeshComponent"))
    {
        return ProceduralMeshImpl(Tool, Args);
    }
    if (Tool == TEXT("set_material"))
    {
        return SetMaterialImpl(Args);
    }

    if (Tool == TEXT("realtime_mesh") ||
        Tool == TEXT("create_lod") ||
        Tool == TEXT("create_section_group") ||
        Tool == TEXT("update_mesh_data") ||
        Tool == TEXT("set_material_slot") ||
        Tool == TEXT("setup_collision") ||
        Tool == TEXT("chaos_edit"))
    {
        return RuntimePluginUnsupported(Tool, Args);
    }

    if (Tool == TEXT("generate_proxy_mesh") || Tool == TEXT("setup_hlod"))
    {
        return Unsupported(Tool, TEXT("proxy/HLOD generation requires editor build utilities not exposed through this safe asset-level surface"));
    }

    return GeometryScriptUnsupported(Tool, Args);
}

}  // namespace

void RegisterMeshTools(FSageToolDispatch& Dispatch)
{
    auto Register = [&Dispatch](const TCHAR* ToolName)
    {
        Dispatch.RegisterHandler(ToolName,
            [Name = FString(ToolName)](const TSharedPtr<FJsonObject>& Args) -> FOutcome
            {
                return detail::RunOnGameThread([&]() -> FOutcome
                {
                    return DispatchMeshTool(Name, Args);
                });
            });
    };

    static const TCHAR* kMeshTools[] = {
        TEXT("get_mesh_lods"),
        TEXT("get_mesh_uvs"),
        TEXT("analyze_skeletal_mesh"),
        TEXT("analyze_mesh_quality"),
        TEXT("compare_meshes"),
        TEXT("get_vertex_colors"),
        TEXT("mesh_boolean"),
        TEXT("mesh_simplify"),
        TEXT("mesh_remesh"),
        TEXT("mesh_mirror"),
        TEXT("mesh_fill_holes"),
        TEXT("compute_uvs"),
        TEXT("fix_mesh_quality"),
        TEXT("generate_collision"),
        TEXT("generate_lods"),
        TEXT("set_lod_screen_sizes"),
        TEXT("set_collision_preset"),
        TEXT("set_mesh_collision"),
        TEXT("auto_generate_lods"),
        TEXT("generate_proxy_mesh"),
        TEXT("setup_hlod"),
        TEXT("boolean_union"),
        TEXT("boolean_subtract"),
        TEXT("boolean_intersection"),
        TEXT("boolean_trim"),
        TEXT("remesh_uniform"),
        TEXT("remesh_voxel"),
        TEXT("remove_degenerates"),
        TEXT("auto_uv"),
        TEXT("unwrap_uv"),
        TEXT("pack_uv_islands"),
        TEXT("project_uv"),
        TEXT("transform_uvs"),
        TEXT("generate_complex_collision"),
        TEXT("simplify_collision"),
        TEXT("set_lod_settings"),
        TEXT("procedural_mesh"),
        TEXT("create_section"),
        TEXT("update_section"),
        TEXT("clear"),
        TEXT("set_material"),
        TEXT("UProceduralMeshComponent"),
        TEXT("realtime_mesh"),
        TEXT("create_lod"),
        TEXT("create_section_group"),
        TEXT("update_mesh_data"),
        TEXT("set_material_slot"),
        TEXT("setup_collision"),
        TEXT("chaos_edit"),
    };

    for (const TCHAR* ToolName : kMeshTools)
    {
        Register(ToolName);
    }
}

}  // namespace sage::tools

#undef LOCTEXT_NAMESPACE
