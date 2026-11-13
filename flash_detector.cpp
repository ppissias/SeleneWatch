#include "flash_detector.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

// One 8-bit value per pixel, as described in the class comment.
void readValues(const Frame& frame, std::vector<uint8_t>& values)
{
    if (frame.width <= 0 || frame.height <= 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format))
    {
        throw std::invalid_argument("frame " + std::to_string(frame.frameIndex)
            + " holds data that does not match its size and format");
    }

    const size_t pixels = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    values.resize(pixels);
    const uint8_t* data = frame.data.data();

    switch (frame.format)
    {
    case PixelFormat::Mono8:
        std::copy(data, data + pixels, values.begin());
        break;

    case PixelFormat::Mono16:
    {
        const int shift = shiftToEightBits(frame);
        for (size_t i = 0; i < pixels; ++i)
        {
            const int sample = data[2 * i] | (data[2 * i + 1] << 8);
            values[i] = static_cast<uint8_t>(std::min(sample >> shift, 255));
        }
        break;
    }

    case PixelFormat::RGB888:
        for (size_t i = 0; i < pixels; ++i)
        {
            values[i] = static_cast<uint8_t>((data[3 * i] + data[3 * i + 1] + data[3 * i + 2]) / 3);
        }
        break;
    }
}

} // namespace

void FlashDetector::reset()
{
    values_.clear();
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
    readValues(frame, values_);

    if (!hasReference_ || referenceWidth_ != frame.width || referenceHeight_ != frame.height)
    {
        referenceWidth_ = frame.width;
        referenceHeight_ = frame.height;
        remember();
        return false;
    }

    lastBrightCount_ = countRisingPixels();
    if (lastBrightCount_ >= kMinBrightPixels)
    {
        if (consecutive_ == 0)
        {
            onsetFrameIndex_ = frame.frameIndex;
        }
        ++consecutive_;
        return consecutive_ == kConsecutiveFrames;
    }

    remember();
    return false;
}

bool FlashDetector::firing() const
{
    return consecutive_ >= kConsecutiveFrames;
}

int FlashDetector::lastBrightCount() const
{
    return lastBrightCount_;
}

uint64_t FlashDetector::onsetFrameIndex() const
{
    return onsetFrameIndex_;
}

// The latest frame becomes the reference. The old reference's buffer is
// kept for the next frame's values, so nothing is allocated per frame.
void FlashDetector::remember()
{
    std::swap(reference_, values_);
    hasReference_ = true;
    consecutive_ = 0;
    onsetFrameIndex_ = 0;
}

int FlashDetector::countRisingPixels() const
{
    int rising = 0;
    for (size_t i = 0; i < values_.size(); ++i)
    {
        if (static_cast<int>(values_[i]) - static_cast<int>(reference_[i]) > kDeltaThreshold)
        {
            ++rising;
        }
    }
    return rising;
}
