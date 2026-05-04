#include "Tools/SageSequencerTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneBoolChannel.h"

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
    // UE 5.7: non-const GetBindings() is deprecated (UE_DEPRECATED on
    // UMovieScene). Force the const overload via a const-qualified pointer.
    const UMovieScene* CMS = MS;
    R->SetNumberField(TEXT("binding_count"), CMS->GetBindings().Num());
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

// ---- seq.add_keyframe ------------------------------------------------------

FSageToolDispatch::FOutcome AddKeyframeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString Path, TrackName;
    if (!Args.IsValid() || !Args->TryGetStringField(TEXT("path"), Path))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'"));
    if (!Args->TryGetStringField(TEXT("track_name"), TrackName) || TrackName.IsEmpty())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'track_name'"));

    double TimeSeconds = 0.0;
    if (!Args->TryGetNumberField(TEXT("time"), TimeSeconds))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'time' (seconds)"));

    const TSharedPtr<FJsonValue> ValueField = Args->Values.FindRef(TEXT("value"));
    if (!ValueField.IsValid())
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'value'"));

    int32 ChannelIndex = 0;
    if (Args.IsValid())
    {
        double N = 0;
        if (Args->TryGetNumberField(TEXT("channel_index"), N))
            ChannelIndex = FMath::Max(0, static_cast<int32>(N));
    }

    UObject* Asset = ResolveAsset(Path);
    ULevelSequence* Seq = Cast<ULevelSequence>(Asset);
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a ULevelSequence: %s"), *Path));

    UMovieScene* MS = Seq->GetMovieScene();
    if (!MS) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no MovieScene"));

    UMovieSceneTrack* Track = nullptr;
    for (UMovieSceneTrack* T : MS->GetTracks())
    {
        if (T && T->GetName() == TrackName) { Track = T; break; }
    }
    if (!Track)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("track '%s' not found on sequence"), *TrackName));
    }

    UMovieSceneSection* Section = nullptr;
    for (UMovieSceneSection* S : Track->GetAllSections())
    {
        if (S) { Section = S; break; }
    }
    if (!Section)
    {
        Section = Track->CreateNewSection();
        if (!Section)
            return FSageToolDispatch::FOutcome::MakeError(-32000,
                TEXT("CreateNewSection returned null"));
        Section->SetRange(TRange<FFrameNumber>::All());
        Track->AddSection(*Section);
    }

    const FFrameRate Tick = MS->GetTickResolution();
    const FFrameNumber Frame = Tick.AsFrameTime(TimeSeconds).RoundToFrame();

    FScopedTransaction Tx(LOCTEXT("SeqAddKey", "Add Sequencer Keyframe"));
    Section->Modify();

    FString ChannelTypeUsed;
    bool bAdded = false;

    // Try float channel first, then double, integer, bool — in declaration order.
    FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
    {
        TArrayView<FMovieSceneFloatChannel*> Channels =
            Proxy.GetChannels<FMovieSceneFloatChannel>();
        if (Channels.IsValidIndex(ChannelIndex))
        {
            double V = 0.0; ValueField->TryGetNumber(V);
            Channels[ChannelIndex]->AddCubicKey(Frame, static_cast<float>(V));
            ChannelTypeUsed = TEXT("float");
            bAdded = true;
        }
    }
    if (!bAdded)
    {
        TArrayView<FMovieSceneDoubleChannel*> Channels =
            Proxy.GetChannels<FMovieSceneDoubleChannel>();
        if (Channels.IsValidIndex(ChannelIndex))
        {
            double V = 0.0; ValueField->TryGetNumber(V);
            Channels[ChannelIndex]->AddCubicKey(Frame, V);
            ChannelTypeUsed = TEXT("double");
            bAdded = true;
        }
    }
    if (!bAdded)
    {
        TArrayView<FMovieSceneIntegerChannel*> Channels =
            Proxy.GetChannels<FMovieSceneIntegerChannel>();
        if (Channels.IsValidIndex(ChannelIndex))
        {
            double V = 0.0; ValueField->TryGetNumber(V);
            Channels[ChannelIndex]->GetData().AddKey(Frame, static_cast<int32>(V));
            ChannelTypeUsed = TEXT("integer");
            bAdded = true;
        }
    }
    if (!bAdded)
    {
        TArrayView<FMovieSceneBoolChannel*> Channels =
            Proxy.GetChannels<FMovieSceneBoolChannel>();
        if (Channels.IsValidIndex(ChannelIndex))
        {
            bool B = false;
            if (!ValueField->TryGetBool(B))
            {
                double V = 0.0; ValueField->TryGetNumber(V);
                B = V != 0.0;
            }
            Channels[ChannelIndex]->GetData().AddKey(Frame, B);
            ChannelTypeUsed = TEXT("bool");
            bAdded = true;
        }
    }

    if (!bAdded)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            FString::Printf(TEXT("track '%s' has no float/double/integer/bool channel "
                                  "at index %d (or unsupported channel type)"),
                            *TrackName, ChannelIndex));
    }

    TRange<FFrameNumber> Range = MS->GetPlaybackRange();
    if (Range.HasUpperBound() && Frame > Range.GetUpperBoundValue())
    {
        MS->SetPlaybackRange(TRange<FFrameNumber>(Range.GetLowerBoundValue(), Frame));
    }
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"),          Seq->GetPathName());
    R->SetStringField(TEXT("track_name"),    TrackName);
    R->SetNumberField(TEXT("time"),          TimeSeconds);
    R->SetNumberField(TEXT("frame"),         Frame.Value);
    R->SetNumberField(TEXT("channel_index"), ChannelIndex);
    R->SetStringField(TEXT("channel_type"),  ChannelTypeUsed);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- seq.add_possessable ---------------------------------------------------

