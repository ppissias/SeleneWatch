#pragma once

#include "frame.h"
#include "detection_result.h"

#include <deque>
#include <vector>

// Feature-9: per-pixel running reference and variance, in native sample
// units. A robust grid estimate supplies a noise floor. At least one compact
// patch over K must satisfy the size limits. Spatially separate patches have
// independent persistence; all new candidates are reported.
//
// The source thread owns all state. Frames are read only. Reference and
// variance are held only within passing patch boxes plus one pixel. Background
// elsewhere keeps learning, including while a clip is open. Measurement and
// learning are separate so capture can retain the actual pre-update difference.
// Display requests only produce derived data; they never change decisions.
//
// Feature-11 adds three settings, all off on the Moon. Masked pixels are not
// tested and do not count. A frame where a set share of the unmasked pixels
// is at K fires as a large brightening, and the whole frame is held from
// learning while it lasts. A patch or brightening that fires longer than the
// release limit is learned like the background.
//
// Feature-13 adds the same tests with the sign reversed, off by default:
// dark patches with their own size limits, and a large darkening. Rising
// and falling patches are searched and followed apart. The clip's peak
// preview stays the strongest rising patch.
class FlashDetector
{
public:
    explicit FlashDetector(DetectionSettings settings = {}, MemoryBudget budget = {});
    // Repeated settling frames reuse charged arrays instead of reallocating
    // full images. A normal reset releases them for a new run/layout.
    void reset(bool releaseBuffers = true);
    // Retain the learned reference, but confirm patches again after arming.
    void rearm();
    void setSettings(const DetectionSettings& settings);

    // Source thread, in this order for each frame: analyze, inspect/copy derived
    // measurements, updateReference with that same immutable frame.
    bool analyze(const Frame& frame, DetectionDisplayRequest display = {});
    void updateReference(const Frame& frame);
    [[nodiscard]] const std::vector<DetectionSummary>& candidates() const;
    [[nodiscard]] const DetectionSummary& strongestPatch() const;

    [[nodiscard]] bool firing() const;
    [[nodiscard]] int lastBrightCount() const;
    [[nodiscard]] uint64_t onsetFrameIndex() const;
    [[nodiscard]] const DetectionSummary& summary() const;
    [[nodiscard]] DetectionView view() const;
    // Before updateReference: reuses the caller's charged buffer for this
    // frame's actual pre-update display difference.
    void difference(const Frame& frame, BudgetVector<float>& values) const;

private:
    DetectionSettings settings_;
    DetectionSummary summary_;
    DetectionSummary strongest_;
    MemoryBudget budget_;
    BudgetVector<float> mean_;
    BudgetVector<float> variance_;
    BudgetVector<float> grid_;
    // Sparse 8-connected patch work space, reused between frames.
    BudgetVector<size_t> thresholdPixels_;
    BudgetVector<float> thresholdSigma_;
    BudgetVector<uint8_t> visited_;
    BudgetVector<size_t> pending_;
    BudgetVector<uint8_t> protected_;
    // Feature-11: the mask as pixels, built for one mask and frame size.
    BudgetVector<uint8_t> masked_;
    DetectionMask maskBuiltFor_;
    int maskWidth_{0}, maskHeight_{0};
    size_t unmaskedPixels_{0};
    // A large brightening or darkening in progress: since which frame and
    // for how long. While one is held, the whole frame is held from learning.
    struct LargeChange
    {
        bool active{false};
        uint64_t onset{0};
        double seconds{0};
    };
    LargeChange brightening_, darkening_;
    bool holdAll_{false};
    // Feature-13: the dark side's threshold pixels and its active patches,
    // kept apart from the rising ones so the two are never merged.
    BudgetVector<size_t> darkPixels_;
    BudgetVector<float> darkSigma_;
    // Carries a passing patch across frames for spatial matching and confirmation.
    struct Patch
    {
        DetectionSummary measured;
        int consecutive{0};
        double seconds{0};
    };
    std::vector<Patch> active_;
    std::vector<Patch> activeDark_;
    std::vector<DetectionSummary> candidates_;
    size_t learnedFrames_{0};
    std::deque<DetectionHistoryPoint> history_;
    std::shared_ptr<const DetectionMap> map_;
    int width_{0}, height_{0};
    PixelFormat format_{PixelFormat::Mono8};
    int bits_{0};
    BitAlignment alignment_{BitAlignment::Low};
    BayerPattern bayer_{BayerPattern::None};
    std::chrono::system_clock::time_point previousTime_{};
    double frameInterval_{0};
    bool awaitingUpdate_{false};
};
