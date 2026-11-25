#include "detection/flash_detector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace {

// A derived scalar view of a frame at an optional shift (Feature-50), never
// modifying the stored evidence. Raw Bayer mosaics step by 2 pixels so
// interpolation stays strictly within each colour sub-plane.
struct ShiftedView
{
    const uint8_t* data{nullptr};
    int width{0};
    int height{0};
    PixelFormat format{PixelFormat::Mono8};
    int bitShift{0};
    ShiftMode mode{ShiftMode::Off};
    int step{1};
    int ox{0};
    int oy{0};
    int dxStep{0};
    int dyStep{0};
    float w00{1.0f};
    float w10{0.0f};
    float w01{0.0f};
    float w11{0.0f};
    int rxMin{0};
    int rxMax{0};
    int ryMin{0};
    int ryMax{0};
    int nativeDx{0};
    int nativeDy{0};

    ShiftedView(const Frame& frame, FrameShift shift)
        : data(frame.data.data()), width(frame.width), height(frame.height), format(frame.format),
          bitShift(frame.format == PixelFormat::Mono16 && frame.alignment == BitAlignment::High
              ? 16 - frame.significantBits : 0)
    {
        if (shift.mode == ShiftMode::Off || !std::isfinite(shift.dx) || !std::isfinite(shift.dy))
        {
            rxMax = width;
            ryMax = height;
            return;
        }
        const bool bayer = frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
        step = bayer ? 2 : 1;
        if (shift.mode == ShiftMode::Integer)
        {
            mode = ShiftMode::Integer;
            ox = bayer ? 2 * static_cast<int>(std::lround(shift.dx / 2.0)) : static_cast<int>(std::lround(shift.dx));
            oy = bayer ? 2 * static_cast<int>(std::lround(shift.dy / 2.0)) : static_cast<int>(std::lround(shift.dy));
            nativeDx = ox;
            nativeDy = oy;
            rxMin = std::clamp(-ox, 0, width);
            rxMax = std::clamp(width - ox, 0, width);
            ryMin = std::clamp(-oy, 0, height);
            ryMax = std::clamp(height - oy, 0, height);
            return;
        }

        const double ux = shift.dx / step;
        const double uy = shift.dy / step;
        int kx = static_cast<int>(std::floor(ux));
        int ky = static_cast<int>(std::floor(uy));
        double wx = ux - kx;
        double wy = uy - ky;
        if (wx < 1e-6) wx = 0.0;
        else if (wx > 1.0 - 1e-6) { wx = 0.0; ++kx; }
        if (wy < 1e-6) wy = 0.0;
        else if (wy > 1.0 - 1e-6) { wy = 0.0; ++ky; }

        ox = kx * step;
        oy = ky * step;
        nativeDx = static_cast<int>(std::lround(shift.dx));
        nativeDy = static_cast<int>(std::lround(shift.dy));
        if (wx == 0.0 && wy == 0.0)
        {
            mode = ShiftMode::Integer;
            rxMin = std::clamp(-ox, 0, width);
            rxMax = std::clamp(width - ox, 0, width);
            ryMin = std::clamp(-oy, 0, height);
            ryMax = std::clamp(height - oy, 0, height);
            return;
        }

        mode = ShiftMode::Subpixel;
        dxStep = wx > 0.0 ? step : 0;
        dyStep = wy > 0.0 ? step : 0;
        w00 = static_cast<float>((1.0 - wx) * (1.0 - wy));
        w10 = static_cast<float>(wx * (1.0 - wy));
        w01 = static_cast<float>((1.0 - wx) * wy);
        w11 = static_cast<float>(wx * wy);
        rxMin = std::clamp(-ox, 0, width);
        rxMax = std::clamp(width - ox - dxStep, 0, width);
        ryMin = std::clamp(-oy, 0, height);
        ryMax = std::clamp(height - oy - dyStep, 0, height);
    }

    [[nodiscard]] bool valid(int rx, int ry) const
    {
        return rx >= rxMin && rx < rxMax && ry >= ryMin && ry < ryMax;
    }

    [[nodiscard]] float rawAt(size_t i) const
    {
        switch (format)
        {
        case PixelFormat::Mono8: return data[i];
        case PixelFormat::Mono16:
        {
            uint16_t v = 0;
            std::memcpy(&v, data + 2 * i, sizeof(v));
            return static_cast<float>(v >> bitShift);
        }
        case PixelFormat::RGB888:
            return (data[3 * i] + data[3 * i + 1] + data[3 * i + 2]) / 3.0f;
        }
        return 0.0f;
    }

    [[nodiscard]] float sampleOne(int rx, int ry) const
    {
        const size_t base = static_cast<size_t>(ry + oy) * static_cast<size_t>(width) + static_cast<size_t>(rx + ox);
        if (mode != ShiftMode::Subpixel) return rawAt(base);
        const size_t row1 = base + static_cast<size_t>(dyStep) * static_cast<size_t>(width);
        return w00 * rawAt(base) + w10 * rawAt(base + dxStep)
             + w01 * rawAt(row1) + w11 * rawAt(row1 + dxStep);
    }

