#pragma once

#include "frame.h"

#include <mutex>
#include <utility>

// A one-slot mailbox for a reader that only wants the newest frame.
// The source thread puts every frame. A frame that was not taken in time is
// replaced, so a slow reader skips frames and never holds the source up.
class LatestFrameSlot
{
public:
    // Stores the frame as the newest. Returns true when the slot was empty,
    // which is when the reader needs to be woken.
    bool put(FramePtr frame)
    {
        // An unread frame is released after the lock is dropped.
        FramePtr replaced;
        std::lock_guard<std::mutex> lock(mutex_);
        replaced = std::exchange(frame_, std::move(frame));
        return replaced == nullptr;
    }

    // Takes the newest frame and empties the slot. Null when nothing is waiting.
    [[nodiscard]] FramePtr take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(frame_, nullptr);
    }

private:
    std::mutex mutex_;
    FramePtr frame_;
};
