#pragma once

#include "frame.h"
#include "stream_info.h"
#include "detection_result.h"
#include "capture_settings.h"

#include <chrono>
#include <vector>

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

    DetectionSettings settings;
    double peakSigma{0};
    double noiseFloor{0.5};
    int peakX{0}, peakY{0};
    int boxX{0}, boxY{0}, boxWidth{0}, boxHeight{0};
};

// Frames kept around one or more candidates, oldest first.
// The frames are shared with the ring and with any overlapping clip.
struct EventClip
{
    EventClip() = default;
    explicit EventClip(MemoryBudget budget) : peakDifference(BudgetAllocator<float>(std::move(budget))) {}
    std::vector<FramePtr> frames;

    // At least one. The first trigger names the clip.
    std::vector<ClipTrigger> triggers;

    // Strongest passing frame in this clip and its actual difference from
    // the held running reference. This is derived data, never evidence pixels.
    DetectionSummary peak;
    BudgetVector<float> peakDifference;
    bool previewLimited{false};
    CaptureSettings captureSettings;
    std::string endReason;
    size_t payloadBytes{0};
    size_t historyByteLimit{0}, collectionByteLimit{0}, writerByteLimit{0};
    double availableHistorySeconds{0};

    // Frames before and after the first trigger frame.
    int framesBefore{0};
    int framesAfter{0};

    // What the source said about its stream. Added when the clip is handed
    // to the writer.
    StreamInfo stream;
};
