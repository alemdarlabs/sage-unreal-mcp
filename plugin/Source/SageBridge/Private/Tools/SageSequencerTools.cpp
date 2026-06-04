#include "Tools/SageSequencerTools.h"
#include "SageBridge.h"
#include "ToolDispatch/SageToolDispatch.h"
#include "Tools/SageToolHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
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

FString GetBindingDisplayName(UMovieScene* MovieScene, const FGuid& BindingGuid)
{
    if (!MovieScene) return FString();
    if (const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(BindingGuid))
    {
        return Possessable->GetName();
    }
    if (const FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(BindingGuid))
    {
        return Spawnable->GetName();
    }
    return FString();
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

// ---- level_sequence_query / sequencer_edit --------------------------------

FString FirstSeqStringArg(const TSharedPtr<FJsonObject>& Args,
                          std::initializer_list<const TCHAR*> Names,
                          const FString& Fallback = FString())
{
    if (!Args.IsValid()) return Fallback;
    FString Value;
    for (const TCHAR* Name : Names)
    {
        if (Args->TryGetStringField(Name, Value) && !Value.IsEmpty())
        {
            return Value;
        }
    }
    return Fallback;
}

int32 SeqMaxResultsArg(const TSharedPtr<FJsonObject>& Args, int32 DefaultValue)
{
    int32 MaxResults = DefaultValue;
    if (Args.IsValid())
    {
        Args->TryGetNumberField(TEXT("max_results"), MaxResults);
        Args->TryGetNumberField(TEXT("limit"), MaxResults);
    }
    return FMath::Clamp(MaxResults, 1, 5000);
}

TSharedPtr<FJsonObject> FrameRateToJson(const FFrameRate& Rate)
{
    auto R = MakeShared<FJsonObject>();
    R->SetNumberField(TEXT("numerator"), Rate.Numerator);
    R->SetNumberField(TEXT("denominator"), Rate.Denominator);
    return R;
}

TSharedPtr<FJsonObject> FrameRangeToJson(const TRange<FFrameNumber>& Range)
{
    auto R = MakeShared<FJsonObject>();
    R->SetBoolField(TEXT("has_lower"), Range.HasLowerBound());
    R->SetBoolField(TEXT("has_upper"), Range.HasUpperBound());
    if (Range.HasLowerBound())
    {
        R->SetNumberField(TEXT("lower_frame"), Range.GetLowerBoundValue().Value);
    }
    if (Range.HasUpperBound())
    {
        R->SetNumberField(TEXT("upper_frame"), Range.GetUpperBoundValue().Value);
    }
    return R;
}

TSharedPtr<FJsonObject> SectionToJson(UMovieSceneSection* Section)
{
    auto R = MakeShared<FJsonObject>();
    if (!Section) return R;
    R->SetStringField(TEXT("name"), Section->GetName());
    R->SetStringField(TEXT("class"), Section->GetClass()->GetPathName());
    R->SetBoolField(TEXT("active"), Section->IsActive());
    R->SetObjectField(TEXT("range"), FrameRangeToJson(Section->GetRange()));

    FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
    R->SetNumberField(TEXT("float_channels"), Proxy.GetChannels<FMovieSceneFloatChannel>().Num());
    R->SetNumberField(TEXT("double_channels"), Proxy.GetChannels<FMovieSceneDoubleChannel>().Num());
    R->SetNumberField(TEXT("integer_channels"), Proxy.GetChannels<FMovieSceneIntegerChannel>().Num());
    R->SetNumberField(TEXT("bool_channels"), Proxy.GetChannels<FMovieSceneBoolChannel>().Num());
    return R;
}

TSharedPtr<FJsonObject> TrackToJson(UMovieSceneTrack* Track)
{
    auto R = MakeShared<FJsonObject>();
    if (!Track) return R;
    R->SetStringField(TEXT("name"), Track->GetName());
    R->SetStringField(TEXT("class"), Track->GetClass()->GetPathName());
    R->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
    TArray<TSharedPtr<FJsonValue>> Sections;
    for (UMovieSceneSection* Section : Track->GetAllSections())
    {
        if (Section)
        {
            Sections.Add(MakeShared<FJsonValueObject>(SectionToJson(Section)));
        }
    }
    R->SetArrayField(TEXT("sections"), Sections);
    R->SetNumberField(TEXT("section_count"), Sections.Num());
    return R;
}

TSharedPtr<FJsonObject> SequenceToJson(ULevelSequence* Seq, bool bIncludeDetails)
{
    auto R = MakeShared<FJsonObject>();
    if (!Seq) return R;
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetStringField(TEXT("name"), Seq->GetName());
    R->SetStringField(TEXT("class"), Seq->GetClass()->GetPathName());

    UMovieScene* MS = Seq->GetMovieScene();
    if (!MS)
    {
        R->SetBoolField(TEXT("has_movie_scene"), false);
        return R;
    }
    R->SetBoolField(TEXT("has_movie_scene"), true);
    R->SetObjectField(TEXT("display_rate"), FrameRateToJson(MS->GetDisplayRate()));
    R->SetObjectField(TEXT("tick_resolution"), FrameRateToJson(MS->GetTickResolution()));
    R->SetObjectField(TEXT("playback_range"), FrameRangeToJson(MS->GetPlaybackRange()));
    R->SetNumberField(TEXT("track_count"), MS->GetTracks().Num());
    R->SetNumberField(TEXT("possessable_count"), MS->GetPossessableCount());
    R->SetNumberField(TEXT("spawnable_count"), MS->GetSpawnableCount());

    const UMovieScene* CMS = MS;
    R->SetNumberField(TEXT("binding_count"), CMS->GetBindings().Num());
    if (!bIncludeDetails) return R;

    TArray<TSharedPtr<FJsonValue>> Tracks;
    for (UMovieSceneTrack* Track : MS->GetTracks())
    {
        if (Track) Tracks.Add(MakeShared<FJsonValueObject>(TrackToJson(Track)));
    }
    R->SetArrayField(TEXT("tracks"), Tracks);

    TArray<TSharedPtr<FJsonValue>> Bindings;
    for (const FMovieSceneBinding& Binding : CMS->GetBindings())
    {
        const FGuid BindingGuid = Binding.GetObjectGuid();
        auto B = MakeShared<FJsonObject>();
        B->SetStringField(TEXT("guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
        B->SetStringField(TEXT("name"), GetBindingDisplayName(MS, BindingGuid));
        TArray<TSharedPtr<FJsonValue>> BindingTracks;
        for (UMovieSceneTrack* Track : Binding.GetTracks())
        {
            if (Track) BindingTracks.Add(MakeShared<FJsonValueObject>(TrackToJson(Track)));
        }
        B->SetArrayField(TEXT("tracks"), BindingTracks);
        B->SetNumberField(TEXT("track_count"), BindingTracks.Num());
        Bindings.Add(MakeShared<FJsonValueObject>(B));
    }
    R->SetArrayField(TEXT("bindings"), Bindings);

    TArray<TSharedPtr<FJsonValue>> Possessables;
    for (int32 Index = 0; Index < MS->GetPossessableCount(); ++Index)
    {
        const FMovieScenePossessable& P = MS->GetPossessable(Index);
        auto PJ = MakeShared<FJsonObject>();
        PJ->SetNumberField(TEXT("index"), Index);
        PJ->SetStringField(TEXT("guid"), P.GetGuid().ToString(EGuidFormats::DigitsWithHyphens));
        PJ->SetStringField(TEXT("name"), P.GetName());
        PJ->SetStringField(TEXT("class"), P.GetPossessedObjectClass() ? P.GetPossessedObjectClass()->GetPathName() : FString());
        Possessables.Add(MakeShared<FJsonValueObject>(PJ));
    }
    R->SetArrayField(TEXT("possessables"), Possessables);

    TArray<TSharedPtr<FJsonValue>> Spawnables;
    for (int32 Index = 0; Index < MS->GetSpawnableCount(); ++Index)
    {
        const FMovieSceneSpawnable& S = MS->GetSpawnable(Index);
        auto SJ = MakeShared<FJsonObject>();
        SJ->SetNumberField(TEXT("index"), Index);
        SJ->SetStringField(TEXT("guid"), S.GetGuid().ToString(EGuidFormats::DigitsWithHyphens));
        SJ->SetStringField(TEXT("name"), S.GetName());
        SJ->SetStringField(TEXT("class"), S.GetObjectTemplate() ? S.GetObjectTemplate()->GetClass()->GetPathName() : FString());
        Spawnables.Add(MakeShared<FJsonValueObject>(SJ));
    }
    R->SetArrayField(TEXT("spawnables"), Spawnables);
    return R;
}

FSageToolDispatch::FOutcome LevelSequenceQueryImpl(const TSharedPtr<FJsonObject>& Args)
{
    FString Path = FirstSeqStringArg(Args, {TEXT("path"), TEXT("sequence")});
    bool bIncludeDetails = true;
    if (Args.IsValid()) Args->TryGetBoolField(TEXT("include_details"), bIncludeDetails);
    const FString Op = FirstSeqStringArg(Args, {TEXT("op"), TEXT("action")}, Path.IsEmpty() ? TEXT("list") : TEXT("get")).ToLower();

    if (!Path.IsEmpty() || Op == TEXT("get") || Op == TEXT("tracks") || Op == TEXT("bindings"))
    {
        if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'/'sequence'"));
        ULevelSequence* Seq = Cast<ULevelSequence>(ResolveAsset(Path));
        if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("not a ULevelSequence: %s"), *Path));
        TSharedPtr<FJsonObject> R = SequenceToJson(Seq, bIncludeDetails || Op != TEXT("get"));
        R->SetStringField(TEXT("op"), Op);
        return FSageToolDispatch::FOutcome::MakeSuccess(R);
    }

    const FString SearchPath = FirstSeqStringArg(Args, {TEXT("folder"), TEXT("directory"), TEXT("search_path")}, TEXT("/Game"));
    const FString Query = FirstSeqStringArg(Args, {TEXT("query"), TEXT("q"), TEXT("name")});
    const int32 MaxResults = SeqMaxResultsArg(Args, 200);

    UClass* SequenceClass = ULevelSequence::StaticClass();
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*SearchPath));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(SequenceClass->GetClassPathName());
    TArray<FAssetData> Assets;
    ARM.Get().GetAssets(Filter, Assets);

    TArray<TSharedPtr<FJsonValue>> Rows;
    for (const FAssetData& Asset : Assets)
    {
        const FString AssetPath = Asset.GetSoftObjectPath().ToString();
        if (!Query.IsEmpty()
            && !Asset.AssetName.ToString().Contains(Query, ESearchCase::IgnoreCase)
            && !AssetPath.Contains(Query, ESearchCase::IgnoreCase))
        {
            continue;
        }
        auto Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Row->SetStringField(TEXT("path"), AssetPath);
        Row->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
        if (bIncludeDetails)
        {
            if (ULevelSequence* Seq = Cast<ULevelSequence>(ResolveAsset(AssetPath)))
            {
                Row->SetObjectField(TEXT("summary"), SequenceToJson(Seq, /*bIncludeDetails=*/false));
            }
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
        if (Rows.Num() >= MaxResults) break;
    }

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("op"), Op);
    R->SetStringField(TEXT("path"), SearchPath);
    if (!Query.IsEmpty()) R->SetStringField(TEXT("query"), Query);
    R->SetArrayField(TEXT("sequences"), Rows);
    R->SetNumberField(TEXT("count"), Rows.Num());
    R->SetBoolField(TEXT("capped"), Rows.Num() >= MaxResults);
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SetPlaybackRangeImpl(const TSharedPtr<FJsonObject>& Args)
{
    FSageToolDispatch::FOutcome Reject;
    if (detail::RejectIfPie(Reject)) return Reject;

    const FString Path = FirstSeqStringArg(Args, {TEXT("path"), TEXT("sequence")});
    if (Path.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'path'/'sequence'"));
    ULevelSequence* Seq = Cast<ULevelSequence>(ResolveAsset(Path));
    if (!Seq) return FSageToolDispatch::FOutcome::MakeError(-32602, FString::Printf(TEXT("not a ULevelSequence: %s"), *Path));
    UMovieScene* MS = Seq->GetMovieScene();
    if (!MS) return FSageToolDispatch::FOutcome::MakeError(-32603, TEXT("no MovieScene"));

    double StartSeconds = 0.0;
    double EndSeconds = 0.0;
    int32 StartFrame = 0;
    int32 EndFrame = 0;
    const bool bHasStartFrame = Args.IsValid() && Args->TryGetNumberField(TEXT("start_frame"), StartFrame);
    const bool bHasEndFrame = Args.IsValid() && Args->TryGetNumberField(TEXT("end_frame"), EndFrame);
    const bool bHasStartSeconds = Args.IsValid() && Args->TryGetNumberField(TEXT("start"), StartSeconds);
    const bool bHasEndSeconds = Args.IsValid() && Args->TryGetNumberField(TEXT("end"), EndSeconds);
    if ((!bHasStartFrame || !bHasEndFrame) && (!bHasStartSeconds || !bHasEndSeconds))
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602,
            TEXT("provide start_frame/end_frame or start/end seconds"));
    }
    const FFrameRate Tick = MS->GetTickResolution();
    const FFrameNumber Lower = bHasStartFrame ? FFrameNumber(StartFrame) : Tick.AsFrameTime(StartSeconds).RoundToFrame();
    const FFrameNumber Upper = bHasEndFrame ? FFrameNumber(EndFrame) : Tick.AsFrameTime(EndSeconds).RoundToFrame();
    if (Upper <= Lower)
    {
        return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("playback range end must be greater than start"));
    }

    FScopedTransaction Tx(LOCTEXT("SetSeqPlaybackRange", "Set Sequencer Playback Range"));
    Seq->Modify();
    MS->Modify();
    MS->SetPlaybackRange(TRange<FFrameNumber>(Lower, Upper));
    Seq->MarkPackageDirty();

    auto R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("path"), Seq->GetPathName());
    R->SetObjectField(TEXT("playback_range"), FrameRangeToJson(MS->GetPlaybackRange()));
    return FSageToolDispatch::FOutcome::MakeSuccess(R);
}

