#include "session/blip_injector.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

void validateFrameForBlip(const Frame& frame)
{
    if (frame.width <= 0 || frame.height <= 0 || bytesPerPixel(frame.format) == 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format)
        || frame.significantBits < 1 || frame.significantBits > 16)
    {
        throw std::invalid_argument("invalid frame for test flash injection");
    }
}

void addPixelDelta(uint8_t* pixel, bool mono16, int channels, uint32_t maximum, uint32_t increment)
{
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

void subPixelDelta(uint8_t* pixel, bool mono16, int channels, uint32_t decrement)
{
    if (mono16)
    {
        const uint32_t sample = pixel[0] | (static_cast<uint32_t>(pixel[1]) << 8);
        const uint32_t value = sample > decrement ? sample - decrement : 0u;
        pixel[0] = static_cast<uint8_t>(value & 0xFF);
        pixel[1] = static_cast<uint8_t>(value >> 8);
    }
    else
    {
        for (int channel = 0; channel < channels; ++channel)
        {
            pixel[channel] = static_cast<uint8_t>(pixel[channel] > decrement ? pixel[channel] - decrement : 0u);
        }
    }
}

// Squared distance from point (px, py) to segment (x0, y0)-(x1, y1).
double pointToSegmentDistSq(double px, double py, double x0, double y0, double x1, double y1)
{
    const double sx = x1 - x0;
    const double sy = y1 - y0;
    const double lenSq = sx * sx + sy * sy;
    if (lenSq <= 1e-12)
    {
        const double dx = px - x0;
        const double dy = py - y0;
        return dx * dx + dy * dy;
    }
    const double t = std::clamp(((px - x0) * sx + (py - y0) * sy) / lenSq, 0.0, 1.0);
    const double dx = px - (x0 + t * sx);
    const double dy = py - (y0 + t * sy);
    return dx * dx + dy * dy;
}

// Rounding to the nearest ADU makes the Gaussian exactly zero beyond the
// radius where its amplitude falls below half an ADU. No arbitrary cutoff
// removes a representable part of the signal.
void paintGaussianSpot(Frame& frame, double cx, double cy, double fwhm, double amplitude, bool subtract)
{
    if (amplitude < 0.5)
    {
        return;
    }

    const double fourLogTwo = 4.0 * std::log(2.0);
    const double radius = std::min(static_cast<double>(std::max(frame.width, frame.height)),
        std::ceil(fwhm * std::sqrt((std::log(amplitude) + std::log(2.0)) / fourLogTwo)));
    const int left = static_cast<int>(std::max(0.0, std::floor(cx - radius)));
    const int right = static_cast<int>(std::min(frame.width - 1.0, std::ceil(cx + radius)));
    const int top = static_cast<int>(std::max(0.0, std::floor(cy - radius)));
    const int bottom = static_cast<int>(std::min(frame.height - 1.0, std::ceil(cy + radius)));
    if (left > right || top > bottom) return;

    const bool mono16 = frame.format == PixelFormat::Mono16;
    const int shift = mono16 && frame.alignment == BitAlignment::High ? 16 - frame.significantBits : 0;
    const uint32_t maximum = mono16 ? ((1u << frame.significantBits) - 1u) << shift : 255u;
    const int channels = frame.format == PixelFormat::RGB888 ? 3 : 1;
    const size_t pixelBytes = static_cast<size_t>(bytesPerPixel(frame.format));

    for (int y = top; y <= bottom; ++y)
    {
        const double dy = (y - cy) / fwhm;
        for (int x = left; x <= right; ++x)
        {
            const double dx = (x - cx) / fwhm;
            const uint32_t delta = static_cast<uint32_t>(std::round(
                amplitude * std::exp(-fourLogTwo * (dx * dx + dy * dy)))) << shift;
            if (delta == 0)
            {
                continue;
            }
            uint8_t* pixel = frame.data.data()
                + (static_cast<size_t>(y) * frame.width + x) * pixelBytes;
            if (subtract) subPixelDelta(pixel, mono16, channels, delta);
            else addPixelDelta(pixel, mono16, channels, maximum, delta);
        }
    }
}

void paintStreakSegment(Frame& frame, double x0, double y0, double x1, double y1, double fwhm, double amplitude)
{
    if (amplitude < 0.5) return;

    const double fourLogTwo = 4.0 * std::log(2.0);
    const double radius = std::min(static_cast<double>(std::max(frame.width, frame.height)),
        std::ceil(fwhm * std::sqrt((std::log(amplitude) + std::log(2.0)) / fourLogTwo)));
    const int left = static_cast<int>(std::max(0.0, std::floor(std::min(x0, x1) - radius)));
    const int right = static_cast<int>(std::min(frame.width - 1.0, std::ceil(std::max(x0, x1) + radius)));
    const int top = static_cast<int>(std::max(0.0, std::floor(std::min(y0, y1) - radius)));
    const int bottom = static_cast<int>(std::min(frame.height - 1.0, std::ceil(std::max(y0, y1) + radius)));
    if (left > right || top > bottom) return;

    const bool mono16 = frame.format == PixelFormat::Mono16;
    const int shift = mono16 && frame.alignment == BitAlignment::High ? 16 - frame.significantBits : 0;
    const uint32_t maximum = mono16 ? ((1u << frame.significantBits) - 1u) << shift : 255u;
    const int channels = frame.format == PixelFormat::RGB888 ? 3 : 1;
    const size_t pixelBytes = static_cast<size_t>(bytesPerPixel(frame.format));
    const double invFwhmSq = 1.0 / (fwhm * fwhm);

    for (int y = top; y <= bottom; ++y)
    {
        for (int x = left; x <= right; ++x)
        {
            const double distSq = pointToSegmentDistSq(x, y, x0, y0, x1, y1);
            const uint32_t delta = static_cast<uint32_t>(std::round(
                amplitude * std::exp(-fourLogTwo * distSq * invFwhmSq))) << shift;
            if (delta == 0) continue;
            uint8_t* pixel = frame.data.data()
                + (static_cast<size_t>(y) * frame.width + x) * pixelBytes;
            addPixelDelta(pixel, mono16, channels, maximum, delta);
        }
    }
}

void paintLightning(Frame& frame, double amplitude)
{
    if (amplitude < 0.5) return;
    const bool mono16 = frame.format == PixelFormat::Mono16;
    const int shift = mono16 && frame.alignment == BitAlignment::High ? 16 - frame.significantBits : 0;
    const uint32_t maximum = mono16 ? ((1u << frame.significantBits) - 1u) << shift : 255u;
    const int channels = frame.format == PixelFormat::RGB888 ? 3 : 1;
    const size_t pixelBytes = static_cast<size_t>(bytesPerPixel(frame.format));
    const uint32_t delta = static_cast<uint32_t>(std::round(amplitude)) << shift;
    if (delta == 0) return;

    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    for (size_t i = 0; i < pixels; ++i)
    {
        addPixelDelta(frame.data.data() + i * pixelBytes, mono16, channels, maximum, delta);
    }
}

void applyBlipStep(Frame& frame, const BlipSettings& settings, int step)
{
    validateFrameForBlip(frame);
    if (settings.kind != BlipKind::Lightning && step == 0
        && (settings.x >= frame.width || settings.y >= frame.height))
    {
        throw std::invalid_argument("test flash centre is outside the frame");
    }

    const double cx = settings.x + settings.vx * step;
    const double cy = settings.y + settings.vy * step;
    switch (settings.kind)
    {
    case BlipKind::BrightFlash:
        paintGaussianSpot(frame, cx, cy, settings.fwhm, settings.amplitude, false);
        break;
    case BlipKind::DarkTransit:
        paintGaussianSpot(frame, cx, cy, settings.fwhm, settings.amplitude, true);
        break;
    case BlipKind::MovingStreak:
        paintStreakSegment(frame, cx, cy, cx + settings.vx * 0.85, cy + settings.vy * 0.85,
            settings.fwhm, settings.amplitude);
        break;
    case BlipKind::BlinkingMover:
    {
        // Steady dim navigation light with a bright strobe every 5th frame (>= 2.5x median).
        const bool strobe = (step % 5) == 1;
        const double amp = strobe ? settings.amplitude : settings.amplitude * 0.28;
        paintGaussianSpot(frame, cx, cy, settings.fwhm, amp, false);
        break;
    }
    case BlipKind::Lightning:
        paintLightning(frame, settings.amplitude);
        break;
    }
}

} // namespace

bool BlipInjector::request(const BlipSettings& settings)
{
    if (settings.x < 0 || settings.y < 0 || !std::isfinite(settings.fwhm) || settings.fwhm <= 0.0
        || !std::isfinite(settings.amplitude) || settings.amplitude <= 0.0 || settings.amplitude > 65535.0
        || !std::isfinite(settings.vx) || !std::isfinite(settings.vy)
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

    applyBlipStep(frame, active_->settings, active_->frameNumber);
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
