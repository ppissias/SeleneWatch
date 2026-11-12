#pragma once

#include "frame.h"

#include <chrono>
#include <vector>

inline constexpr int kPostEventFrames = 30;

// Frames kept around one candidate, oldest first.
struct EventClip
{
    std::vector<Frame> frames;
    uint64_t onsetFrameIndex{0};
    uint64_t triggerFrameIndex{0};
    std::chrono::system_clock::time_point triggerTime{};
    int deltaThreshold{0};
    int minBrightPixels{0};
    int brightPixelCount{0};
    int framesBefore{0};
    int framesAfter{0};
};
