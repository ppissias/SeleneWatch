#pragma once

#include "frame.h"

#include <vector>

// The Feature-4 trigger, kept until Feature-9 replaces it. Its thresholds
// were chosen for the synthetic image and are not meant for real data.
//
// Compares each frame with a held reference. The reference stays on the
// pre-flash image while a possible flash is in progress, so the second
// bright frame is not compared with the first bright frame.
//
// It reads one 8-bit value per pixel across the whole frame: Mono8 as
// stored, Mono16 as its top 8 significant bits, so that the threshold keeps
// its meaning, and RGB888 as the mean of the three channels. A Bayer mosaic
// is read as it is. The frame itself is never changed.
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
    // the pre-flash reference. Throws std::invalid_argument for a frame
    // whose data does not match its size and format.
    bool observe(const Frame& frame);

    // True while the latest frame belongs to a run that is a candidate.
    [[nodiscard]] bool firing() const;

    [[nodiscard]] int lastBrightCount() const;

    // First frame of the current run. Meaningful while a run is in progress.
    [[nodiscard]] uint64_t onsetFrameIndex() const;

private:
    void remember();
    [[nodiscard]] int countRisingPixels() const;

    // The 8-bit values of the latest frame and of the reference.
    std::vector<uint8_t> values_;
    std::vector<uint8_t> reference_;
    int referenceWidth_{0};
    int referenceHeight_{0};
    bool hasReference_{false};
    int consecutive_{0};
    int lastBrightCount_{0};
    uint64_t onsetFrameIndex_{0};
};
