#include "sky_exposure.h"

#include <algorithm>
#include <format>
#include <vector>

std::optional<SkyExposureChange> decideSkyExposure(double level, double exposureUs, double gain,
    std::chrono::steady_clock::time_point now, SkyExposureState& state, const SkyExposureRule& rule)
{
    if (level >= rule.low && level <= rule.high)
    {
        state.outsideSince.reset();
        return std::nullopt;
    }
    if (!state.outsideSince)
    {
        state.outsideSince = now;
        return std::nullopt;
    }
    if (now - *state.outsideSince < rule.hold) return std::nullopt;
    if (state.lastChange && now - *state.lastChange < rule.spacing) return std::nullopt;

    const double ratio = std::clamp(rule.aim / std::max(level, 1e-6), 1.0 / rule.maxFactor, rule.maxFactor);
    SkyExposureChange change{exposureUs, gain, {}};
    const std::string measured = std::format("the sky at {:.1f}% of full scale, outside {:.0f}% to {:.0f}%",
        100 * level, 100 * rule.low, 100 * rule.high);
    if (level < rule.low)
    {
        // Darker: a longer exposure first, then more gain.
        if (exposureUs < rule.maxExposureUs)
            change.exposureUs = std::min(rule.maxExposureUs, exposureUs * ratio);
        else if (rule.gainStep > 0 && gain < rule.maxGain)
            change.gain = std::min(rule.maxGain, gain + rule.gainStep);
        else return std::nullopt;
    }
    else
    {
        // Brighter: less gain first, then a shorter exposure.
        if (rule.gainStep > 0 && gain > rule.minGain)
            change.gain = std::max(rule.minGain, gain - rule.gainStep);
        else if (exposureUs > rule.minExposureUs)
            change.exposureUs = std::max(rule.minExposureUs, exposureUs * ratio);
        else return std::nullopt;
    }
    change.reason = measured;
    state.lastChange = now;
    state.outsideSince.reset();
    return change;
}

std::optional<double> skyLevel(const Frame& frame, const DetectionMask& mask)
{
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    if (pixels == 0 || frame.data.size() != frameBytes(frame.width, frame.height, frame.format)) return std::nullopt;
    std::vector<uint8_t> masked;
    if (!mask.empty())
    {
        masked.assign(pixels, 0);
        mask.fill(frame.width, frame.height, masked.data());
    }
    const int shift = frame.format == PixelFormat::Mono16 && frame.alignment == BitAlignment::High ? 16 - frame.significantBits : 0;
    const double fullScale = frame.format == PixelFormat::Mono16 ? (1 << frame.significantBits) - 1 : 255;
    const size_t step = std::max(size_t{1}, pixels / 8192);
    std::vector<double> samples;
    samples.reserve(8192);
    for (size_t i = 0; i < pixels; i += step)
    {
        if (!masked.empty() && masked[i]) continue;
        const uint8_t* data = frame.data.data();
        switch (frame.format)
        {
        case PixelFormat::Mono8: samples.push_back(data[i]); break;
        case PixelFormat::Mono16: samples.push_back(((data[2 * i] | (data[2 * i + 1] << 8)) >> shift)); break;
        case PixelFormat::RGB888: samples.push_back((data[3 * i] + data[3 * i + 1] + data[3 * i + 2]) / 3.0); break;
        }
    }
    if (samples.empty()) return std::nullopt;
    const auto middle = samples.begin() + static_cast<ptrdiff_t>(samples.size() / 2);
    std::nth_element(samples.begin(), middle, samples.end());
    return *middle / fullScale;
}