FSageToolDispatch::FOutcome AddPossessableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SeqPath, ActorId;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"),     SeqPath)
        || !Args->TryGetStringField(TEXT("actor_id"), ActorId))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path' or 'actor_id'"));

    UObject* Asset = ResolveAsset(SeqPath);
    ULevelSequence* Seq = Cast<ULevelSequence>(Asset);
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a ULevelSequence: %s"), *SeqPath));

    AActor* A = detail::ResolveActor(ActorId);
    if (!A) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("actor not found: %s"), *ActorId));

    UMovieScene* MS = Seq->GetMovieScene();
    if (!MS) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no MovieScene"));

    FScopedTransaction Tx(LOCTEXT("AddPossessable", "Add Possessable"));
    Seq->Modify();
    MS->Modify();

    FGuid Guid = MS->AddPossessable(A->GetActorLabel(), A->GetClass());
    Seq->BindPossessableObject(Guid, *A, A->GetWorld());
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("sequence"),   Seq->GetPathName());
    R->SetStringField(TEXT("actor_id"),   A->GetPathName());
    R->SetStringField(TEXT("guid"),       Guid.ToString(EGuidFormats::DigitsWithHyphens));
    R->SetStringField(TEXT("label"),      A->GetActorLabel());
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

// ---- seq.add_spawnable -----------------------------------------------------

FSageToolDispatch::FOutcome AddSpawnableImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    FString SeqPath, ClassPath;
    if (!Args.IsValid()
        || !Args->TryGetStringField(TEXT("path"),       SeqPath)
        || !Args->TryGetStringField(TEXT("class_path"), ClassPath))
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path' or 'class_path'"));

    UObject* Asset = ResolveAsset(SeqPath);
    ULevelSequence* Seq = Cast<ULevelSequence>(Asset);
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("not a ULevelSequence: %s"), *SeqPath));

    UClass* Cls = FindObject<UClass>(nullptr, *ClassPath);
    if (!Cls) Cls = LoadObject<UClass>(nullptr, *ClassPath);
    if (!Cls) return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("class not found: %s"), *ClassPath));

    UMovieScene* MS = Seq->GetMovieScene();
    if (!MS) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no MovieScene"));

    FScopedTransaction Tx(LOCTEXT("AddSpawnable", "Add Spawnable"));
    Seq->Modify();
    MS->Modify();

    // Create a template object for the spawnable. UE 5.7 spawnable templates
    // require RF_ArchetypeObject in addition to RF_Transactional — without it
    // the spawnable serialize/instantiate path treats the object as a regular
    // outer-of-MovieScene UObject and fails to spawn at runtime.
    UObject* Template = NewObject<UObject>(Seq, Cls,
        FName(*Cls->GetName()), RF_Transactional | RF_ArchetypeObject);
    if (!Template)
    {
        Tx.Cancel();
        return FSageToolDispatch::FOutcome::MakeError(-32000,
            FString::Printf(TEXT("NewObject(template) returned null for %s"), *Cls->GetName()));
    }
    const FGuid SpawnableGuid = MS->AddSpawnable(Cls->GetName(), *Template);
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("sequence"),   Seq->GetPathName());
    R->SetStringField(TEXT("class"),      Cls->GetName());
    R->SetStringField(TEXT("guid"),       SpawnableGuid.ToString(EGuidFormats::DigitsWithHyphens));
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

    Dispatch.RegisterHandler(TEXT("seq.create"),           GT(&CreateSequenceImpl));
    Dispatch.RegisterHandler(TEXT("seq.list_tracks"),      GT(&ListTracksImpl));
    Dispatch.RegisterHandler(TEXT("seq.add_track"),        GT(&AddTrackImpl));
    Dispatch.RegisterHandler(TEXT("seq.add_keyframe"),     GT(&AddKeyframeImpl));
    Dispatch.RegisterHandler(TEXT("seq.add_possessable"),  GT(&AddPossessableImpl));
    Dispatch.RegisterHandler(TEXT("seq.add_spawnable"),    GT(&AddSpawnableImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
