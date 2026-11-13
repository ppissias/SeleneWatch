#pragma once

#include "event_clip.h"
#include "flash_detector.h"
#include "frame.h"
#include "frame_ring_buffer.h"

#include <functional>

// Runs the detector on every frame and collects the clips around candidates.
//
// Owns the ring, the flash detector, and the clip in progress. It does not
// know how busy the writer is: every finished clip is handed to onClip, and
// whoever receives it decides whether it can be saved.
//
// A clip starts at a candidate with the frames then in the ring. It ends
// after kPostEventFrames quiet frames in a row, or at kMaxPostEventFrames
// frames after its first trigger. A candidate that fires while a clip is
// open joins that clip. If the detector is still firing when a clip reaches
// its cap, the next clip starts at once and overlaps the one just ended.
//
// The Session calls onFrame, reset, and finishPartial: on the source thread
// while the source runs, and on the window thread only while the source
// thread is joined or has not started. The buffer queries take the ring lock
// and may be called from the window timer during streaming.
class EventCapture
{
public:
    using CandidateStarted = std::function<void()>;
    using ClipReady = std::function<void(EventClip clip)>;

    EventCapture(size_t capacity, CandidateStarted onCandidate, ClipReady onClip, DetectionSettings settings = {});

    // Drops the ring, the detector state, and any clip still being collected.
    void reset();

    // Source thread. May call the candidate and clip callbacks before returning.
    void onFrame(const FramePtr& frame, DetectionDisplayRequest display = {});
    void setDetectionSettings(const DetectionSettings& settings);
    [[nodiscard]] const DetectionSummary& detectionSummary() const;
    [[nodiscard]] DetectionView detectionView() const;

    // Hands off a short clip when Stop interrupts collection.
    void finishPartial();

    [[nodiscard]] size_t bufferSize() const;
    [[nodiscard]] size_t bufferCapacity() const;

private:
    void beginClip(const ClipTrigger& trigger);
    void handOff();
    [[nodiscard]] ClipTrigger currentTrigger(const Frame& frame, bool continuation) const;

    FrameRingBuffer ring_;
    CandidateStarted onCandidate_;
    ClipReady onClip_;
    FlashDetector detector_;
    EventClip clip_;
    bool collecting_{false};

    // Frames since the detector last fired, while a clip is open.
    int quietFrames_{0};
};
