#include "flash_detector.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

// A derived scalar view, never a conversion of the stored evidence.
struct Samples
{
    const Frame& frame;
    int shift;
    float at(size_t i) const
    {
        const uint8_t* data = frame.data.data();
        switch (frame.format)
        {
        case PixelFormat::Mono8: return data[i];
        case PixelFormat::Mono16: return static_cast<float>((data[2 * i] | (data[2 * i + 1] << 8)) >> shift);
        case PixelFormat::RGB888: return (data[3 * i] + data[3 * i + 1] + data[3 * i + 2]) / 3.0f;
        }
        return 0;
    }
};

float median(std::vector<float>& values)
{
    const auto middle = values.begin() + static_cast<ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

} // namespace

FlashDetector::FlashDetector(DetectionSettings settings)
{
    setSettings(settings);
}

void FlashDetector::setSettings(const DetectionSettings& settings)
{
    if (!settings.valid()) throw std::invalid_argument("invalid detection settings");
    // A pending two-frame confirmation must use one set of settings. Keep
    // an already-triggered run armed until quiet, so an edit cannot duplicate it.
    if (settings != settings_ && !runTriggered_)
    {
        consecutive_ = 0;
        onsetFrameIndex_ = 0;
    }
    settings_ = settings;
}

void FlashDetector::reset()
{
    mean_.clear(); variance_.clear(); grid_.clear(); history_.clear(); map_.reset();
    thresholdPixels_.clear(); thresholdSigma_.clear(); visited_.clear(); pending_.clear();
    learnedFrames_ = 0;
    width_ = height_ = bits_ = 0;
    consecutive_ = 0;
    runTriggered_ = false;
    onsetFrameIndex_ = 0;
    previousTime_ = {};
    summary_ = {};
}

