#pragma once

#include "frame.h"
#include "detection_result.h"

#include <mutex>
#include <utility>

// A preview frame and the measurements made on that exact frame.
struct PreviewFrame
{
    FramePtr frame;
    std::shared_ptr<const DetectionView> detection;
};

// A one-slot mailbox for a reader that only wants the newest preview. An
// unread packet is replaced, so a slow reader never holds the source up.
class LatestFrameSlot
{
public:
    // Stores the frame as the newest. Returns true when the slot was empty,
    // which is when the reader needs to be woken.
    bool put(FramePtr frame, std::shared_ptr<const DetectionView> detection = {})
    {
        // An unread frame is released after the lock is dropped.
        PreviewFrame replaced;
        std::lock_guard<std::mutex> lock(mutex_);
        replaced = std::exchange(frame_, PreviewFrame{std::move(frame), std::move(detection)});
        return replaced.frame == nullptr;
    }

    // Takes the newest frame and empties the slot. Null when nothing is waiting.
    [[nodiscard]] PreviewFrame take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(frame_, {});
    }

private:
    std::mutex mutex_;
    PreviewFrame frame_;
};
