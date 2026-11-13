#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

enum class PixelFormat
{
    Mono8,
    RGB888
};

// Represents a single image frame acquired from a camera or test source.
struct Frame
{
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::Mono8;
    uint64_t frameIndex = 0;

    std::chrono::system_clock::time_point timestamp{};
    std::chrono::steady_clock::time_point steadyTimestamp{};

    std::vector<uint8_t> data;
};

// A published frame. A source fills a frame while it is the only owner.
// Once the frame is held as a FramePtr, nobody writes it again, so the
// ring, the clips, and the preview can share it without copying pixels.
using FramePtr = std::shared_ptr<const Frame>;
