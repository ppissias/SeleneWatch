#include "event_capture.h"

#include <utility>

EventCapture::EventCapture(size_t capacity, CandidateStarted onCandidate, ClipReady onClip)
    : ring_(capacity)
    , onCandidate_(std::move(onCandidate))
    , onClip_(std::move(onClip))
{
}

void EventCapture::reset()
{
    ring_.clear();
    detector_.reset();
    collecting_ = false;
    postRemaining_ = 0;
    clip_ = {};
}

void EventCapture::onFrame(Frame& frame)
{
    const bool busy = collecting_ || writesInFlight_.load(std::memory_order_relaxed) > 0;
    if (!busy)
    {
        if (detector_.observe(frame))
        {
            const uint64_t onsetIndex = detector_.onsetFrameIndex();
            const uint64_t triggerIndex = frame.frameIndex;
            const auto triggerTime = frame.timestamp;
            const int brightPixelCount = detector_.lastBrightCount();
            ring_.push(frame);
            beginCapture(onsetIndex, triggerIndex, triggerTime, brightPixelCount);
            return;
        }
    }
    else
    {
        detector_.advanceReference(frame);
        if (collecting_)
        {
            appendPostFrame(frame);
        }
    }

    ring_.push(frame);
}

void EventCapture::finishPartial()
{
    handOff();
}

int EventCapture::onWriteFinished()
{
    return writesInFlight_.fetch_sub(1, std::memory_order_relaxed) - 1;
}

int EventCapture::writesInFlight() const
{
    return writesInFlight_.load(std::memory_order_relaxed);
}

size_t EventCapture::bufferSize() const
{
    return ring_.size();
}

size_t EventCapture::bufferCapacity() const
{
    return ring_.capacity();
}

size_t EventCapture::memoryUsageBytes() const
{
    return ring_.memoryUsageBytes();
}

void EventCapture::beginCapture(uint64_t onsetIndex, uint64_t triggerIndex,
    std::chrono::system_clock::time_point triggerTime, int brightPixelCount)
{
    collecting_ = true;
    clip_ = {};
    clip_.frames = ring_.copyStored();
    clip_.onsetFrameIndex = onsetIndex;
    clip_.triggerFrameIndex = triggerIndex;
    clip_.triggerTime = triggerTime;
    clip_.deltaThreshold = FlashDetector::kDeltaThreshold;
    clip_.minBrightPixels = FlashDetector::kMinBrightPixels;
    clip_.brightPixelCount = brightPixelCount;
    clip_.framesBefore = clip_.frames.empty() ? 0 : static_cast<int>(clip_.frames.size()) - 1;
    clip_.framesAfter = 0;
    postRemaining_ = kPostEventFrames;

    if (onCandidate_)
    {
        onCandidate_();
    }
}

void EventCapture::appendPostFrame(const Frame& frame)
{
    clip_.frames.push_back(frame);
    ++clip_.framesAfter;
    --postRemaining_;
    if (postRemaining_ == 0)
    {
        handOff();
    }
}

void EventCapture::handOff()
{
    if (!collecting_)
    {
        return;
    }

    collecting_ = false;
    postRemaining_ = 0;
    EventClip finished = std::move(clip_);
    clip_ = {};
    if (!onClip_)
    {
        return;
    }

    writesInFlight_.fetch_add(1, std::memory_order_relaxed);
    onClip_(std::move(finished));
}
