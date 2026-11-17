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

// The extent of a large change, in frame pixels.
struct Box
{
    int x{0}, y{0}, width{0}, height{0};
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
      masked_(BudgetAllocator<uint8_t>(budget_)), darkPixels_(BudgetAllocator<size_t>(budget_)),
      darkSigma_(BudgetAllocator<float>(budget_))
{
    setSettings(settings);
}

void FlashDetector::setSettings(const DetectionSettings& settings)
{
    if (!settings.valid()) throw std::invalid_argument("invalid detection settings");
    // Pending confirmation cannot combine different settings. Already firing
    // patches remain firing until absent, so an edit cannot duplicate them.
    if (settings != settings_)
    {
        std::erase_if(active_, [](const Patch& patch) { return !patch.measured.firing; });
        std::erase_if(activeDark_, [](const Patch& patch) { return !patch.measured.firing; });
    }
    settings_ = settings;
}

void FlashDetector::rearm()
{
    active_.clear();
    activeDark_.clear();
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
        darkPixels_ = BudgetVector<size_t>(darkPixels_.get_allocator());
        darkSigma_ = BudgetVector<float>(darkSigma_.get_allocator());
    }
    maskBuiltFor_ = {}; maskWidth_ = maskHeight_ = 0; unmaskedPixels_ = 0;
    brightening_ = {}; darkening_ = {}; holdAll_ = false;
    active_.clear(); activeDark_.clear(); candidates_.clear(); awaitingUpdate_ = false;
    history_.clear(); map_.reset();
    learnedFrames_ = 0;
    width_ = height_ = bits_ = 0;
    previousTime_ = {};
    summary_ = {};
    strongest_ = {};
}