    // Fills dst[rxMin..rxMax) for reference row ry without per-pixel format branching.
    void fillRow(int ry, float* dst) const
    {
        if (rxMin >= rxMax) return;
        const size_t row0 = static_cast<size_t>(ry + oy) * static_cast<size_t>(width) + static_cast<size_t>(ox);
        if (mode != ShiftMode::Subpixel)
        {
            switch (format)
            {
            case PixelFormat::Mono8:
            {
                const uint8_t* src = data + row0;
                for (int rx = rxMin; rx < rxMax; ++rx) dst[rx] = static_cast<float>(src[rx]);
                break;
            }
            case PixelFormat::Mono16:
            {
                const uint8_t* src = data + 2 * row0;
                if (bitShift == 0)
                {
                    for (int rx = rxMin; rx < rxMax; ++rx)
                    {
                        uint16_t v = 0;
                        std::memcpy(&v, src + 2 * static_cast<size_t>(rx), sizeof(v));
                        dst[rx] = static_cast<float>(v);
                    }
                }
                else
                {
                    for (int rx = rxMin; rx < rxMax; ++rx)
                    {
                        uint16_t v = 0;
                        std::memcpy(&v, src + 2 * static_cast<size_t>(rx), sizeof(v));
                        dst[rx] = static_cast<float>(v >> bitShift);
                    }
                }
                break;
            }
            case PixelFormat::RGB888:
            {
                const uint8_t* src = data + 3 * row0;
                for (int rx = rxMin; rx < rxMax; ++rx)
                {
                    const size_t p = 3 * static_cast<size_t>(rx);
                    dst[rx] = (src[p] + src[p + 1] + src[p + 2]) / 3.0f;
                }
                break;
            }
            }
            return;
        }

        const size_t row1 = static_cast<size_t>(ry + oy + dyStep) * static_cast<size_t>(width) + static_cast<size_t>(ox);
        switch (format)
        {
        case PixelFormat::Mono8:
        {
            const uint8_t* r0 = data + row0;
            const uint8_t* r1 = data + row1;
            const int dxs = dxStep;
            for (int rx = rxMin; rx < rxMax; ++rx)
                dst[rx] = w00 * r0[rx] + w10 * r0[rx + dxs] + w01 * r1[rx] + w11 * r1[rx + dxs];
            break;
        }
        case PixelFormat::Mono16:
        {
            const uint8_t* r0 = data + 2 * row0;
            const uint8_t* r1 = data + 2 * row1;
            const size_t dxBytes = 2 * static_cast<size_t>(dxStep);
            if (bitShift == 0)
            {
                for (int rx = rxMin; rx < rxMax; ++rx)
                {
                    const size_t b = 2 * static_cast<size_t>(rx);
                    uint16_t v00 = 0, v10 = 0, v01 = 0, v11 = 0;
                    std::memcpy(&v00, r0 + b, 2);
                    std::memcpy(&v10, r0 + b + dxBytes, 2);
                    std::memcpy(&v01, r1 + b, 2);
                    std::memcpy(&v11, r1 + b + dxBytes, 2);
                    dst[rx] = w00 * v00 + w10 * v10 + w01 * v01 + w11 * v11;
                }
            }
            else
            {
                for (int rx = rxMin; rx < rxMax; ++rx)
                {
                    const size_t b = 2 * static_cast<size_t>(rx);
                    uint16_t v00 = 0, v10 = 0, v01 = 0, v11 = 0;
                    std::memcpy(&v00, r0 + b, 2);
                    std::memcpy(&v10, r0 + b + dxBytes, 2);
                    std::memcpy(&v01, r1 + b, 2);
                    std::memcpy(&v11, r1 + b + dxBytes, 2);
                    dst[rx] = w00 * (v00 >> bitShift) + w10 * (v10 >> bitShift)
                            + w01 * (v01 >> bitShift) + w11 * (v11 >> bitShift);
                }
            }
            break;
        }
        case PixelFormat::RGB888:
        {
            const uint8_t* r0 = data + 3 * row0;
            const uint8_t* r1 = data + 3 * row1;
            const size_t dxBytes = 3 * static_cast<size_t>(dxStep);
            constexpr float inv3 = 1.0f / 3.0f;
            for (int rx = rxMin; rx < rxMax; ++rx)
            {
                const size_t b = 3 * static_cast<size_t>(rx);
                const float v00 = (r0[b] + r0[b + 1] + r0[b + 2]) * inv3;
                const float v10 = (r0[b + dxBytes] + r0[b + dxBytes + 1] + r0[b + dxBytes + 2]) * inv3;
                const float v01 = (r1[b] + r1[b + 1] + r1[b + 2]) * inv3;
                const float v11 = (r1[b + dxBytes] + r1[b + dxBytes + 1] + r1[b + dxBytes + 2]) * inv3;
                dst[rx] = w00 * v00 + w10 * v10 + w01 * v01 + w11 * v11;
            }
            break;
        }
        }
    }
};

float median(BudgetVector<float>& values)
{
    const auto middle = values.begin() + static_cast<ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

} // namespace

