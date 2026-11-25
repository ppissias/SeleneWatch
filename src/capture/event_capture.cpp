#include "capture/event_capture.h"

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

void EventCapture::reset(bool releaseDetectorBuffers)
{
    ring_.clear(); detector_.reset(releaseDetectorBuffers); collecting_ = false; lastFiringTime_ = {};
    clip_ = {};
    manualTail_.reset();
}

void EventCapture::resetDetector(bool releaseBuffers)
{
    detector_.reset(releaseBuffers);
}

void EventCapture::recenterDetector(int stepX, int stepY, const Frame& frame, FrameShift newShift)
{
    detector_.recenterReference(stepX, stepY, frame, newShift);
}

// Disarming hands off the available clip. Arming reconfirms ongoing patches in Monitoring.
void EventCapture::setMonitoring(bool monitoring)
{
    if (monitoring_ == monitoring) return;
    if (!monitoring) finishPartial("monitoring stopped");
    else detector_.rearm();
    manualTail_.reset();
    monitoring_ = monitoring;
}

void EventCapture::rememberPeak(const Frame& frame)
{
    const auto& strongest=detector_.strongestPatch();
    if (!strongest.passed || (clip_.peak.ready && strongest.peakSigma <= clip_.peak.peakSigma)) return;
    clip_.peak = strongest;
    try { detector_.difference(frame, clip_.peakDifference); }
    catch (const MemoryLimitError&)
    {
        clip_.peakDifference.clear();
        clip_.previewLimited = true;
    }
}

void EventCapture::onFrame(const FramePtr& frame, DetectionDisplayRequest display, FrameShift shift)
{
    ring_.push(frame);
    detector_.analyze(*frame, display, shift);
    if (!monitoring_)
    {
        detector_.updateReference(*frame);
        return;
    }
    const bool firing = detector_.firing();
    if (!detector_.candidates().empty())
    {
        // A new clip takes the current frame from the ring. An open clip appends it here.
        const bool starting = !collecting_;
        if (collecting_)
        {
            clip_.frames.push_back(frame); ++clip_.framesAfter;
            clip_.payloadBytes += frame->data.capacity();
        }
        else beginClip(currentTrigger(detector_.candidates().front(),false));
        for (size_t i=0;i<detector_.candidates().size();++i)
        {
            const auto trigger=currentTrigger(detector_.candidates()[i],false);
            if (!starting || i>0)
                clip_.triggers.push_back(trigger);
            if (onCandidate_) onCandidate_();
        }
    }
    else if (collecting_)
    {
        clip_.frames.push_back(frame); ++clip_.framesAfter;
        clip_.payloadBytes += frame->data.capacity();
    }
    if (collecting_)
    {
        rememberPeak(*frame);
        if (detector_.summary().newlyReleased) clip_.releases.push_back({frame->frameIndex, frame->timestamp});
        if (firing) lastFiringTime_ = frame->timestamp;
        const double quiet = std::chrono::duration<double>(frame->timestamp - lastFiringTime_).count();
        const double after = std::chrono::duration<double>(frame->timestamp - clip_.triggers.front().triggerTime).count();
        if (quiet >= captureSettings_.postSeconds) handOff("post-event interval complete");
        else if (after >= captureSettings_.maxPostSeconds || clip_.payloadBytes >= maxClipBytes_)
        {
            const bool manualTail = manualTail_ && std::chrono::duration<double>(frame->timestamp-manualTail_->triggerTime).count()<captureSettings_.postSeconds;
            const auto previous = clip_.triggers.front().triggerTime;
            // On the Moon each clip of a long event starts with its own
            // history, so it stands on its own. In Sky mode an aircraft can
            // stay for minutes, so the next clip starts after this one.
            const bool withHistory = captureSettings_.target != Target::Sky;
            handOff(after >= captureSettings_.maxPostSeconds ? "maximum post-event interval" : "clip memory allowance");
            if (firing)
            {
                beginClip(currentTrigger(detector_.summary(),true), withHistory);
                clip_.continuesClipTriggeredAt = previous;
                if (withHistory) rememberPeak(*frame);
            }
            if (manualTail)
            {
                // Preserve the original manual request as collection continues in the next clip.
                auto continuation=*manualTail_;
                continuation.continuesPreviousClip=true;
                continuation.triggerFrameIndex=frame->frameIndex; continuation.triggerTime=frame->timestamp;
                if (collecting_) clip_.triggers.push_back(continuation);
                else
                {
                    beginClip(continuation, withHistory);
                    clip_.continuesClipTriggeredAt = previous;
                    lastFiringTime_=manualTail_->triggerTime;
                }
            }
        }
    }
    // Snapshots above retain the difference used for the decision. Learning
    // follows them, independently of whether evidence is being collected.
    detector_.updateReference(*frame);
}

