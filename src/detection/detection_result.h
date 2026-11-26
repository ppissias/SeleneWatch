#pragma once

#include "detection/detection_settings.h"
#include "core/frame.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

// Which derived detector map, if any, to show on the preview (Mask highlights
// pixels excluded by Ignore brighter than, a horizon mask, or shifted borders).
enum class DetectionOverlay { None, Sigma, Noise, Mask };

// A request for derived display data. It has no effect on trigger decisions.
struct DetectionDisplayRequest
{
    DetectionOverlay overlay{DetectionOverlay::None};
    int probeX{-1};
    int probeY{-1};
    bool makeMap{false};
};

// What a measurement found: a compact patch that rose or fell (Feature-13),
// or a large share of the frame that rose (Feature-11) or fell at once.
enum class ChangeKind { Rise, LargeRise, Fall, LargeFall };

// Feature-50: runtime shift stabilization state, active estimator, and why it was chosen.
enum class StabilizationState { Off, Locked, GraceHold, LostFallback };
enum class StabilizationMethod { None, FeatureLock, DiskCentroid };
enum class StabilizationSource { None, IsolatedDisk, GuidingBox, AutoPatch };

[[nodiscard]] inline const char* stabilizationStateName(StabilizationState state)
{
    switch (state)
    {
    case StabilizationState::Off: return "off";
    case StabilizationState::Locked: return "locked";
    case StabilizationState::GraceHold: return "grace_hold";
    case StabilizationState::LostFallback: return "lost_fallback";
    }
    return "off";
}

[[nodiscard]] inline const char* stabilizationMethodName(StabilizationMethod method)
{
    switch (method)
    {
    case StabilizationMethod::None: return "none";
    case StabilizationMethod::FeatureLock: return "feature_lock";
    case StabilizationMethod::DiskCentroid: return "disk_centroid";
    }
    return "none";
}

// Measurements of one frame in native ADU and per-pixel noise units.
struct DetectionSummary
{
    ChangeKind change{ChangeKind::Rise};
    // A large change: the share of the unmasked pixels at K, and the
    // frame's median difference from the background.
    double share{0};
    double medianDifferenceAdu{0};
    // Patches, or a large change, that reached the release limit on this
    // frame and are now learned into the background.
    size_t newlyReleased{0};
    // Feature-13 and Feature-45: with darkening the detector looks for falls,
    // and every measurement here describes them: brightPixels counts pixels
    // that fell by K, peakSigma is the deepest fall in units of the noise,
    // peakAdu its (negative) difference in ADU, and the box and peak place
    // them. patchSearchLimited then says the dark pixels were too many.
    DetectionSettings settings;
    uint64_t frameIndex{0};
    std::chrono::system_clock::time_point time{};
    double noiseFloor{0.5};
    double peakSigma{0};
    double peakAdu{0};
    int peakX{0};
    int peakY{0};
    int brightPixels{0};
    int boxX{0}, boxY{0}, boxWidth{0}, boxHeight{0};
    double probeNoise{0};
    int probeX{-1}, probeY{-1};
    double processingMs{0};
    bool ready{false};
    bool warmingUp{false};
    double warmupTotalSeconds{0};
    double warmupRemainingSeconds{0};
    bool passed{false};
    bool triggered{false};
    bool firing{false};
    bool patchSearchLimited{false}; // The count and box then describe the whole frame.
    bool overlaySkipped{false};
    uint64_t onsetFrameIndex{0};
    size_t newCandidates{0}, activePatches{0}, protectedPixels{0}, learningFrames{0};
    double longestActiveSeconds{0};
    // Feature-50: stabilization telemetry for this frame.
    StabilizationState stabilizationState{StabilizationState::Off};
    StabilizationMethod stabilizationMethod{StabilizationMethod::None};
    StabilizationSource stabilizationSource{StabilizationSource::None};
    double stabilizationDx{0};
    double stabilizationDy{0};
    double stabilizationMs{0};
    int stabilizationLockX{0};
    int stabilizationLockY{0};
    int stabilizationLockWidth{0};
    int stabilizationLockHeight{0};
};

// A compact reading for the live history plot, timestamped by the source.
// With darkening, the deepest fall and the pixels that fell by K.
struct DetectionHistoryPoint
{
    std::chrono::system_clock::time_point time;
    double peakSigma{0};
    int brightPixels{0};
};

// A map of the exact pre-update reference/noise used for a frame's decision:
// the change in sigma (a fall, with darkening), or the noise in ADU. The GUI
// colours it without accessing detector state.
struct DetectionMap
{
    explicit DetectionMap(MemoryBudget budget = {}) : values(BudgetAllocator<float>(std::move(budget))) {}
    int width{0}, height{0};
    DetectionOverlay kind{DetectionOverlay::None};
    BudgetVector<float> values;
};

// Immutable display snapshot, sent together with its frame in the preview slot.
struct DetectionView
{
    DetectionSummary summary;
    std::vector<DetectionHistoryPoint> history;
    std::shared_ptr<const DetectionMap> map;
    bool monitoring{false};
    bool settling{false};
};