FSageToolDispatch::FOutcome SequencerEditImpl(const TSharedPtr<FJsonObject>& Args)
{
    const FString Op = FirstSeqStringArg(Args, {TEXT("op"), TEXT("action"), TEXT("operation")}).ToLower();
    if (Op.IsEmpty()) return FSageToolDispatch::FOutcome::MakeError(-32602, TEXT("missing 'op'/'action'"));

    if (Op == TEXT("create") || Op == TEXT("create_sequence")) return CreateSequenceImpl(Args);
    if (Op == TEXT("add_track")) return AddTrackImpl(Args);
    if (Op == TEXT("add_keyframe") || Op == TEXT("keyframe") || Op == TEXT("add_key")) return AddKeyframeImpl(Args);
    if (Op == TEXT("add_possessable") || Op == TEXT("bind_actor")) return AddPossessableImpl(Args);
    if (Op == TEXT("add_spawnable")) return AddSpawnableImpl(Args);
    if (Op == TEXT("set_playback_range") || Op == TEXT("set_range")) return SetPlaybackRangeImpl(Args);
    if (Op == TEXT("query") || Op == TEXT("get") || Op == TEXT("list"))
    {
        return LevelSequenceQueryImpl(Args);
    }

    return FSageToolDispatch::FOutcome::MakeError(-32602,
        FString::Printf(TEXT("unsupported sequencer_edit op '%s'; supported ops: create, add_track, add_keyframe, add_possessable, add_spawnable, set_playback_range, query"), *Op));
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
    Dispatch.RegisterHandler(TEXT("level_sequence_query"), GT(&LevelSequenceQueryImpl));
    Dispatch.RegisterHandler(TEXT("sequencer_edit"),       GT(&SequencerEditImpl));
}

#undef LOCTEXT_NAMESPACE

}  // namespace sage::tools
