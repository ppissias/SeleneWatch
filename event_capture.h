#pragma once

#include "event_clip.h"
#include "flash_detector.h"
#include "frame.h"
#include "frame_ring_buffer.h"

#include <atomic>
#include <chrono>
#include <functional>

// Collects one candidate clip from the live frames.
//
// Owns the ring, the flash detector, and the clip in progress. The window
// keeps the preview, the counters, and the buttons.
//
// The source thread calls onFrame. The window thread calls reset,
// finishPartial, and onWriteFinished only while the source thread is joined
// or has not started. The buffer queries take the ring lock and may be
// called from the window timer during streaming.
class EventCapture
{
public:
    using CandidateStarted = std::function<void()>;
    using ClipReady = std::function<void(EventClip clip)>;

    EventCapture(size_t capacity, CandidateStarted onCandidate, ClipReady onClip);

    // Drops the ring, the detector, and any clip still being collected.
    // An in-flight write is left alone.
    void reset();

    // Source thread. May call the candidate and clip callbacks before returning.
    void onFrame(Frame& frame);

    // Hands off a short clip when Stop interrupts collection.
    void finishPartial();

    // Writer finished one clip. Returns how many writes are still in flight.
    int onWriteFinished();

    [[nodiscard]] int writesInFlight() const;

    [[nodiscard]] size_t bufferSize() const;
    [[nodiscard]] size_t bufferCapacity() const;
    [[nodiscard]] size_t memoryUsageBytes() const;

private:
    void beginCapture(uint64_t onsetIndex, uint64_t triggerIndex,
        std::chrono::system_clock::time_point triggerTime, int brightPixelCount);
    void appendPostFrame(const Frame& frame);
    void handOff();

    FrameRingBuffer ring_;
    CandidateStarted onCandidate_;
    ClipReady onClip_;
    FlashDetector detector_;
    EventClip clip_;
    int postRemaining_{0};
    bool collecting_{false};
    std::atomic<int> writesInFlight_{0};
};