bool FlashDetector::analyze(const Frame& frame, DetectionDisplayRequest display)
{
    if (awaitingUpdate_) throw std::logic_error("update the detector reference before analyzing another frame");
    const auto started = std::chrono::steady_clock::now();
    if (frame.width <= 0 || frame.height <= 0 || bytesPerPixel(frame.format) == 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format)
        || frame.significantBits < 1 || frame.significantBits > 16)
        throw std::invalid_argument("frame data does not match its size and pixel layout");
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    const Samples samples{frame, frame.format == PixelFormat::Mono16 && frame.alignment == BitAlignment::High
        ? 16 - frame.significantBits : 0};
    summary_ = {};
    strongest_ = {};
    summary_.settings = settings_;
    summary_.frameIndex = frame.frameIndex;
    summary_.time = frame.timestamp;
    summary_.probeX = display.probeX; summary_.probeY = display.probeY;
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
        mean_.resize(pixels);
        variance_.assign(pixels, 0);
        learnedFrames_ = 1;
        for (size_t i = 0; i < pixels; ++i) mean_[i] = samples.at(i);
        active_.clear();
        activeDark_.clear();
        history_.clear();
        brightening_ = {}; darkening_ = {}; holdAll_ = false;
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
        const bool masking = !masked_.empty();
        const auto isMasked = [&](size_t i) { return masking && masked_[i]; };

        // Median absolute deviation on a bounded grid. Removing the median
        // difference keeps a uniform brightness step out of the noise floor.
        // Masked pixels are left out unless the mask covers the whole grid.
        grid_.clear();
        grid_.reserve(4096);
        const size_t step = std::max(size_t{1}, (pixels + 4095) / 4096);
        for (size_t i = 0; i < pixels; i += step)
            if (!isMasked(i)) grid_.push_back(samples.at(i) - mean_[i]);
        if (grid_.empty())
            for (size_t i = 0; i < pixels; i += step) grid_.push_back(samples.at(i) - mean_[i]);
        const float centre = median(grid_);
        for (float& value : grid_) value = std::abs(value - centre);
        summary_.noiseFloor = std::max(settings_.minNoiseAdu, 1.4826 * median(grid_));

        int left = width_, top = height_, right = -1, bottom = -1;
        int darkLeft = width_, darkTop = height_, darkRight = -1, darkBottom = -1;
        thresholdPixels_.clear(); thresholdSigma_.clear();
        darkPixels_.clear(); darkSigma_.clear();
        const size_t sparseLimit = std::min(pixels, std::max(size_t{4096}, static_cast<size_t>(settings_.maxPixels) * 4));
        // Feature-13: the dark side's search has its own bound, from its own maximum patch.
        const size_t darkSparseLimit = std::min(pixels, std::max(size_t{4096}, static_cast<size_t>(settings_.maxDarkPixels) * 4));
        const bool countDark = settings_.darkening || settings_.darkeningShare > 0;
        const double floorVariance = summary_.noiseFloor * summary_.noiseFloor;
        std::shared_ptr<DetectionMap> map;
        if (display.makeMap && display.overlay != DetectionOverlay::None)
        {
            try
            {
                map = std::make_shared<DetectionMap>(budget_);
                map->width = width_; map->height = height_; map->kind = display.overlay;
                map->values.resize(pixels);
            }
            catch (const MemoryLimitError&) { map.reset(); summary_.overlaySkipped = true; }
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
                if (isMasked(i)) continue;
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
                else if (countDark && sigma <= -settings_.sigma)
                {
                    ++summary_.darkPixels;
                    summary_.deepestSigma = std::max(summary_.deepestSigma, -sigma);
                    if (darkPixels_.size() < darkSparseLimit)
                    {
                        darkPixels_.push_back(i);
                        darkSigma_.push_back(static_cast<float>(-sigma));
                    }
                    darkLeft = std::min(darkLeft, x); darkTop = std::min(darkTop, y);
                    darkRight = std::max(darkRight, x); darkBottom = std::max(darkBottom, y);
                }
            }
        }
        if (summary_.brightPixels > 0)
        {
            summary_.boxX = left; summary_.boxY = top;
            summary_.boxWidth = right - left + 1; summary_.boxHeight = bottom - top + 1;
        }
        // The whole frame's measurement, before it is replaced by the best patch.
        const DetectionSummary whole = summary_;
        size_t newlyReleased = 0;
        DetectionSummary best = summary_;
        int bestPriority = -1;
        int largestDark = 0;
        // A global bounding box lets an unrelated hot pixel veto a real
        // flash. Split the sparse threshold pixels into 8-connected patches.
        // Dense changes are rejected before this work; its storage/work is
        // bounded by max(4096, four times the observer's maximum patch size).
        // The same search serves rises and, with Feature-13, falls: each sign
        // with its own pixels, limits and patches from the frame before, so a
        // rising and a falling patch are never merged.
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
                if (change == ChangeKind::Fall) largestDark = std::max(largestDark, count);
                const bool passes = count >= settings_.minPixels && count <= maxPixels
                    && static_cast<double>(x1 - x0 + 1) * (y1 - y0 + 1) <= maxBoxRatio * count;
                DetectionSummary measured = whole;
                measured.change = change;
                measured.passed = passes;
                measured.brightPixels = count; measured.boxX = x0; measured.boxY = y0;
                measured.boxWidth = x1-x0+1; measured.boxHeight = y1-y0+1;
                measured.peakSigma = peak;
                measured.peakX = static_cast<int>(peakAt % static_cast<size_t>(width_));
                measured.peakY = static_cast<int>(peakAt / static_cast<size_t>(width_));
                measured.peakAdu = samples.at(peakAt)-mean_[peakAt];
                if (passes)
                {
                    Patch patch{measured, 1, 0};
                    patch.measured.onsetFrameIndex = frame.frameIndex;
                    for (const auto& prior : previous)
                    {
                        const auto& p = prior.measured;
                        if (!p.firing && frame.frameIndex!=p.frameIndex+1) continue;
                        // Adjacent/overlapping boxes follow a moving patch.
                        // Merges and splits inherit firing, rather than duplicating
                        // a continuing event. Separate boxes remain independent.
                        if (x0 > p.boxX+p.boxWidth || p.boxX > x1+1
                            || y0 > p.boxY+p.boxHeight || p.boxY > y1+1) continue;
                        patch.consecutive = std::max(patch.consecutive, std::min(2,prior.consecutive+1));
                        patch.measured.onsetFrameIndex = std::min(patch.measured.onsetFrameIndex,p.onsetFrameIndex);
                        patch.measured.firing = patch.measured.firing || p.firing;
                        patch.seconds = std::max(patch.seconds,prior.seconds+frameInterval_);
                    }
                    patch.measured.triggered = !patch.measured.firing && (settings_.oneFrame || patch.consecutive>=2);
                    patch.measured.firing = patch.measured.firing || patch.measured.triggered;
                    measured = patch.measured;
                    // The clip's peak preview is a positive difference: rises only.
                    if (change == ChangeKind::Rise && (!strongest_.passed || measured.peakSigma>strongest_.peakSigma)) strongest_=measured;
                    if (measured.triggered) candidates_.push_back(measured);
                    // Feature-11: past the release limit a patch is learned
                    // like the background. A star drifting past a fixed camera
                    // then fades; a plane moves on faster than it is learned.
                    const double release = settings_.releaseSeconds;
                    const bool released = release > 0 && patch.seconds >= release;
                    if (released && patch.seconds - frameInterval_ < release) ++newlyReleased;
                    current.push_back(std::move(patch));
                    if (!released)
                        for (int py=std::max(0,y0-1); py<=std::min(height_-1,y1+1); ++py)
                            std::fill(protected_.begin()+static_cast<ptrdiff_t>(py)*width_+std::max(0,x0-1),
                                protected_.begin()+static_cast<ptrdiff_t>(py)*width_+std::min(width_,x1+2),uint8_t{1});
                }
                const int priority = measured.triggered?3:measured.firing?2:passes?1:0;
                if (priority>bestPriority || (priority==bestPriority && measured.peakSigma>best.peakSigma))
                { best=measured; bestPriority=priority; }
            }
        };
        std::vector<Patch> rising, falling;
        const bool riseLimited = static_cast<size_t>(whole.brightPixels) > sparseLimit;
        if (!riseLimited && !thresholdPixels_.empty())
            searchPatches(thresholdPixels_, thresholdSigma_, ChangeKind::Rise, settings_.maxPixels, settings_.maxPatchBoxRatio, active_, rising);
        const bool darkLimited = countDark && static_cast<size_t>(whole.darkPixels) > darkSparseLimit;
        if (settings_.darkening && !darkLimited && !darkPixels_.empty())
            searchPatches(darkPixels_, darkSigma_, ChangeKind::Fall, settings_.maxDarkPixels, settings_.maxDarkBoxRatio, activeDark_, falling);
        if (bestPriority >= 0) summary_ = best;
        summary_.patchSearchLimited = riseLimited;
        summary_.darkSearchLimited = darkLimited;
        summary_.largestDarkPatch = largestDark;

        // Feature-11 and Feature-13: a large brightening or darkening, when a
        // set share of the unmasked frame is at K at once. One frame is
        // enough: a lightning stroke can be one frame. While it lasts the
        // whole frame is held from learning, so a flash does not raise the
        // noise of the frames after it. Returns whether it holds the frame.
        const auto largeChange = [&](LargeChange& state, int count, double share, ChangeKind change, Box box)
        {
            const bool large = share > 0 && unmaskedPixels_ > 0
                && static_cast<double>(count) >= share * static_cast<double>(unmaskedPixels_);
            if (!large)
            {
                state.active = false;
                return false;
            }
            if (!state.active) { state.onset = frame.frameIndex; state.seconds = 0; }
            else state.seconds += frameInterval_;
            DetectionSummary measured = whole;
            measured.change = change;
            measured.brightPixels = count;
            measured.boxX = box.x; measured.boxY = box.y; measured.boxWidth = box.width; measured.boxHeight = box.height;
            if (change == ChangeKind::LargeFall) measured.peakSigma = whole.deepestSigma;
            measured.share = static_cast<double>(count) / static_cast<double>(unmaskedPixels_);
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
            const bool darkSearch = summary_.darkSearchLimited;
            summary_ = measured;
            summary_.patchSearchLimited = riseLimited;
            summary_.darkSearchLimited = darkSearch;
            summary_.largestDarkPatch = largestDark;
            return !released;
        };
        const bool holdRise = largeChange(brightening_, whole.brightPixels, settings_.brighteningShare, ChangeKind::LargeRise,
            Box{whole.boxX, whole.boxY, whole.boxWidth, whole.boxHeight});
        const bool holdFall = largeChange(darkening_, whole.darkPixels, settings_.darkeningShare, ChangeKind::LargeFall,
            whole.darkPixels > 0 ? Box{darkLeft, darkTop, darkRight - darkLeft + 1, darkBottom - darkTop + 1} : Box{});
        holdAll_ = holdRise || holdFall;
        summary_.newlyReleased = newlyReleased;
        active_ = std::move(rising);
        activeDark_ = std::move(falling);
        map_ = std::move(map);
    }
    summary_.newCandidates = candidates_.size(); summary_.activePatches = active_.size() + activeDark_.size();
    // A held brightening protects every pixel, so nothing learns this frame.
    summary_.protectedPixels = holdAll_ ? mean_.size()
        : static_cast<size_t>(std::count(protected_.begin(),protected_.end(),uint8_t{1}));
    summary_.learningFrames = learnedFrames_;
    for (const auto& patch:active_) summary_.longestActiveSeconds=std::max(summary_.longestActiveSeconds,patch.seconds);
    for (const auto& patch:activeDark_) summary_.longestActiveSeconds=std::max(summary_.longestActiveSeconds,patch.seconds);
    const auto copyContext=[this](DetectionSummary& candidate) {
        candidate.newCandidates=summary_.newCandidates; candidate.activePatches=summary_.activePatches;
        candidate.protectedPixels=summary_.protectedPixels; candidate.learningFrames=summary_.learningFrames;
        candidate.longestActiveSeconds=summary_.longestActiveSeconds;
    };
    for (auto& candidate:candidates_) copyContext(candidate);
    copyContext(strongest_);
    awaitingUpdate_ = true;
    previousTime_ = frame.timestamp;
    history_.push_back({frame.timestamp, summary_.peakSigma, summary_.patchSearchLimited ? 0 : summary_.brightPixels});
    while (!history_.empty() && (history_.size() > 4096
        || frame.timestamp - history_.front().time > std::chrono::seconds{10})) history_.pop_front();
    summary_.processingMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    strongest_.processingMs=summary_.processingMs;
    return summary_.triggered;
}

