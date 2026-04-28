#include "Tools/SageSequencerTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "SageSeq"

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

// ---- seq.create ----------------------------------------------------------

FSageToolDispatch::FOutcome CreateSequenceImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
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

    FScopedTransaction Tx(LOCTEXT("CreateSeq", "Create Level Sequence"));
    UPackage* Pkg = CreatePackage(*(PackagePath / AssetName));
    if (!Pkg)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("CreatePackage failed for %s/%s"), *PackagePath, *AssetName));
    }
    Pkg->FullyLoad();
    Pkg->Modify();

    ULevelSequence* Seq = NewObject<ULevelSequence>(Pkg, *AssetName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Seq)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            TEXT("NewObject<ULevelSequence> failed"));
    }
    Seq->Initialize();
    FAssetRegistryModule::AssetCreated(Seq);
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),  Seq->GetPathName());
    R->SetStringField(TEXT("name"),  Seq->GetName());
    R->SetStringField(TEXT("class"), Seq->GetClass()->GetName());
    if (UMovieScene* MS = Seq->GetMovieScene())
    {
        R->SetNumberField(TEXT("track_count"), MS->GetTracks().Num());
    }
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- seq.list_tracks -----------------------------------------------------

FSageToolDispatch::FOutcome ListTracksImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    UObject* Asset = ResolveAsset(Path);
    ULevelSequence* Seq = Cast<ULevelSequence>(Asset);
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a ULevelSequence: %s"),
                        Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));

    UMovieScene* MS = Seq->GetMovieScene();
    if (!MS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("sequence has no MovieScene"));
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());

    TArray<TSharedPtr<FJsonValue>> Tracks;
    for (UMovieSceneTrack* T : MS->GetTracks())
    {
        if (!T) continue;
        auto J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("name"),  T->GetName());
        J->SetStringField(TEXT("class"), T->GetClass()->GetName());
        J->SetStringField(TEXT("display_name"), T->GetDisplayName().ToString());
        Tracks.Add(MakeShared<FJsonValueObject>(J));
    }
    R->SetArrayField (TEXT("tracks"),       Tracks);
    R->SetNumberField(TEXT("track_count"),  Tracks.Num());
    R->SetNumberField(TEXT("binding_count"), MS->GetBindings().Num());
    R->SetNumberField(TEXT("possessable_count"), MS->GetPossessableCount());
    R->SetNumberField(TEXT("spawnable_count"),   MS->GetSpawnableCount());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- seq.add_track -------------------------------------------------------

FSageToolDispatch::FOutcome AddTrackImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, ClassPath;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    }
    if (!Args->TryGetStringField(TEXT("track_class"), ClassPath) || ClassPath.IsEmpty())
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'track_class'"));
    }
    UObject* Asset = ResolveAsset(Path);
    ULevelSequence* Seq = Cast<ULevelSequence>(Asset);
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a ULevelSequence: %s"),
                        Asset ? *Asset->GetClass()->GetName() : TEXT("<not found>")));

    UMovieScene* MS = Seq->GetMovieScene();
    if (!MS)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32603,
            TEXT("sequence has no MovieScene"));
    }

    UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassPath);
    if (!Cls || !Cls->IsChildOf(UMovieSceneTrack::StaticClass()))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("track_class %s is not a UMovieSceneTrack"), *ClassPath));
    }
    if (Cls->HasAnyClassFlags(CLASS_Abstract))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("track_class %s is abstract"), *Cls->GetName()));
    }

    FScopedTransaction Tx(LOCTEXT("AddTrack", "Add Track"));
    Seq->Modify();
    MS->Modify();

    UMovieSceneTrack* Track = MS->AddTrack(Cls);
    if (!Track)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("AddTrack returned null for %s"), *Cls->GetName()));
    }
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("sequence"),    Seq->GetPathName());
    R->SetStringField(TEXT("track_name"),  Track->GetName());
    R->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
    R->SetNumberField(TEXT("track_count"), MS->GetTracks().Num());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

}  // namespace (anonymous)

void RegisterSequencerTools(FSageToolDispatch& Dispatch)
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

    Dispatch.RegisterHandler(TEXT("seq.create"),      GT(&CreateSequenceImpl));
    Dispatch.RegisterHandler(TEXT("seq.list_tracks"), GT(&ListTracksImpl));
    Dispatch.RegisterHandler(TEXT("seq.add_track"),   GT(&AddTrackImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
