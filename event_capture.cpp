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
    quietFrames_ = 0;
    clip_ = {};
}

void EventCapture::onFrame(const FramePtr& frame)
{
    // The ring takes the frame first, so a clip that starts on this frame
    // finds it there as its newest.
    ring_.push(frame);

    const bool triggered = detector_.observe(*frame);
    const bool firing = detector_.firing();

    if (triggered)
    {
        const ClipTrigger trigger{detector_.onsetFrameIndex(), frame->frameIndex, frame->timestamp,
            detector_.lastBrightCount(), false};

        if (collecting_)
        {
            clip_.frames.push_back(frame);
            ++clip_.framesAfter;
            clip_.triggers.push_back(trigger);
        }
        else
        {
            beginClip(trigger);
        }

        if (onCandidate_)
        {
            onCandidate_();
        }
    }
    else if (collecting_)
    {
        clip_.frames.push_back(frame);
        ++clip_.framesAfter;
    }

    if (!collecting_)
    {
        return;
    }

    // The quiet count runs from the last frame on which the detector fired,
    // so a long event is kept whole and the tail follows its end.
    quietFrames_ = firing ? 0 : quietFrames_ + 1;

    if (quietFrames_ >= kPostEventFrames)
    {
        handOff();
        return;
    }

    if (clip_.framesAfter >= kMaxPostEventFrames)
    {
        handOff();
        if (firing)
        {
            beginClip(ClipTrigger{detector_.onsetFrameIndex(), frame->frameIndex, frame->timestamp,
                detector_.lastBrightCount(), true});
        }
    }
}

void EventCapture::finishPartial()
{
    handOff();
}

size_t EventCapture::bufferSize() const
{
    return ring_.size();
}

size_t EventCapture::bufferCapacity() const
{
    return ring_.capacity();
}

void EventCapture::beginClip(const ClipTrigger& trigger)
{
    collecting_ = true;
    quietFrames_ = 0;
    clip_ = {};

    // Pointers to the frames in the ring: the history and the trigger frame.
    clip_.frames = ring_.stored();
    clip_.triggers.push_back(trigger);
    clip_.deltaThreshold = FlashDetector::kDeltaThreshold;
    clip_.minBrightPixels = FlashDetector::kMinBrightPixels;
    clip_.framesBefore = clip_.frames.empty() ? 0 : static_cast<int>(clip_.frames.size()) - 1;
    clip_.framesAfter = 0;
}

void EventCapture::handOff()
{
    if (!collecting_)
    {
        return;
    }

    collecting_ = false;
    quietFrames_ = 0;
    EventClip finished = std::move(clip_);
    clip_ = {};
    if (onClip_)
    {
        onClip_(std::move(finished));
    }
}