void FlashDetector::updateReference(const Frame& frame)
{
    if (!awaitingUpdate_ || frame.frameIndex!=summary_.frameIndex || frame.timestamp!=summary_.time
        || frame.width!=width_ || frame.height!=height_ || frame.format!=format_
        || frame.significantBits!=bits_ || frame.alignment!=alignment_ || frame.bayer!=bayer_
        || frame.data.size()!=mean_.size()*static_cast<size_t>(bytesPerPixel(format_)))
        throw std::logic_error("reference update requires the frame just analyzed");
    if (summary_.ready && summary_.protectedPixels<mean_.size())
    {
        const auto started=std::chrono::steady_clock::now();
        const Samples samples{frame, format_==PixelFormat::Mono16 && alignment_==BitAlignment::High?16-bits_:0};
        const float alpha=static_cast<float>(std::max(1.0/static_cast<double>(learnedFrames_+1),
            -std::expm1(-frameInterval_/settings_.referenceTimeConstantSeconds)));
        for (size_t i=0;i<mean_.size();++i)
        {
            if (protected_[i]) continue;
            const float delta=samples.at(i)-mean_[i];
            mean_[i]+=alpha*delta;
            variance_[i]=(1-alpha)*(variance_[i]+alpha*delta*delta);
        }
        if (learnedFrames_<1'000'000) ++learnedFrames_;
        summary_.processingMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    }
    awaitingUpdate_=false;
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
    if (!awaitingUpdate_ || frame.frameIndex!=summary_.frameIndex || frame.timestamp!=summary_.time)
        throw std::logic_error("copy the measured difference before updating the reference");
    if (frame.width != width_ || frame.height != height_ || frame.format != format_
        || frame.data.size() != frameBytes(width_, height_, format_))
        throw std::invalid_argument("difference requested for another frame layout");
    const Samples samples{frame, format_ == PixelFormat::Mono16 && alignment_ == BitAlignment::High ? 16 - bits_ : 0};
    values.resize(mean_.size());
    for (size_t i = 0; i < values.size(); ++i) values[i] = samples.at(i) - mean_[i];
}
