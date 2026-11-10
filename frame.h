#pragma once

#include <cstdint>
#include <vector>
#include <chrono>
#include <QtGui/QImage>

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

    // Converts the frame's raw pixel buffer to a QImage for UI preview.
    [[nodiscard]] QImage toQImage() const
    {
        if (width <= 0 || height <= 0 || data.empty())
        {
            return {};
        }

        switch (format)
        {
        case PixelFormat::Mono8:
            return QImage(data.data(), width, height, width, QImage::Format_Grayscale8).copy();

        case PixelFormat::RGB888:
            return QImage(data.data(), width, height, width * 3, QImage::Format_RGB888).copy();
        }

        return {};
    }
};
