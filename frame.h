#pragma once

#include <cstdint>
#include <vector>
#include <chrono>

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