FlashDetector::FlashDetector(DetectionSettings settings, MemoryBudget budget)
    : budget_(std::move(budget)), mean_(BudgetAllocator<float>(budget_)), variance_(BudgetAllocator<float>(budget_)),
      grid_(BudgetAllocator<float>(budget_)), thresholdPixels_(BudgetAllocator<size_t>(budget_)),
      thresholdSigma_(BudgetAllocator<float>(budget_)), visited_(BudgetAllocator<uint8_t>(budget_)),
      pending_(BudgetAllocator<size_t>(budget_)), protected_(BudgetAllocator<uint8_t>(budget_)),
      masked_(BudgetAllocator<uint8_t>(budget_))
{
    setSettings(settings);
}

void FlashDetector::setSettings(const DetectionSettings& settings)
{
    if (!settings.valid()) throw std::invalid_argument("invalid detection settings");
    // Pending confirmation cannot combine different settings. Already firing
    // patches remain firing until absent, so an edit cannot duplicate them.
    // Feature-45: a patch of the other direction does not continue at all.
    if (settings.darkening != settings_.darkening)
    {
        active_.clear();
        largeChange_ = {};
    }
    else if (settings != settings_)
        std::erase_if(active_, [](const Patch& patch) { return !patch.measured.firing; });
    settings_ = settings;
}

void FlashDetector::rearm()
{
    active_.clear();
    candidates_.clear();
}

void FlashDetector::reset(bool releaseBuffers)
{
    if (releaseBuffers)
    {
        mean_ = BudgetVector<float>(mean_.get_allocator());
        variance_ = BudgetVector<float>(variance_.get_allocator());
        grid_ = BudgetVector<float>(grid_.get_allocator());
        thresholdPixels_ = BudgetVector<size_t>(thresholdPixels_.get_allocator());
        thresholdSigma_ = BudgetVector<float>(thresholdSigma_.get_allocator());
        visited_ = BudgetVector<uint8_t>(visited_.get_allocator());
        pending_ = BudgetVector<size_t>(pending_.get_allocator());
        protected_ = BudgetVector<uint8_t>(protected_.get_allocator());
        masked_ = BudgetVector<uint8_t>(masked_.get_allocator());
        rowBuf_.clear();
        rowBuf_.shrink_to_fit();
    }
    maskBuiltFor_ = {}; maskWidth_ = maskHeight_ = 0; unmaskedPixels_ = 0;
    largeChange_ = {}; holdAll_ = false;
    active_.clear(); candidates_.clear(); awaitingUpdate_ = false;
    lastShift_ = {};
    history_.clear(); map_.reset();
    learnedFrames_ = 0;
    learnedSeconds_ = 0;
    firstFrame_ = false;
    width_ = height_ = bits_ = 0;
    previousTime_ = {};
    summary_ = {};
    strongest_ = {};
}

