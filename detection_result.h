#pragma once

#include "detection_settings.h"
#include "frame.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

enum class DetectionOverlay { None, Sigma, Noise };

// A request for derived display data. It has no effect on trigger decisions.
struct DetectionDisplayRequest
{
    DetectionOverlay overlay{DetectionOverlay::None};
    int probeX{-1};
    int probeY{-1};
    bool makeMap{false};
};

// Measurements of one frame in native ADU and per-pixel noise units.
struct DetectionSummary
{
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
    bool passed{false};
    bool triggered{false};
    bool firing{false};
};

struct DetectionHistoryPoint
{
    std::chrono::system_clock::time_point time;
    double peakSigma{0};
    int brightPixels{0};
};

// A map of the exact pre-update reference/noise used
// for a frame's decision. The GUI colours it without accessing detector state.
struct DetectionMap
{
    int width{0}, height{0};
    DetectionOverlay kind{DetectionOverlay::None};
    std::vector<float> values;
};

// Immutable display snapshot, sent together with its frame in the preview slot.
struct DetectionView
{
    DetectionSummary summary;
    std::vector<DetectionHistoryPoint> history;
    std::shared_ptr<const DetectionMap> map;
};
