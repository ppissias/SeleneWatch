#pragma once

#include "frame.h"
#include "detection_result.h"

#include <deque>
#include <vector>

// Feature-9: per-pixel running reference and variance, in native sample
// units. A robust grid estimate supplies a noise floor. At least one compact
// patch over K must satisfy the size limits; the strongest passing patch wins.
//
// The source thread owns all state. Frames are read only. Reference and
// variance freeze on a passing frame and while capture holds a clip open.
// Display requests only produce derived data; they never change decisions.
class FlashDetector
{
public:
    explicit FlashDetector(DetectionSettings settings = {}, MemoryBudget budget = {});
    void reset();
    void setSettings(const DetectionSettings& settings);

    // True once per run: on its first passing frame, or its second when
    // one-frame triggering is off. Invalid frame layouts throw.
    bool observe(const Frame& frame, bool holdReference = false, DetectionDisplayRequest display = {});

    [[nodiscard]] bool firing() const;
    [[nodiscard]] int lastBrightCount() const;
    [[nodiscard]] uint64_t onsetFrameIndex() const;
    [[nodiscard]] const DetectionSummary& summary() const;
    [[nodiscard]] DetectionView view() const;
    // Reuses the caller's charged buffer for a derived display difference.
    void difference(const Frame& frame, BudgetVector<float>& values) const;

private:
    DetectionSettings settings_;
    DetectionSummary summary_;
    MemoryBudget budget_;
    BudgetVector<float> mean_;
    BudgetVector<float> variance_;
    BudgetVector<float> grid_;
    // Sparse 8-connected patch work space, reused between frames.
    BudgetVector<size_t> thresholdPixels_;
    BudgetVector<float> thresholdSigma_;
    BudgetVector<uint8_t> visited_;
    BudgetVector<size_t> pending_;
    size_t learnedFrames_{0};
    std::deque<DetectionHistoryPoint> history_;
    std::shared_ptr<const DetectionMap> map_;
    int width_{0}, height_{0};
    PixelFormat format_{PixelFormat::Mono8};
    int bits_{0};
    BitAlignment alignment_{BitAlignment::Low};
    BayerPattern bayer_{BayerPattern::None};
    std::chrono::system_clock::time_point previousTime_{};
    int consecutive_{0};
    bool runTriggered_{false};
    uint64_t onsetFrameIndex_{0};
};