bool FlashDetector::analyze(const Frame& frame, DetectionDisplayRequest display, FrameShift shift)
{
    if (awaitingUpdate_) throw std::logic_error("update the detector reference before analyzing another frame");
    const auto started = std::chrono::steady_clock::now();
    if (frame.width <= 0 || frame.height <= 0 || bytesPerPixel(frame.format) == 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format)
        || frame.significantBits < 1 || frame.significantBits > 16)
        throw std::invalid_argument("frame data does not match its size and pixel layout");
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    const ShiftedView view(frame, shift);
    lastShift_ = shift;
    rowBuf_.resize(static_cast<size_t>(frame.width));

    summary_ = {};
    strongest_ = {};
    summary_.settings = settings_;
    summary_.frameIndex = frame.frameIndex;
    summary_.time = frame.timestamp;
    summary_.probeX = display.probeX; summary_.probeY = display.probeY;
    summary_.stabilizationState = shift.state;
    summary_.stabilizationMethod = shift.method;
    summary_.stabilizationSource = shift.source;
    summary_.stabilizationDx = shift.dx;
    summary_.stabilizationDy = shift.dy;
    summary_.stabilizationMs = shift.estimatorMs;
    map_.reset();
    candidates_.clear();
    protected_.assign(pixels, 0);
    frameInterval_ = std::chrono::duration<double>(frame.timestamp - previousTime_).count();
    if (!(frameInterval_ > 0) || !std::isfinite(frameInterval_)) frameInterval_ = 1.0/30;
    frameInterval_ = std::min(frameInterval_, 10.0);

    if (width_ != frame.width || height_ != frame.height || format_ != frame.format
        || bits_ != frame.significantBits || alignment_ != frame.alignment || bayer_ != frame.bayer)
    {
        width_ = frame.width; height_ = frame.height; format_ = frame.format;
        bits_ = frame.significantBits; alignment_ = frame.alignment; bayer_ = frame.bayer;
        mean_.assign(pixels, 0.0f);
        variance_.assign(pixels, 0.0f);
        learnedFrames_ = 1;
        learnedSeconds_ = 0;
        firstFrame_ = true;
        for (int ry = view.ryMin; ry < view.ryMax; ++ry)
        {
            view.fillRow(ry, rowBuf_.data());
            float* dst = mean_.data() + static_cast<size_t>(ry) * width_;
            for (int rx = view.rxMin; rx < view.rxMax; ++rx) dst[rx] = rowBuf_[rx];
        }
        active_.clear();
        history_.clear();
        largeChange_ = {}; holdAll_ = false;
        if (warmupEnabled_)
        {
            const double warmupTarget = std::clamp(settings_.referenceTimeConstantSeconds, 0.0, kMaxWarmupSeconds);
            summary_.warmingUp = true;
            summary_.warmupTotalSeconds = warmupTarget;
            summary_.warmupRemainingSeconds = warmupTarget;
        }
    }
    else
    {
        firstFrame_ = false;
        const double warmupTarget = warmupEnabled_
            ? std::clamp(settings_.referenceTimeConstantSeconds, 0.0, kMaxWarmupSeconds)
            : 0.0;
        const bool warming = warmupEnabled_
            && ((learnedFrames_ < kMinWarmupFrames) || (learnedSeconds_ < warmupTarget));
        if (warming)
        {
            summary_.ready = false;
            summary_.warmingUp = true;
            summary_.warmupTotalSeconds = warmupTarget;
            summary_.warmupRemainingSeconds = std::max(0.0, warmupTarget - learnedSeconds_);
            active_.clear();
            largeChange_ = {};
            holdAll_ = false;
        }
        else
        {
        summary_.ready = true;
        // Feature-11: the mask as pixels, rebuilt when it or the frame changes.
        if (settings_.mask != maskBuiltFor_ || maskWidth_ != width_ || maskHeight_ != height_)
        {
            masked_.assign(settings_.mask.empty() ? 0 : pixels, 0);
            if (!settings_.mask.empty()) settings_.mask.fill(width_, height_, masked_.data());
            unmaskedPixels_ = pixels - static_cast<size_t>(std::count(masked_.begin(), masked_.end(), uint8_t{1}));
            maskBuiltFor_ = settings_.mask; maskWidth_ = width_; maskHeight_ = height_;
        }
        // Feature-31: with a ceiling, a pixel whose learned brightness is above
        // it (the sunlit side) is left out like a masked one. The reference
        // follows the Moon's drift, so the left-out area moves with it.
        const bool masking = !masked_.empty();
        const float ceiling = settings_.ignoreAboveShare > 0
            ? static_cast<float>(settings_.ignoreAboveShare * (format_ == PixelFormat::Mono16 ? (1 << bits_) - 1 : 255))
            : std::numeric_limits<float>::infinity();
        const auto isMasked = [&](size_t i) { return (masking && masked_[i]) || mean_[i] > ceiling; };

        // Median absolute deviation on a bounded grid within the valid overlap.
        // Removing the median difference keeps a uniform brightness step out of
        // the noise floor. Masked pixels are left out unless the mask covers all.
        grid_.clear();
        grid_.reserve(4096);
        const int validW = std::max(0, view.rxMax - view.rxMin);
        const int validH = std::max(0, view.ryMax - view.ryMin);
        const size_t validPixels = static_cast<size_t>(validW) * validH;
        if (validPixels > 0)
        {
            const size_t step = std::max(size_t{1}, (validPixels + 4095) / 4096);
            for (size_t k = 0; k < validPixels; k += step)
            {
                const int rx = view.rxMin + static_cast<int>(k % static_cast<size_t>(validW));
                const int ry = view.ryMin + static_cast<int>(k / static_cast<size_t>(validW));
                const size_t i = static_cast<size_t>(ry) * width_ + rx;
                if (!isMasked(i)) grid_.push_back(view.sampleOne(rx, ry) - mean_[i]);
            }
            if (grid_.empty())
            {
                for (size_t k = 0; k < validPixels; k += step)
                {
                    const int rx = view.rxMin + static_cast<int>(k % static_cast<size_t>(validW));
                    const int ry = view.ryMin + static_cast<int>(k / static_cast<size_t>(validW));
                    const size_t i = static_cast<size_t>(ry) * width_ + rx;
                    grid_.push_back(view.sampleOne(rx, ry) - mean_[i]);
                }
            }
        }
        const float centre = grid_.empty() ? 0.0f : median(grid_);
        for (float& value : grid_) value = std::abs(value - centre);
        summary_.noiseFloor = std::max(settings_.minNoiseAdu, grid_.empty() ? 0.0 : 1.4826 * median(grid_));

        // Feature-45: one direction of change is looked for, a rise or, with
        // darkening, a fall. A pixel's sigma below is its change in that
        // direction over its noise, so every measurement is of that direction.
        const bool falls = settings_.darkening;
        const double direction = falls ? -1.0 : 1.0;
        int left = width_, top = height_, right = -1, bottom = -1;
        thresholdPixels_.clear(); thresholdSigma_.clear();
        const size_t sparseLimit = std::min(pixels, std::max(size_t{4096}, static_cast<size_t>(settings_.patchMaxPixels()) * 4));
        const double floorVariance = summary_.noiseFloor * summary_.noiseFloor;
        std::shared_ptr<DetectionMap> map;
        if (display.makeMap && display.overlay != DetectionOverlay::None)
        {
            try
            {
                map = std::make_shared<DetectionMap>(budget_);
                map->width = width_; map->height = height_; map->kind = display.overlay;
                map->values.assign(pixels, 0.0f);
            }
            catch (const MemoryLimitError&) { map.reset(); summary_.overlaySkipped = true; }
        }
        const int probeRefX = display.probeX >= 0 ? display.probeX - view.nativeDx : -1;
        const int probeRefY = display.probeY >= 0 ? display.probeY - view.nativeDy : -1;
        for (int ry = view.ryMin; ry < view.ryMax; ++ry)
        {
            view.fillRow(ry, rowBuf_.data());
            const size_t rowOff = static_cast<size_t>(ry) * width_;
            const int ny = std::clamp(ry + view.nativeDy, 0, height_ - 1);
            const size_t mapRowOff = static_cast<size_t>(ny) * width_;
            for (int rx = view.rxMin; rx < view.rxMax; ++rx)
            {
                const size_t i = rowOff + rx;
                const double noise = std::sqrt(std::max(static_cast<double>(variance_[i]), floorVariance));
                const double delta = rowBuf_[rx] - mean_[i];
                const double sigma = direction * delta / noise;
                if (map)
                {
                    const int nx = std::clamp(rx + view.nativeDx, 0, width_ - 1);
                    map->values[mapRowOff + nx] = static_cast<float>(
                        display.overlay == DetectionOverlay::Noise ? noise : std::max(0.0, sigma));
                }
                if (isMasked(i)) continue;
                if (sigma > summary_.peakSigma)
                {
                    summary_.peakSigma = sigma; summary_.peakAdu = delta;
                    summary_.peakX = std::clamp(rx + view.nativeDx, 0, width_ - 1);
                    summary_.peakY = ny;
                }
                if (rx == probeRefX && ry == probeRefY) summary_.probeNoise = noise;
                if (sigma >= settings_.sigma)
                {
                    ++summary_.brightPixels;
                    if (thresholdPixels_.size() < sparseLimit)
                    {
                        thresholdPixels_.push_back(i);
                        thresholdSigma_.push_back(static_cast<float>(sigma));
                    }
                    left = std::min(left, rx); top = std::min(top, ry);
                    right = std::max(right, rx); bottom = std::max(bottom, ry);
                }
            }
        }
        if (summary_.brightPixels > 0)
        {
            const int nx0 = std::clamp(left + view.nativeDx, 0, width_ - 1);
            const int ny0 = std::clamp(top + view.nativeDy, 0, height_ - 1);
            const int nx1 = std::clamp(right + view.nativeDx, 0, width_ - 1);
            const int ny1 = std::clamp(bottom + view.nativeDy, 0, height_ - 1);
            summary_.boxX = nx0; summary_.boxY = ny0;
            summary_.boxWidth = nx1 - nx0 + 1; summary_.boxHeight = ny1 - ny0 + 1;
        }
        // The whole frame's measurement, before it is replaced by the best patch.
        const DetectionSummary whole = summary_;
        size_t newlyReleased = 0;
        DetectionSummary best = summary_;
        int bestPriority = -1;
        // A global bounding box lets an unrelated hot pixel veto a real
        // flash. Split the sparse threshold pixels into 8-connected patches.
        // Dense changes are rejected before this work; its storage/work is
        // bounded by max(4096, four times the observer's maximum patch size).
        // Feature-45: it searches the one direction looked for, with that
        // direction's limits and its patches from the frame before.
        const auto searchPatches = [&](const BudgetVector<size_t>& threshold, const BudgetVector<float>& strength,
            ChangeKind change, int maxPixels, double maxBoxRatio, const std::vector<Patch>& previous, std::vector<Patch>& current)
        {
            visited_.assign(threshold.size(), 0);
            for (size_t seed = 0; seed < threshold.size(); ++seed)
            {
                if (visited_[seed]) continue;
                pending_.clear(); pending_.push_back(seed); visited_[seed] = 1;
                int count = 0, x0 = width_, y0 = height_, x1 = -1, y1 = -1;
                double peak = 0; size_t peakAt = 0;
                while (!pending_.empty())
                {
                    const size_t item = pending_.back(); pending_.pop_back();
                    const size_t pixel = threshold[item];
                    const int x = static_cast<int>(pixel % static_cast<size_t>(width_));
                    const int y = static_cast<int>(pixel / static_cast<size_t>(width_));
                    ++count;
                    x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                    if (strength[item] > peak) { peak = strength[item]; peakAt = pixel; }
                    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                    {
                        if ((!dx && !dy) || x + dx < 0 || x + dx >= width_ || y + dy < 0 || y + dy >= height_) continue;
                        const size_t neighbor = static_cast<size_t>(y + dy) * width_ + x + dx;
                        const auto at = std::lower_bound(threshold.begin(), threshold.end(), neighbor);
                        if (at == threshold.end() || *at != neighbor) continue;
                        const size_t pos = static_cast<size_t>(at - threshold.begin());
                        if (!visited_[pos]) { visited_[pos] = 1; pending_.push_back(pos); }
                    }
                }
                const bool passes = count >= settings_.minPixels && count <= maxPixels
                    && static_cast<double>(x1 - x0 + 1) * (y1 - y0 + 1) <= maxBoxRatio * count;
                const int peakRx = static_cast<int>(peakAt % static_cast<size_t>(width_));
                const int peakRy = static_cast<int>(peakAt / static_cast<size_t>(width_));
                const int nx0 = std::clamp(x0 + view.nativeDx, 0, width_ - 1);
                const int ny0 = std::clamp(y0 + view.nativeDy, 0, height_ - 1);
                const int nx1 = std::clamp(x1 + view.nativeDx, 0, width_ - 1);
                const int ny1 = std::clamp(y1 + view.nativeDy, 0, height_ - 1);
                DetectionSummary measured = whole;
                measured.change = change;
                measured.passed = passes;
                measured.brightPixels = count;
                measured.boxX = nx0; measured.boxY = ny0;
                measured.boxWidth = nx1 - nx0 + 1; measured.boxHeight = ny1 - ny0 + 1;
                measured.peakSigma = peak;
                measured.peakX = std::clamp(peakRx + view.nativeDx, 0, width_ - 1);
                measured.peakY = std::clamp(peakRy + view.nativeDy, 0, height_ - 1);
                measured.peakAdu = view.sampleOne(peakRx, peakRy) - mean_[peakAt];
                if (passes)
                {
                    Patch patch{measured, x0, y0, x1 - x0 + 1, y1 - y0 + 1, 1, 0};
                    patch.measured.onsetFrameIndex = frame.frameIndex;
                    for (const auto& prior : previous)
                    {
                        const auto& p = prior.measured;
                        if (!p.firing && frame.frameIndex != p.frameIndex + 1) continue;
                        // Adjacent/overlapping boxes in reference coordinates follow a
                        // moving patch while keeping stationary patches locked across drift.
                        if (x0 > prior.refBoxX + prior.refBoxWidth || prior.refBoxX > x1 + 1
                            || y0 > prior.refBoxY + prior.refBoxHeight || prior.refBoxY > y1 + 1) continue;
                        patch.consecutive = std::max(patch.consecutive, std::min(2, prior.consecutive + 1));
                        patch.measured.onsetFrameIndex = std::min(patch.measured.onsetFrameIndex, p.onsetFrameIndex);
                        patch.measured.firing = patch.measured.firing || p.firing;
                        patch.seconds = std::max(patch.seconds, prior.seconds + frameInterval_);
                    }
                    patch.measured.triggered = !patch.measured.firing && (settings_.oneFrame || patch.consecutive >= 2);
                    patch.measured.firing = patch.measured.firing || patch.measured.triggered;
                    measured = patch.measured;
                    // Strongest passing patch on this frame (rise or fall, matching the watch).
                    if (!strongest_.passed || measured.peakSigma > strongest_.peakSigma) strongest_ = measured;
                    if (measured.triggered) candidates_.push_back(measured);
                    // Feature-11: past the release limit a patch is learned
                    // like the background. A star drifting past a fixed camera
                    // then fades; a plane moves on faster than it is learned.
                    const double release = settings_.releaseSeconds;
                    const bool released = release > 0 && patch.seconds >= release;
                    if (released && patch.seconds - frameInterval_ < release) ++newlyReleased;
                    current.push_back(std::move(patch));
                    if (!released)
                        for (int py = std::max(0, y0 - 1); py <= std::min(height_ - 1, y1 + 1); ++py)
                            std::fill(protected_.begin() + static_cast<ptrdiff_t>(py) * width_ + std::max(0, x0 - 1),
                                protected_.begin() + static_cast<ptrdiff_t>(py) * width_ + std::min(width_, x1 + 2), uint8_t{1});
                }
                const int priority = measured.triggered ? 3 : measured.firing ? 2 : passes ? 1 : 0;
                if (priority > bestPriority || (priority == bestPriority && measured.peakSigma > best.peakSigma))
                { best = measured; bestPriority = priority; }
            }
        };
        std::vector<Patch> patches;
        const bool limited = static_cast<size_t>(whole.brightPixels) > sparseLimit;
        if (!limited && !thresholdPixels_.empty())
            searchPatches(thresholdPixels_, thresholdSigma_, falls ? ChangeKind::Fall : ChangeKind::Rise,
                settings_.patchMaxPixels(), settings_.patchMaxBoxRatio(), active_, patches);
        if (bestPriority >= 0) summary_ = best;
        summary_.patchSearchLimited = limited;

        // Feature-11, Feature-13 and Feature-45: a large brightening or, with
        // darkening, a large darkening, when a set share of the unmasked
        // frame is at K at once. One frame is enough: a lightning stroke can
        // be one frame. While it lasts the whole frame is held from learning,
        // so a flash does not raise the noise of the frames after it.
        const double largeShare = falls ? settings_.darkeningShare : settings_.brighteningShare;
        holdAll_ = false;
        if (largeShare > 0 && unmaskedPixels_ > 0
            && static_cast<double>(whole.brightPixels) >= largeShare * static_cast<double>(unmaskedPixels_))
        {
            LargeChange& state = largeChange_;
            if (!state.active) { state.onset = frame.frameIndex; state.seconds = 0; }
            else state.seconds += frameInterval_;
            DetectionSummary measured = whole;
            measured.change = falls ? ChangeKind::LargeFall : ChangeKind::LargeRise;
            measured.share = static_cast<double>(whole.brightPixels) / static_cast<double>(unmaskedPixels_);
            measured.medianDifferenceAdu = centre;
            measured.passed = true;
            measured.triggered = !state.active;
            measured.firing = true;
            measured.onsetFrameIndex = state.onset;
            state.active = true;
            const double release = settings_.releaseSeconds;
            const bool released = release > 0 && state.seconds >= release;
            if (released && state.seconds - frameInterval_ < release) ++newlyReleased;
            if (measured.triggered) candidates_.push_back(measured);
            summary_ = measured;
            summary_.patchSearchLimited = limited;
            holdAll_ = !released;
        }
        else largeChange_.active = false;
        summary_.newlyReleased = newlyReleased;
        active_ = std::move(patches);
        map_ = std::move(map);
        }
    }
    summary_.newCandidates = candidates_.size(); summary_.activePatches = active_.size();
    // A held brightening protects every pixel, so nothing learns this frame.
    summary_.protectedPixels = holdAll_ ? mean_.size()
        : static_cast<size_t>(std::count(protected_.begin(), protected_.end(), uint8_t{1}));
    summary_.learningFrames = learnedFrames_;
    for (const auto& patch : active_) summary_.longestActiveSeconds = std::max(summary_.longestActiveSeconds, patch.seconds);
    const auto copyContext = [this](DetectionSummary& candidate) {
        candidate.newCandidates = summary_.newCandidates; candidate.activePatches = summary_.activePatches;
        candidate.protectedPixels = summary_.protectedPixels; candidate.learningFrames = summary_.learningFrames;
        candidate.longestActiveSeconds = summary_.longestActiveSeconds;
        candidate.stabilizationState = summary_.stabilizationState;
        candidate.stabilizationMethod = summary_.stabilizationMethod;
        candidate.stabilizationSource = summary_.stabilizationSource;
        candidate.stabilizationDx = summary_.stabilizationDx;
        candidate.stabilizationDy = summary_.stabilizationDy;
        candidate.stabilizationMs = summary_.stabilizationMs;
    };
    for (auto& candidate : candidates_) copyContext(candidate);
    copyContext(strongest_);
    awaitingUpdate_ = true;
    previousTime_ = frame.timestamp;
    if (summary_.ready)
    {
        history_.push_back({frame.timestamp, summary_.peakSigma, summary_.patchSearchLimited ? 0 : summary_.brightPixels});
        while (!history_.empty() && (history_.size() > 4096
            || frame.timestamp - history_.front().time > std::chrono::seconds{10})) history_.pop_front();
    }
    summary_.processingMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    strongest_.processingMs = summary_.processingMs;
    return summary_.triggered;
}

