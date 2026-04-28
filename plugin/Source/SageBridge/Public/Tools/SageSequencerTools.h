#pragma once

#include "CoreMinimal.h"

class FSageToolDispatch;

namespace sage::tools
{
/**
 * Registers Sequencer authoring handlers (Phase 4.6 round 3 batch 6):
 * seq.create      — create ULevelSequence
 * seq.list_tracks — read root tracks (UMovieScene::GetTracks)
 * seq.add_track   — add a UMovieSceneTrack subclass to the root
 *
 * All authoring runs on GameThread.
 */
SAGEBRIDGE_API void RegisterSequencerTools(FSageToolDispatch& Dispatch);

}  // namespace sage::tools
