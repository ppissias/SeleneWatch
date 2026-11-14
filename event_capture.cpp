#include "event_capture.h"

#include <stdexcept>
#include <utility>

EventCapture::EventCapture(CaptureSettings capture, CandidateStarted onCandidate, ClipReady onClip,
    DetectionSettings settings, MemoryBudget budget)
    : ring_(capture.memoryBytes / 4, capture.historySeconds), budget_(std::move(budget)),
      captureSettings_(capture), maxClipBytes_(capture.memoryBytes * 2 / 5), maxHistoryBytes_(capture.memoryBytes / 4),
      onCandidate_(std::move(onCandidate)), onClip_(std::move(onClip)), detector_(settings, budget_) {}

void EventCapture::configure(CaptureSettings capture, size_t clipBytes, size_t historyBytes)
{
    if (!capture.valid()) throw std::invalid_argument("invalid capture settings");
    reset();
    captureSettings_ = capture; maxClipBytes_ = clipBytes;
    maxHistoryBytes_ = historyBytes;
    ring_.configure(historyBytes, capture.historySeconds);
}

void EventCapture::reset()
{
    ring_.clear(); detector_.reset(); collecting_ = false; lastFiringTime_ = {};
    clip_ = {};
}

void EventCapture::rememberPeak(const Frame& frame)
{
    if (!detector_.summary().passed || (clip_.peak.ready && detector_.summary().peakSigma <= clip_.peak.peakSigma)) return;
    clip_.peak = detector_.summary();
    try { detector_.difference(frame, clip_.peakDifference); }
    catch (const MemoryLimitError&)
    {
        clip_.peakDifference.clear();
        clip_.previewLimited = true;
    }
}

void EventCapture::onFrame(const FramePtr& frame, DetectionDisplayRequest display)
{
    ring_.push(frame);
    const bool triggered = detector_.observe(*frame, collecting_, display);
    const bool firing = detector_.firing();
    if (triggered)
    {
        const ClipTrigger trigger = currentTrigger(*frame, false);
        if (collecting_)
        {
            clip_.frames.push_back(frame); ++clip_.framesAfter;
            clip_.payloadBytes += frame->data.capacity();
            clip_.triggers.push_back(trigger);
        }
        else beginClip(trigger);
        if (onCandidate_) onCandidate_();
    }
    else if (collecting_)
    {
        clip_.frames.push_back(frame); ++clip_.framesAfter;
        clip_.payloadBytes += frame->data.capacity();
    }
    if (!collecting_) return;
    rememberPeak(*frame);
    if (firing) lastFiringTime_ = frame->timestamp;
    const double quiet = std::chrono::duration<double>(frame->timestamp - lastFiringTime_).count();
    const double after = std::chrono::duration<double>(frame->timestamp - clip_.triggers.front().triggerTime).count();
    if (quiet >= captureSettings_.postSeconds) { handOff("post-event interval complete"); return; }
    if (after >= captureSettings_.maxPostSeconds || clip_.payloadBytes >= maxClipBytes_)
    {
        handOff(after >= captureSettings_.maxPostSeconds ? "maximum post-event interval" : "clip memory allowance");
        if (firing) { beginClip(currentTrigger(*frame, true)); rememberPeak(*frame); }
    }
}

void EventCapture::finishPartial(std::string reason) { handOff(std::move(reason)); }
size_t EventCapture::bufferSize() const { return ring_.size(); }
RingUsage EventCapture::historyUsage() const { return ring_.usage(); }
void EventCapture::setDetectionSettings(const DetectionSettings& settings) { detector_.setSettings(settings); }
const DetectionSummary& EventCapture::detectionSummary() const { return detector_.summary(); }
DetectionView EventCapture::detectionView() const { return detector_.view(); }

void EventCapture::beginClip(const ClipTrigger& trigger)
{
    collecting_ = true;
    clip_ = EventClip(budget_);
    clip_.captureSettings = captureSettings_;
    clip_.historyByteLimit = maxHistoryBytes_; clip_.collectionByteLimit = maxClipBytes_;
    clip_.availableHistorySeconds = ring_.usage().seconds;
    clip_.frames = ring_.stored();
    for (const auto& frame : clip_.frames) clip_.payloadBytes += frame->data.capacity();
    clip_.triggers.push_back(trigger);
    clip_.framesBefore = clip_.frames.empty() ? 0 : static_cast<int>(clip_.frames.size()) - 1;
    lastFiringTime_ = trigger.triggerTime;
}

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

void EventCapture::handOff(std::string reason)
{
    if (!collecting_) return;
    collecting_ = false; lastFiringTime_ = {};
    clip_.endReason = std::move(reason);
    EventClip finished = std::move(clip_); clip_ = {};
    if (onClip_) onClip_(std::move(finished));
}
