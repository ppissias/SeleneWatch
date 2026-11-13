#include "event_capture.h"

#include <utility>

EventCapture::EventCapture(size_t capacity, CandidateStarted onCandidate, ClipReady onClip, DetectionSettings settings)
    : ring_(capacity)
    , onCandidate_(std::move(onCandidate))
    , onClip_(std::move(onClip))
    , detector_(settings)
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

void EventCapture::onFrame(const FramePtr& frame, DetectionDisplayRequest display)
{
    // The ring takes the frame first, so a clip that starts on this frame
    // finds it there as its newest.
    ring_.push(frame);

    const bool triggered = detector_.observe(*frame, collecting_, display);
    const bool firing = detector_.firing();

    if (triggered)
    {
        const ClipTrigger trigger = currentTrigger(*frame, false);

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

    if (detector_.summary().passed && (clip_.peakDifference.empty()
        || detector_.summary().peakSigma > clip_.peak.peakSigma))
    {
        clip_.peak = detector_.summary();
        clip_.peakDifference = detector_.difference(*frame);
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
            beginClip(currentTrigger(*frame, true));
            clip_.peak = detector_.summary();
            clip_.peakDifference = detector_.difference(*frame);
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
    clip_.framesBefore = clip_.frames.empty() ? 0 : static_cast<int>(clip_.frames.size()) - 1;
    clip_.framesAfter = 0;
}

void EventCapture::setDetectionSettings(const DetectionSettings& settings) { detector_.setSettings(settings); }
const DetectionSummary& EventCapture::detectionSummary() const { return detector_.summary(); }
DetectionView EventCapture::detectionView() const { return detector_.view(); }

ClipTrigger EventCapture::currentTrigger(const Frame& frame, bool continuation) const
{
    const DetectionSummary& measured = detector_.summary();
    ClipTrigger trigger{detector_.onsetFrameIndex(), frame.frameIndex, frame.timestamp, measured.brightPixels, continuation};
    trigger.settings = measured.settings;
    trigger.peakSigma = measured.peakSigma; trigger.noiseFloor = measured.noiseFloor;
    trigger.peakX = measured.peakX; trigger.peakY = measured.peakY;
    trigger.boxX = measured.boxX; trigger.boxY = measured.boxY;
    trigger.boxWidth = measured.boxWidth; trigger.boxHeight = measured.boxHeight;
    return trigger;
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