void EventCapture::finishPartial(std::string reason) { handOff(std::move(reason)); }
size_t EventCapture::bufferSize() const { return ring_.size(); }
RingUsage EventCapture::historyUsage() const { return ring_.usage(); }
void EventCapture::setDetectionSettings(const DetectionSettings& settings) { detector_.setSettings(settings); }
void EventCapture::setDetectorWarmupEnabled(bool enabled) { detector_.setWarmupEnabled(enabled); }
const DetectionSummary& EventCapture::detectionSummary() const { return detector_.summary(); }
const std::vector<DetectionSummary>& EventCapture::detectionCandidates() const { return detector_.candidates(); }
DetectionView EventCapture::detectionView() const { return detector_.view(); }

// Keep the observer's request time and the latest retained source frame time separately.
bool EventCapture::saveNow(std::chrono::system_clock::time_point requestedAt)
{
    if (!monitoring_) return false;
    const auto frames=ring_.stored();
    if (frames.empty()) return false;
    const auto& frame=*frames.back();
    ClipTrigger trigger;
    trigger.kind=TriggerKind::Manual; trigger.requestedAt=requestedAt;
    trigger.settings=detector_.summary().settings;
    trigger.onsetFrameIndex=trigger.triggerFrameIndex=frame.frameIndex;
    trigger.triggerTime=frame.timestamp;
    trigger.requestSourceTime=frame.timestamp;
    if (collecting_) clip_.triggers.push_back(trigger);
    else beginClip(trigger);
    manualTail_=trigger;
    lastFiringTime_=frame.timestamp;
    return true;
}

void EventCapture::beginClip(const ClipTrigger& trigger, bool withHistory)
{
    collecting_ = true;
    clip_ = EventClip(budget_);
    clip_.captureSettings = captureSettings_;
    clip_.historyByteLimit = maxHistoryBytes_; clip_.collectionByteLimit = maxClipBytes_;
    clip_.availableHistorySeconds = withHistory ? ring_.usage().seconds : 0;
    if (withHistory) clip_.frames = ring_.stored();
    for (const auto& frame : clip_.frames) clip_.payloadBytes += frame->data.capacity();
    clip_.triggers.push_back(trigger);
    clip_.framesBefore = clip_.frames.empty() ? 0 : static_cast<int>(clip_.frames.size()) - 1;
    lastFiringTime_ = trigger.triggerTime;
}

ClipTrigger EventCapture::currentTrigger(const DetectionSummary& measured, bool continuation) const
{
    ClipTrigger trigger{measured.onsetFrameIndex, measured.frameIndex, measured.time, measured.brightPixels, continuation};
    trigger.settings = measured.settings;
    trigger.peakSigma = measured.peakSigma; trigger.noiseFloor = measured.noiseFloor;
    trigger.peakX = measured.peakX; trigger.peakY = measured.peakY;
    trigger.boxX = measured.boxX; trigger.boxY = measured.boxY;
    trigger.boxWidth = measured.boxWidth; trigger.boxHeight = measured.boxHeight;
    trigger.activePatches=measured.activePatches; trigger.protectedPixels=measured.protectedPixels;
    trigger.learningFrames=measured.learningFrames; trigger.longestActiveSeconds=measured.longestActiveSeconds;
    trigger.change=measured.change; trigger.share=measured.share; trigger.medianDifferenceAdu=measured.medianDifferenceAdu;
    trigger.peakAdu=measured.peakAdu;
    trigger.stabilizationState=measured.stabilizationState;
    trigger.stabilizationMethod=measured.stabilizationMethod;
    trigger.stabilizationDx=measured.stabilizationDx;
    trigger.stabilizationDy=measured.stabilizationDy;
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