void FlashDetector::updateReference(const Frame& frame)
{
    if (!awaitingUpdate_ || frame.frameIndex != summary_.frameIndex || frame.timestamp != summary_.time
        || frame.width != width_ || frame.height != height_ || frame.format != format_
        || frame.significantBits != bits_ || frame.alignment != alignment_ || frame.bayer != bayer_
        || frame.data.size() != mean_.size() * static_cast<size_t>(bytesPerPixel(format_)))
        throw std::logic_error("reference update requires the frame just analyzed");
    if (!firstFrame_ && summary_.protectedPixels < mean_.size())
    {
        const auto started = std::chrono::steady_clock::now();
        const ShiftedView view(frame, lastShift_);
        const float alpha = static_cast<float>(std::max(1.0 / static_cast<double>(learnedFrames_ + 1),
            -std::expm1(-frameInterval_ / settings_.referenceTimeConstantSeconds)));
        const float keep = 1.0f - alpha;
        for (int ry = view.ryMin; ry < view.ryMax; ++ry)
        {
            view.fillRow(ry, rowBuf_.data());
            const size_t rowOff = static_cast<size_t>(ry) * width_;
            for (int rx = view.rxMin; rx < view.rxMax; ++rx)
            {
                const size_t i = rowOff + rx;
                if (protected_[i]) continue;
                const float delta = rowBuf_[rx] - mean_[i];
                mean_[i] += alpha * delta;
                variance_[i] = keep * (variance_[i] + alpha * delta * delta);
            }
        }
        if (learnedFrames_ < 1'000'000) ++learnedFrames_;
        learnedSeconds_ += frameInterval_;
        summary_.processingMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    awaitingUpdate_ = false;
}

void FlashDetector::recenterReference(int stepX, int stepY, const Frame& frame, FrameShift newShift)
{
    if (awaitingUpdate_) throw std::logic_error("cannot recenter reference while awaiting updateReference");
    if (learnedFrames_ == 0 || frame.width != width_ || frame.height != height_ || frame.format != format_
        || frame.significantBits != bits_ || frame.alignment != alignment_ || frame.bayer != bayer_)
        return;
    const bool bayer = bayer_ != BayerPattern::None && format_ != PixelFormat::RGB888;
    if (bayer)
    {
        stepX = 2 * (stepX / 2);
        stepY = 2 * (stepY / 2);
    }
    if (stepX == 0 && stepY == 0) return;

    const ShiftedView newView(frame, newShift);
    const int yStart = stepY > 0 ? height_ - 1 : 0;
    const int yEnd = stepY > 0 ? -1 : height_;
    const int yInc = stepY > 0 ? -1 : 1;
    const int xStart = stepX > 0 ? width_ - 1 : 0;
    const int xEnd = stepX > 0 ? -1 : width_;
    const int xInc = stepX > 0 ? -1 : 1;

    for (int ry = yStart; ry != yEnd; ry += yInc)
    {
        const int srcY = ry - stepY;
        const bool yInside = srcY >= 0 && srcY < height_;
        const size_t dstRow = static_cast<size_t>(ry) * width_;
        const size_t srcRow = yInside ? static_cast<size_t>(srcY) * width_ : 0;
        for (int rx = xStart; rx != xEnd; rx += xInc)
        {
            const int srcX = rx - stepX;
            const size_t dstIdx = dstRow + rx;
            if (yInside && srcX >= 0 && srcX < width_)
            {
                const size_t srcIdx = srcRow + srcX;
                mean_[dstIdx] = mean_[srcIdx];
                variance_[dstIdx] = variance_[srcIdx];
            }
            else
            {
                if (newView.rxMin < newView.rxMax && newView.ryMin < newView.ryMax)
                {
                    const int cx = std::clamp(rx, newView.rxMin, newView.rxMax - 1);
                    const int cy = std::clamp(ry, newView.ryMin, newView.ryMax - 1);
                    mean_[dstIdx] = newView.sampleOne(cx, cy);
                }
                else
                {
                    mean_[dstIdx] = 0.0f;
                }
                variance_[dstIdx] = 0.0f;
            }
        }
    }

    std::erase_if(active_, [&](Patch& patch) {
        patch.refBoxX += stepX;
        patch.refBoxY += stepY;
        return patch.refBoxX + patch.refBoxWidth <= 0 || patch.refBoxX >= width_
            || patch.refBoxY + patch.refBoxHeight <= 0 || patch.refBoxY >= height_;
    });
}

const std::vector<DetectionSummary>& FlashDetector::candidates() const { return candidates_; }
const DetectionSummary& FlashDetector::strongestPatch() const { return strongest_; }

bool FlashDetector::firing() const { return summary_.firing; }
int FlashDetector::lastBrightCount() const { return summary_.brightPixels; }
uint64_t FlashDetector::onsetFrameIndex() const { return summary_.onsetFrameIndex; }
const DetectionSummary& FlashDetector::summary() const { return summary_; }
DetectionView FlashDetector::view() const
{
    return {summary_, {history_.begin(), history_.end()}, map_};
}

void FlashDetector::difference(const Frame& frame, BudgetVector<float>& values) const
{
    if (!awaitingUpdate_ || frame.frameIndex != summary_.frameIndex || frame.timestamp != summary_.time)
        throw std::logic_error("copy the measured difference before updating the reference");
    if (frame.width != width_ || frame.height != height_ || frame.format != format_
        || frame.data.size() != frameBytes(width_, height_, format_))
        throw std::invalid_argument("difference requested for another frame layout");
    const ShiftedView view(frame, lastShift_);
    values.assign(mean_.size(), 0.0f);
    std::vector<float> row(static_cast<size_t>(width_));
    for (int ry = view.ryMin; ry < view.ryMax; ++ry)
    {
        view.fillRow(ry, row.data());
        const size_t refRow = static_cast<size_t>(ry) * width_;
        const int ny = std::clamp(ry + view.nativeDy, 0, height_ - 1);
        const size_t natRow = static_cast<size_t>(ny) * width_;
        for (int rx = view.rxMin; rx < view.rxMax; ++rx)
        {
            const int nx = std::clamp(rx + view.nativeDx, 0, width_ - 1);
            values[natRow + nx] = row[rx] - mean_[refRow + rx];
        }
    }
}

double FlashDetector::pixelNoise(size_t pixel) const
{
    if (pixel >= variance_.size() || !summary_.ready) return 0;
    const double floor = summary_.noiseFloor;
    return std::sqrt(std::max(static_cast<double>(variance_[pixel]), floor * floor));
}

size_t FlashDetector::workingBytes(size_t pixels, const DetectionSettings& settings)
{
    // Reference and variance, the protected and mask maps, and the bounded
    // sparse search for the patches of the one direction looked for.
    const size_t sparse = std::max(size_t{4096}, static_cast<size_t>(settings.patchMaxPixels()) * 4);
    return pixels * (2 * sizeof(float) + 2) + sparse * (sizeof(size_t) + sizeof(float) + 1 + sizeof(size_t)) + 4096 * sizeof(float);
}
