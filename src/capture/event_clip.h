#pragma once

#include "core/frame.h"
#include "core/observing_site.h"
#include "core/stream_info.h"
#include "detection/detection_result.h"
#include "core/capture_settings.h"

#include <chrono>
#include <optional>
#include <vector>

// How a trigger entered the clip, through detection or the observer's Save now request.
enum class TriggerKind { Automatic, Manual };

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
    size_t activePatches{0}, protectedPixels{0}, learningFrames{0};
    double longestActiveSeconds{0};
    TriggerKind kind{TriggerKind::Automatic};
    std::chrono::system_clock::time_point requestedAt{}; // Host UTC for a manual request.
    std::chrono::system_clock::time_point requestSourceTime{}; // Original application time, retained across continuations.

    // What fired an automatic trigger: a patch that rose or fell (Feature-13),
    // or a large share of the frame (Feature-11), with that share and the
    // frame's median difference. peakAdu is the strongest pixel's difference
    // from the background, negative for a fall.
    ChangeKind change{ChangeKind::Rise};
    double share{0};
    double medianDifferenceAdu{0};
    double peakAdu{0};
};

// A patch or large change in the clip that reached the release limit and
// from then on was learned into the background (Feature-11).
struct ReleaseRecord
{
    uint64_t frameIndex{0};
    std::chrono::system_clock::time_point time{};
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
    // the pre-update reference. This is derived data, never evidence pixels.
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

    std::vector<ReleaseRecord> releases;
    // When this clip continues a long event: the first trigger time of the
    // clip before, which names that clip's folder.
    std::optional<std::chrono::system_clock::time_point> continuesClipTriggeredAt;

    // What the source said about its stream. Added when the clip is handed
    // to the writer.
    StreamInfo stream;

    // Feature-14, added at hand-off too: who observed from where with what,
    // the last clock check, and the continuous recording the clip fell in.
    ObservingSite site;
    std::string clockRecord;
    std::string duringRecording;
};