bool FlashDetector::observe(const Frame& frame, bool holdReference, DetectionDisplayRequest display)
{
    const auto started = std::chrono::steady_clock::now();
    if (frame.width <= 0 || frame.height <= 0 || bytesPerPixel(frame.format) == 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format)
        || frame.significantBits < 1 || frame.significantBits > 16)
        throw std::invalid_argument("frame data does not match its size and pixel layout");
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    const Samples samples{frame, frame.format == PixelFormat::Mono16 && frame.alignment == BitAlignment::High
        ? 16 - frame.significantBits : 0};
    summary_ = {};
    summary_.settings = settings_;
    summary_.frameIndex = frame.frameIndex;
    summary_.time = frame.timestamp;
    summary_.probeX = display.probeX; summary_.probeY = display.probeY;
    map_.reset();

    if (width_ != frame.width || height_ != frame.height || format_ != frame.format
        || bits_ != frame.significantBits || alignment_ != frame.alignment || bayer_ != frame.bayer)
    {
        width_ = frame.width; height_ = frame.height; format_ = frame.format;
        bits_ = frame.significantBits; alignment_ = frame.alignment; bayer_ = frame.bayer;
        mean_.resize(pixels);
        variance_.assign(pixels, 0);
        learnedFrames_ = 1;
        for (size_t i = 0; i < pixels; ++i) mean_[i] = samples.at(i);
        consecutive_ = 0; runTriggered_ = false; onsetFrameIndex_ = 0;
        history_.clear();
    }
    else
    {
        summary_.ready = true;
        // Median absolute deviation on a bounded grid. Removing the median
        // difference keeps a uniform brightness step out of the noise floor.
        grid_.clear();
        grid_.reserve(4096);
        const size_t step = std::max(size_t{1}, (pixels + 4095) / 4096);
        for (size_t i = 0; i < pixels; i += step) grid_.push_back(samples.at(i) - mean_[i]);
        const float centre = median(grid_);
        for (float& value : grid_) value = std::abs(value - centre);
        summary_.noiseFloor = std::max(0.5, 1.4826 * median(grid_));

        int left = width_, top = height_, right = -1, bottom = -1;
        thresholdPixels_.clear(); thresholdSigma_.clear();
        const size_t sparseLimit = std::min(pixels, std::max(size_t{4096}, static_cast<size_t>(settings_.maxPixels) * 4));
        const double floorVariance = summary_.noiseFloor * summary_.noiseFloor;
        std::shared_ptr<DetectionMap> map;
        if (display.makeMap && display.overlay != DetectionOverlay::None)
        {
            map = std::make_shared<DetectionMap>();
            map->width = width_; map->height = height_; map->kind = display.overlay;
            map->values.resize(pixels);
        }
        for (int y = 0; y < height_; ++y)
        {
            for (int x = 0; x < width_; ++x)
            {
                const size_t i = static_cast<size_t>(y) * width_ + x;
                const double noise = std::sqrt(std::max(static_cast<double>(variance_[i]), floorVariance));
                const double delta = samples.at(i) - mean_[i];
                const double sigma = delta / noise;
                if (map) map->values[i] = static_cast<float>(display.overlay == DetectionOverlay::Noise ? noise : std::max(0.0, sigma));
                if (sigma > summary_.peakSigma)
                {
                    summary_.peakSigma = sigma; summary_.peakAdu = delta;
                    summary_.peakX = x; summary_.peakY = y;
                }
                if (x == display.probeX && y == display.probeY) summary_.probeNoise = noise;
                if (sigma >= settings_.sigma)
                {
                    ++summary_.brightPixels;
                    if (thresholdPixels_.size() < sparseLimit)
                    {
                        thresholdPixels_.push_back(i);
                        thresholdSigma_.push_back(static_cast<float>(sigma));
                    }
                    left = std::min(left, x); top = std::min(top, y);
                    right = std::max(right, x); bottom = std::max(bottom, y);
                }
            }
        }
        if (summary_.brightPixels > 0)
        {
            summary_.boxX = left; summary_.boxY = top;
            summary_.boxWidth = right - left + 1; summary_.boxHeight = bottom - top + 1;
        }
        const double boxArea = static_cast<double>(summary_.boxWidth) * summary_.boxHeight;
        summary_.passed = summary_.brightPixels >= settings_.minPixels && summary_.brightPixels <= settings_.maxPixels
            && boxArea <= kMaxPatchBoxRatio * summary_.brightPixels;

        // A global bounding box lets an unrelated hot pixel veto a real
        // flash. Split the sparse threshold pixels into 8-connected patches.
        // Dense changes are rejected before this work; its storage/work is
        // bounded by max(4096, four times the observer's maximum patch size).
        if (static_cast<size_t>(summary_.brightPixels) <= sparseLimit && !thresholdPixels_.empty())
        {
            visited_.assign(thresholdPixels_.size(), 0);
            bool foundPassing = false;
            double bestPeak = -1;
            DetectionSummary best = summary_;
            for (size_t seed = 0; seed < thresholdPixels_.size(); ++seed)
            {
                if (visited_[seed]) continue;
                pending_.clear(); pending_.push_back(seed); visited_[seed] = 1;
                int count = 0, x0 = width_, y0 = height_, x1 = -1, y1 = -1;
                double peak = 0; size_t peakAt = 0;
                while (!pending_.empty())
                {
                    const size_t item = pending_.back(); pending_.pop_back();
                    const size_t pixel = thresholdPixels_[item];
                    const int x = static_cast<int>(pixel % static_cast<size_t>(width_));
                    const int y = static_cast<int>(pixel / static_cast<size_t>(width_));
                    ++count;
                    x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                    if (thresholdSigma_[item] > peak) { peak = thresholdSigma_[item]; peakAt = pixel; }
                    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                    {
                        if ((!dx && !dy) || x + dx < 0 || x + dx >= width_ || y + dy < 0 || y + dy >= height_) continue;
                        const size_t neighbor = static_cast<size_t>(y + dy) * width_ + x + dx;
                        const auto at = std::lower_bound(thresholdPixels_.begin(), thresholdPixels_.end(), neighbor);
                        if (at == thresholdPixels_.end() || *at != neighbor) continue;
                        const size_t pos = static_cast<size_t>(at - thresholdPixels_.begin());
                        if (!visited_[pos]) { visited_[pos] = 1; pending_.push_back(pos); }
                    }
                }
                const bool passes = count >= settings_.minPixels && count <= settings_.maxPixels
                    && static_cast<double>(x1 - x0 + 1) * (y1 - y0 + 1) <= kMaxPatchBoxRatio * count;
                if ((passes && !foundPassing) || (passes == foundPassing && peak > bestPeak))
                {
                    foundPassing = passes; bestPeak = peak;
                    best.brightPixels = count; best.boxX = x0; best.boxY = y0;
                    best.boxWidth = x1 - x0 + 1; best.boxHeight = y1 - y0 + 1;
                    best.peakSigma = peak;
                    best.peakX = static_cast<int>(peakAt % static_cast<size_t>(width_));
                    best.peakY = static_cast<int>(peakAt / static_cast<size_t>(width_));
                    best.peakAdu = samples.at(peakAt) - mean_[peakAt];
                }
            }
            best.passed = foundPassing;
            summary_ = best;
        }
        if (summary_.passed)
        {
            if (consecutive_ == 0) onsetFrameIndex_ = frame.frameIndex;
            consecutive_ = std::min(consecutive_ + 1, 2);
            summary_.triggered = !runTriggered_ && (settings_.oneFrame || consecutive_ >= 2);
            runTriggered_ = runTriggered_ || summary_.triggered;
            summary_.firing = runTriggered_;
        }
        else
        {
            consecutive_ = 0; runTriggered_ = false; onsetFrameIndex_ = 0;
        }

        map_ = std::move(map);

        // Two passes are intentional: a global patch decision must be known
        // before any reference pixel can be changed. No rollback buffer is needed.
        if (!holdReference && !summary_.passed)
        {
            double dt = std::chrono::duration<double>(frame.timestamp - previousTime_).count();
            if (!(dt > 0) || !std::isfinite(dt)) dt = 1.0 / 30.0;
            // Cumulative learning until the two-second exponential rate is
            // reached. A cold variance of zero must not take two seconds to
            // learn ordinary sample-to-sample fluctuations.
            const float alpha = static_cast<float>(std::max(1.0 / static_cast<double>(learnedFrames_ + 1),
                -std::expm1(-std::min(dt, 10.0) / kReferenceTimeConstantSeconds)));
            for (size_t i = 0; i < pixels; ++i)
            {
                const float delta = samples.at(i) - mean_[i];
                mean_[i] += alpha * delta;
                variance_[i] = (1 - alpha) * (variance_[i] + alpha * delta * delta);
            }
            if (learnedFrames_ < 1'000'000) ++learnedFrames_;
        }
    }
    previousTime_ = frame.timestamp;
    history_.push_back({frame.timestamp, summary_.peakSigma, summary_.brightPixels});
    while (!history_.empty() && (history_.size() > 4096
        || frame.timestamp - history_.front().time > std::chrono::seconds{10})) history_.pop_front();
    summary_.processingMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return summary_.triggered;
}

bool FlashDetector::firing() const { return summary_.firing; }
int FlashDetector::lastBrightCount() const { return summary_.brightPixels; }
uint64_t FlashDetector::onsetFrameIndex() const { return onsetFrameIndex_; }
const DetectionSummary& FlashDetector::summary() const { return summary_; }
DetectionView FlashDetector::view() const
{
    return {summary_, {history_.begin(), history_.end()}, map_};
}

std::vector<float> FlashDetector::difference(const Frame& frame) const
{
    if (frame.width != width_ || frame.height != height_ || frame.format != format_
        || frame.data.size() != frameBytes(width_, height_, format_))
        throw std::invalid_argument("difference requested for another frame layout");
    const Samples samples{frame, format_ == PixelFormat::Mono16 && alignment_ == BitAlignment::High ? 16 - bits_ : 0};
    std::vector<float> values(mean_.size());
    for (size_t i = 0; i < values.size(); ++i) values[i] = samples.at(i) - mean_[i];
    return values;
}
