#include "flash_detector.h"
#include "synthetic_scene.h"

void FlashDetector::reset()
{
    reference_.clear();
    referenceWidth_ = 0;
    referenceHeight_ = 0;
    hasReference_ = false;
    consecutive_ = 0;
    lastBrightCount_ = 0;
    onsetFrameIndex_ = 0;
}

bool FlashDetector::observe(const Frame& frame)
{
    if (!hasReference_ || referenceWidth_ != frame.width || referenceHeight_ != frame.height
        || reference_.size() != frame.data.size())
    {
        remember(frame);
        return false;
    }

    lastBrightCount_ = countRisingPixels(frame);
    if (lastBrightCount_ >= kMinBrightPixels)
    {
        if (consecutive_ == 0)
        {
            onsetFrameIndex_ = frame.frameIndex;
        }
        ++consecutive_;
        return consecutive_ >= kConsecutiveFrames;
    }

    remember(frame);
    return false;
}

void FlashDetector::advanceReference(const Frame& frame)
{
    remember(frame);
}

int FlashDetector::lastBrightCount() const
{
    return lastBrightCount_;
}

uint64_t FlashDetector::onsetFrameIndex() const
{
    return onsetFrameIndex_;
}

void FlashDetector::remember(const Frame& frame)
{
    reference_ = frame.data;
    referenceWidth_ = frame.width;
    referenceHeight_ = frame.height;
    hasReference_ = true;
    consecutive_ = 0;
    onsetFrameIndex_ = 0;
}

int FlashDetector::countRisingPixels(const Frame& frame) const
{
    const LunarDisk disk = LunarDisk::centered(frame.width, frame.height);

    int rising = 0;
    for (int y = 0; y < frame.height; ++y)
    {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(frame.width);
        for (int x = 0; x < frame.width; ++x)
        {
            if (!disk.contains(x, y))
            {
                continue;
            }

            const size_t index = row + static_cast<size_t>(x);
            const int delta = static_cast<int>(frame.data[index]) - static_cast<int>(reference_[index]);
            if (delta > kDeltaThreshold)
            {
                ++rising;
            }
        }
    }
    return rising;
}
