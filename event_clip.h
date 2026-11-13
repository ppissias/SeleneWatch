#pragma once

#include "frame.h"
#include "stream_info.h"

#include <chrono>
#include <vector>

// A clip ends after this many quiet frames in a row.
inline constexpr int kPostEventFrames = 30;

// A clip collects at most this many frames after its first trigger.
// About 10 s at 30 fps. A longer event continues in the next clip.
inline constexpr int kMaxPostEventFrames = 300;

// One candidate inside a clip.
struct ClipTrigger
{
    uint64_t onsetFrameIndex{0};
    uint64_t triggerFrameIndex{0};
    std::chrono::system_clock::time_point triggerTime{};
    int brightPixelCount{0};

    // True when this is not a new candidate: the previous clip reached its
    // length cap while the detector was still firing, and this clip carries on.
    bool continuesPreviousClip{false};
};

// Frames kept around one or more candidates, oldest first.
// The frames are shared with the ring and with any overlapping clip.
struct EventClip
{
    std::vector<FramePtr> frames;

    // At least one. The first trigger names the clip.
    std::vector<ClipTrigger> triggers;

    // Detector settings in force when the clip started.
    int deltaThreshold{0};
    int minBrightPixels{0};

    // Frames before and after the first trigger frame.
    int framesBefore{0};
    int framesAfter{0};

    // What the source said about its stream. Added when the clip is handed
    // to the writer.
    StreamInfo stream;
};
