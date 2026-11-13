#include "blip_injector.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

// Rounding to the nearest ADU makes the Gaussian exactly zero beyond the
// radius where its amplitude falls below half an ADU. No arbitrary cutoff
// removes a representable part of the signal.
void addGaussian(Frame& frame, const BlipSettings& settings)
{
    if (frame.width <= 0 || frame.height <= 0 || bytesPerPixel(frame.format) == 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format)
        || frame.significantBits < 1 || frame.significantBits > 16)
    {
        throw std::invalid_argument("invalid frame for test flash injection");
    }
    if (settings.x >= frame.width || settings.y >= frame.height)
    {
        throw std::invalid_argument("test flash centre is outside the frame");
    }
    if (settings.amplitude < 0.5)
    {
        return;
    }

    const double fourLogTwo = 4.0 * std::log(2.0);
    const double radius = std::min(static_cast<double>(std::max(frame.width, frame.height)),
        std::ceil(settings.fwhm * std::sqrt((std::log(settings.amplitude) + std::log(2.0)) / fourLogTwo)));
    const int left = static_cast<int>(std::max(0.0, settings.x - radius));
    const int right = static_cast<int>(std::min(frame.width - 1.0, settings.x + radius));
    const int top = static_cast<int>(std::max(0.0, settings.y - radius));
    const int bottom = static_cast<int>(std::min(frame.height - 1.0, settings.y + radius));

    const bool mono16 = frame.format == PixelFormat::Mono16;
    const int shift = mono16 && frame.alignment == BitAlignment::High ? 16 - frame.significantBits : 0;
    const uint32_t maximum = mono16 ? ((1u << frame.significantBits) - 1u) << shift : 255u;
    const int channels = frame.format == PixelFormat::RGB888 ? 3 : 1;
    const size_t pixelBytes = static_cast<size_t>(bytesPerPixel(frame.format));

    for (int y = top; y <= bottom; ++y)
    {
        const double dy = (y - settings.y) / settings.fwhm;
        for (int x = left; x <= right; ++x)
        {
            const double dx = (x - settings.x) / settings.fwhm;
            const uint32_t increment = static_cast<uint32_t>(std::round(
                settings.amplitude * std::exp(-fourLogTwo * (dx * dx + dy * dy)))) << shift;
            if (increment == 0)
            {
                continue;
            }
            uint8_t* pixel = frame.data.data()
                + (static_cast<size_t>(y) * frame.width + x) * pixelBytes;
            if (mono16)
            {
                const uint32_t sample = pixel[0] | (static_cast<uint32_t>(pixel[1]) << 8);
                // Never dim a sample, including one whose source declared
                // fewer significant bits than it actually uses.
                const uint32_t value = sample >= maximum ? sample : std::min(sample + increment, maximum);
                pixel[0] = static_cast<uint8_t>(value & 0xFF);
                pixel[1] = static_cast<uint8_t>(value >> 8);
            }
            else
            {
                // RGB receives a neutral flash: the same addition to each
                // channel, with each channel saturating independently.
                for (int channel = 0; channel < channels; ++channel)
                {
                    pixel[channel] = static_cast<uint8_t>(std::min(pixel[channel] + increment, 255u));
                }
            }
        }
    }
}

} // namespace

bool BlipInjector::request(const BlipSettings& settings)
{
    if (settings.x < 0 || settings.y < 0 || !std::isfinite(settings.fwhm) || settings.fwhm <= 0.0
        || !std::isfinite(settings.amplitude) || settings.amplitude <= 0.0 || settings.amplitude > 65535.0
        || settings.frames <= 0)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_)
    {
        return false;
    }
    requested_ = settings;
    busy_ = true;
    return true;
}

bool BlipInjector::busy() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

void BlipInjector::apply(Frame& frame)
{
    frame.blip.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requested_)
        {
            active_ = BlipMark{*requested_, frame.frameIndex, 0};
            requested_.reset();
        }
    }
    if (!active_)
    {
        return;
    }

    addGaussian(frame, active_->settings);
    ++active_->frameNumber;
    frame.blip = active_;
    if (active_->frameNumber == active_->settings.frames)
    {
        active_.reset();
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
    }
}

void BlipInjector::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    requested_.reset();
    active_.reset();
    busy_ = false;
}
