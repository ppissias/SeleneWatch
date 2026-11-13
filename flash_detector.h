#pragma once

#include "frame.h"

#include <vector>

// Compares each frame with a held reference. The reference stays on the
// pre-flash image while a possible flash is in progress, so the second
// bright frame is not compared with the first bright frame.
//
// A run is a stretch of consecutive rising frames. A run becomes a
// candidate on its second frame, and it is one candidate however long it
// lasts. The next candidate needs a quiet frame first.
class FlashDetector
{
public:
    static constexpr int kDeltaThreshold = 60;
    static constexpr int kMinBrightPixels = 120;
    static constexpr int kConsecutiveFrames = 2;

    void reset();

    // Call once for every frame. True on the one frame where a run becomes
    // a candidate: the second consecutive frame that rises enough against
    // the pre-flash reference.
    bool observe(const Frame& frame);

    // True while the latest frame belongs to a run that is a candidate.
    [[nodiscard]] bool firing() const;

    [[nodiscard]] int lastBrightCount() const;

    // First frame of the current run. Meaningful while a run is in progress.
    [[nodiscard]] uint64_t onsetFrameIndex() const;

private:
    void remember(const Frame& frame);
    [[nodiscard]] int countRisingPixels(const Frame& frame) const;

    std::vector<uint8_t> reference_;
    int referenceWidth_{0};
    int referenceHeight_{0};
    bool hasReference_{false};
    int consecutive_{0};
    int lastBrightCount_{0};
    uint64_t onsetFrameIndex_{0};
};
