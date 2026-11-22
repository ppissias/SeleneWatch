#include "auto_tune_watch.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

double median(std::vector<double> values)
{
    if (values.empty()) return 0;
    const auto middle = values.begin() + static_cast<ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}
// The median noise the detector uses for the area's pixels, in native ADU.
double areaNoise(const FlashDetector& detector, const TuneArea& area, size_t pixels)
{
    std::vector<double> noise;
    size_t inside = 0;
    for (size_t i = 0; i < pixels; ++i)
        if (area.wholeFrame() || area.inside[i]) ++inside;
    const size_t step = std::max<size_t>(1, inside / 4096);
    size_t seen = 0;
    for (size_t i = 0; i < pixels; ++i)
    {
        if (!area.wholeFrame() && !area.inside[i]) continue;
        if (seen++ % step) continue;
        if (const double n = detector.pixelNoise(i); n > 0) noise.push_back(n);
    }
    return median(std::move(noise));
}

} // namespace

uint64_t AutoTuneWatch::start(TuneWatchRequest request)
{
    std::lock_guard lock(mutex_);
    const uint64_t id = nextId_++;
    published_ = {};
    published_.state = TuneWatchStatus::State::Waiting;
    published_.id = id;
    published_.kind = request.kind;
    published_.requestedSeconds = request.seconds;
    published_.note = "Starting…";
    pending_ = std::move(request);
    cancelRequest_.reset();
    busy_.store(true);
    return id;
}

void AutoTuneWatch::cancel(const std::string& reason)
{
    std::lock_guard lock(mutex_);
    if (!busy_.load()) return;
    pending_.reset();
    cancelRequest_ = reason;
    // Shown at once; the source thread drops its work at the next frame.
    if (published_.state == TuneWatchStatus::State::Waiting || published_.state == TuneWatchStatus::State::Measuring)
    {
        published_.state = TuneWatchStatus::State::Cancelled;
        published_.note = reason;
    }
}

TuneWatchStatus AutoTuneWatch::status() const
{
    std::lock_guard lock(mutex_);
    return published_;
}

std::optional<BlipSettings> AutoTuneWatch::onFrame(const Frame& frame, const FlashDetector& live,
    const std::vector<DetectionSummary>& liveCandidates, const Context& context)
{
    std::optional<std::string> cancelled;
    {
        std::lock_guard lock(mutex_);
        if (pending_)
        {
            request_ = std::move(*pending_);
            pending_.reset();
            working_ = published_;
            rows_.clear();
            levels_.clear(); atZero_.clear(); atMax_.clear();
            noise_ = {};
            started_ = {};
            haveLast_ = false;
            interval_ = 0;
            injecting_ = false;
            flashFirst_.reset(); flashLast_.reset();
            footprint_.reset();
            footprintMax_ = 0;
            changed_ = true;
        }
        if (cancelRequest_)
        {
            cancelled = cancelRequest_;
            cancelRequest_.reset();
        }
    }
    if (cancelled)
    {
        if (request_) finish(TuneWatchStatus::State::Cancelled, *cancelled);
        return std::nullopt;
    }
    if (!request_) return std::nullopt;

    // The stream's pace, for the detector's learning time and the frame budget.
    if (haveLast_ && frame.timestamp > lastTime_)
    {
        const double dt = std::chrono::duration<double>(frame.timestamp - lastTime_).count();
        interval_ = interval_ > 0 ? 0.9 * interval_ + 0.1 * dt : dt;
    }
    const bool looped = haveLast_ && frame.frameIndex <= lastIndex_;
    haveLast_ = true;
    lastTime_ = frame.timestamp;
    lastIndex_ = frame.frameIndex;

    if (!context.preview)
    {
        finish(TuneWatchStatus::State::Cancelled, "Monitoring started: auto-tune measures only in Preview.");
        return std::nullopt;
    }
    if (context.settings != request_->settings)
    {
        finish(TuneWatchStatus::State::Cancelled,
            "The detection settings changed during the measurement, so its results would not match them.");
        return std::nullopt;
    }
    if (!request_->area.fits(frame))
    {
        finish(TuneWatchStatus::State::Cancelled, "The frame size changed during the measurement.");
        return std::nullopt;
    }
    if (looped && working_.state == TuneWatchStatus::State::Measuring)
    {
        sourceInterrupted("the recording started again from its beginning");
        return std::nullopt;
    }

    if (working_.state == TuneWatchStatus::State::Waiting)
    {
        if (context.settling)
        {
            working_.note = "Waiting for the camera to settle…";
            return std::nullopt;
        }
        if (request_->kind != TuneWatchRequest::Kind::Brightness)
        {
            // The copies start from the live detector's background, so it
            // must have learned for a few of its time constants first.
            const DetectionSummary& summary = live.summary();
            const double needed = std::min(10.0, 3 * request_->settings.referenceTimeConstantSeconds);
            const double learned = static_cast<double>(summary.learningFrames) * (interval_ > 0 ? interval_ : 1.0 / 30);
            if (!summary.ready || learned < needed)
            {
                working_.note = std::format("Waiting for the detector to learn the background ({:.0f} of {:.0f} s)…",
                    std::min(learned, needed), needed);
                return std::nullopt;
            }
        }
        begin(frame, live, context);
        if (!request_) return std::nullopt;
        if (request_->kind == TuneWatchRequest::Kind::FlashCheck || request_->kind == TuneWatchRequest::Kind::Footprint)
        {
            injecting_ = true;
            flashAsked_ = frame.timestamp;
            return request_->flash;
        }
        // Brightness is measured from this frame; the K rows from the next.
        if (request_->kind == TuneWatchRequest::Kind::Sensitivity) return std::nullopt;
    }
    measure(frame, live, liveCandidates);
    return std::nullopt;
}

void AutoTuneWatch::begin(const Frame& frame, const FlashDetector& live, const Context& context)
{
    working_.state = TuneWatchStatus::State::Measuring;
    working_.note.clear();
    working_.format = frame.format;
    working_.significantBits = frame.significantBits;
    changed_ = true;
    if (request_->kind == TuneWatchRequest::Kind::Footprint)
    {
        // A copy that keeps patches of any size, so the flash's whole patch
        // at K is reported; its peak is set from the noise at its pixel.
        BlipSettings& flash = request_->flash;
        const size_t at = static_cast<size_t>(flash.y) * frame.width + flash.x;
        const double noise = live.pixelNoise(at);
        if (flash.x < 0 || flash.y < 0 || flash.x >= frame.width || flash.y >= frame.height || noise <= 0)
        {
            finish(TuneWatchStatus::State::Cancelled, "The footprint flash has no measured noise at its place.");
            return;
        }
        flash.amplitude = request_->footprintPeakInK * request_->settings.sigma * noise;
        working_.footprintPeakAdu = flash.amplitude;
        try
        {
            footprint_ = std::make_unique<FlashDetector>(live);
            DetectionSettings settings = request_->settings;
            settings.minPixels = 1;
            settings.maxPixels = std::max(settings.maxPixels, 1000);
            settings.oneFrame = true;
            footprint_->setSettings(settings);
            footprint_->rearm();
        }
        catch (const MemoryLimitError&)
        {
            finish(TuneWatchStatus::State::Cancelled, "The RAM ceiling leaves no room to measure the footprint.");
        }
        footprintMax_ = 0;
        return;
    }
    if (request_->kind != TuneWatchRequest::Kind::Sensitivity) return;

    const TuneArea& area = request_->area;
    if (!area.wholeFrame() && std::none_of(area.inside.begin(), area.inside.end(), [](uint8_t v) { return v != 0; }))
    {
        finish(TuneWatchStatus::State::Cancelled, "The mask covers the whole frame: there is nothing to watch.");
        return;
    }
    working_.areaNoiseAdu = areaNoise(live, area, static_cast<size_t>(frame.width) * frame.height);
    // Feature-30: the rows compare minimum patches at one K, or K values.
    const bool minPatches = !request_->minPixelValues.empty();
    const size_t wanted = minPatches ? request_->minPixelValues.size() : request_->kValues.size();
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    const size_t perRow = std::max<size_t>(1, FlashDetector::workingBytes(pixels, request_->settings));
    // The live detector already takes its time; the copies share what is left
    // of seven tenths of a frame.
    size_t byTime = wanted;
    const double processing = live.summary().processingMs;
    if (processing > 0 && interval_ > 0)
        byTime = static_cast<size_t>(std::max(0.0, std::floor(0.7 * interval_ * 1000 / processing) - 1));
    const size_t byMemory = context.spareBytes / perRow;
    const size_t count = std::min({wanted, byTime, byMemory});
    std::string limit = byMemory < std::min(wanted, byTime) ? "the RAM ceiling's processing reserve" : "the time each frame leaves";
    for (size_t i = 0; i < count; ++i)
    {
        try
        {
            auto copy = std::make_unique<FlashDetector>(live);
            DetectionSettings settings = request_->settings;
            if (minPatches) settings.minPixels = request_->minPixelValues[i];
            else settings.sigma = request_->kValues[i];
            copy->setSettings(settings);
            copy->rearm();
            Row row;
            row.detector = std::move(copy);
            row.result.k = settings.sigma;
            row.result.minPixels = minPatches ? settings.minPixels : 0;
            rows_.push_back(std::move(row));
        }
        catch (const MemoryLimitError&)
        {
            limit = "the RAM ceiling";
            break;
        }
    }
    if (rows_.empty())
    {
        working_.liveOnly = true;
        Row row;
        row.result.k = request_->settings.sigma;
        row.result.minPixels = minPatches ? request_->settings.minPixels : 0;
        rows_.push_back(std::move(row));
        working_.note = minPatches
            ? std::format("Only the current minimum patch was measured: {} leaves no room for more detectors.", limit)
            : std::format("Only the current K was measured: {} leaves no room for more detectors. "
                "Use Try K for another value.", limit);
    }
    else if (rows_.size() < wanted)
        working_.note = std::format("{} of {} {} were compared: {} leaves no room for more.", rows_.size(), wanted,
            minPatches ? "minimum patches" : "K values", limit);
}

void AutoTuneWatch::record(KRowResult& result, const FlashDetector& detector,
    const std::vector<DetectionSummary>& candidates, bool darkening)
{
    const DetectionSummary& summary = detector.summary();
    if (summary.patchSearchLimited || (darkening && summary.darkSearchLimited)) ++result.limitedFrames;
    for (const DetectionSummary& c : candidates)
    {
        ++result.events;
        if (result.kept.size() < KRowResult::kMaxKept)
            result.kept.push_back({c.frameIndex, c.time, c.change, c.boxX, c.boxY, c.boxWidth, c.boxHeight, c.brightPixels, c.peakSigma});
    }
}

void AutoTuneWatch::measure(const Frame& frame, const FlashDetector& live, const std::vector<DetectionSummary>& liveCandidates)
{
    const TuneWatchRequest& request = *request_;
    if (started_ == std::chrono::system_clock::time_point{}) started_ = frame.timestamp;
    ++working_.frames;
    const double frameSeconds = interval_ > 0 ? interval_ : 1.0 / 30;
    working_.seconds = std::chrono::duration<double>(frame.timestamp - started_).count() + frameSeconds;
    working_.progress = request.seconds > 0 ? std::min(1.0, working_.seconds / request.seconds) : 1.0;

    switch (request.kind)
    {
    case TuneWatchRequest::Kind::Brightness:
        if (const auto sample = measureBrightness(frame, request.area, request.brightest))
        {
            levels_.push_back(sample->level);
            atZero_.push_back(sample->atZero);
            atMax_.push_back(sample->atMax);
            noise_.add(frame, request.area);
        }
        break;
    case TuneWatchRequest::Kind::Sensitivity:
        for (Row& row : rows_)
        {
            if (!row.result.stopped.empty()) continue;
            if (!row.detector)
                record(row.result, live, liveCandidates, request.settings.darkening);
            else
            {
                try
                {
                    row.detector->analyze(frame);
                    record(row.result, *row.detector, row.detector->candidates(), request.settings.darkening);
                    row.detector->updateReference(frame);
                }
                catch (const MemoryLimitError&)
                {
                    row.result.stopped = "Stopped: the RAM ceiling was reached.";
                    row.detector.reset();
                    continue;
                }
            }
            ++row.result.frames;
            row.result.seconds = working_.seconds;
        }
        // The area's brightness too, on every fifth frame.
        if (working_.frames % 5 == 1)
            if (const auto sample = measureBrightness(frame, request.area, request.brightest))
            {
                levels_.push_back(sample->level);
                atZero_.push_back(sample->atZero);
                atMax_.push_back(sample->atMax);
            }
        break;
    case TuneWatchRequest::Kind::FlashCheck:
    {
        if (frame.blip)
        {
            if (!flashFirst_) flashFirst_ = frame.frameIndex;
            flashLast_ = frame.frameIndex;
        }
        if (flashFirst_)
        {
            const BlipSettings& flash = request.flash;
            const int reach = static_cast<int>(std::ceil(flash.fwhm)) + 1;
            for (const DetectionSummary& c : liveCandidates)
            {
                if (c.change != ChangeKind::Rise && c.change != ChangeKind::LargeRise) continue;
                if (flash.x < c.boxX - reach || flash.x > c.boxX + c.boxWidth - 1 + reach
                    || flash.y < c.boxY - reach || flash.y > c.boxY + c.boxHeight - 1 + reach) continue;
                working_.recovered = true;
                working_.checkText = std::format("Caught: the detector at K {:.1f} found the test flash ({:.0f} ADU) "
                    "at its place, {} frame(s) after it began, at {:.1f} sigma.", request.settings.sigma, flash.amplitude,
                    frame.frameIndex - *flashFirst_, c.peakSigma);
                finish(TuneWatchStatus::State::Finished, {});
                return;
            }
            if (frame.frameIndex > *flashLast_ + 2)
            {
                working_.recovered = false;
                working_.checkText = std::format("Missed: the detector at K {:.1f} did not find the test flash ({:.0f} ADU) "
                    "at its place and time.", request.settings.sigma, flash.amplitude);
                finish(TuneWatchStatus::State::Finished, {});
                return;
            }
        }
        else if (frame.timestamp - flashAsked_ > std::chrono::seconds{3})
            finish(TuneWatchStatus::State::Cancelled, "The test flash was not injected; another flash may have been in progress.");
        return;
    }
    case TuneWatchRequest::Kind::Footprint:
    {
        // Feature-30: the copy learns on every frame, and on the flash's
        // frames reports the patch at its place: the pixels at K it covers.
        const BlipSettings& flash = request.flash;
        try
        {
            footprint_->analyze(frame);
            if (frame.blip)
            {
                if (!flashFirst_) flashFirst_ = frame.frameIndex;
                flashLast_ = frame.frameIndex;
                const int reach = static_cast<int>(std::ceil(flash.fwhm)) + 1;
                for (const DetectionSummary& c : footprint_->candidates())
                    if (c.change == ChangeKind::Rise && flash.x >= c.boxX - reach && flash.x <= c.boxX + c.boxWidth - 1 + reach
                        && flash.y >= c.boxY - reach && flash.y <= c.boxY + c.boxHeight - 1 + reach)
                        footprintMax_ = std::max(footprintMax_, c.brightPixels);
            }
            footprint_->updateReference(frame);
        }
        catch (const MemoryLimitError&)
        {
            finish(TuneWatchStatus::State::Cancelled, "The RAM ceiling was reached while measuring the footprint.");
            return;
        }
        if (flashLast_ && frame.frameIndex > *flashLast_ + 1)
        {
            if (footprintMax_ > 0)
            {
                working_.footprintPixels = footprintMax_;
                working_.checkText = std::format("A flash {:.1f} px wide, peaking at {:.0f} ADU ({:.1f} × K), covers {} px at K "
                    "{:.1f}.", flash.fwhm, flash.amplitude, request.footprintPeakInK, footprintMax_, request.settings.sigma);
            }
            else
                working_.checkText = "The footprint flash was not found at its place: it may be masked, above Ignore brighter "
                    "than, or too faint for the noise there.";
            finish(TuneWatchStatus::State::Finished, {});
        }
        else if (!flashFirst_ && frame.timestamp - flashAsked_ > std::chrono::seconds{3})
            finish(TuneWatchStatus::State::Cancelled, "The footprint flash was not injected; another flash may have been in progress.");
        return;
    }
    }

    if (working_.seconds >= request.seconds)
    {
        if (request.kind == TuneWatchRequest::Kind::Sensitivity)
            working_.areaNoiseAdu = areaNoise(live, request.area, static_cast<size_t>(frame.width) * frame.height);
        finish(TuneWatchStatus::State::Finished, working_.note);
    }
    else if (request.kind == TuneWatchRequest::Kind::Sensitivity && std::all_of(rows_.begin(), rows_.end(),
        [](const Row& row) { return !row.result.stopped.empty(); }))
        finish(TuneWatchStatus::State::Finished, "Every K row stopped early: the RAM ceiling was reached.");
}

void AutoTuneWatch::finish(TuneWatchStatus::State state, const std::string& note)
{
    working_.state = state;
    working_.note = note;
    if (!levels_.empty())
        working_.brightness = BrightnessSample{median(levels_), median(atZero_), median(atMax_), levels_.size(),
            noise_.median().value_or(0)};

    working_.rows.clear();
    for (const Row& row : rows_) working_.rows.push_back(row.result);
    rows_.clear();
    footprint_.reset();
    request_.reset();
    injecting_ = false;
    changed_ = true;
    std::lock_guard lock(mutex_);
    published_ = working_;
    busy_.store(pending_.has_value());
}

void AutoTuneWatch::publish()
{
    working_.rows.clear();
    for (const Row& row : rows_) working_.rows.push_back(row.result);
    std::lock_guard lock(mutex_);
    // A cancel shown by the window is not overwritten by progress.
    if (cancelRequest_ || pending_) return;
    published_ = working_;
}

bool AutoTuneWatch::takeChanged(std::chrono::steady_clock::time_point now)
{
    const bool running = request_.has_value();
    if (!changed_ && !(running && now - lastNotice_ >= std::chrono::milliseconds{250})) return false;
    if (running) publish();
    changed_ = false;
    lastNotice_ = now;
    return true;
}

void AutoTuneWatch::sourceInterrupted(const std::string& reason)
{
    if (!request_) return;
    if (working_.state == TuneWatchStatus::State::Measuring && request_->kind != TuneWatchRequest::Kind::FlashCheck)
        finish(TuneWatchStatus::State::Finished, std::format("Ended early: {}. It covered {:.1f} s of the {:.0f} s asked for.",
            reason, working_.seconds, request_->seconds));
    else finish(TuneWatchStatus::State::Cancelled, "Stopped: " + reason + ".");
}

void AutoTuneWatch::stopped(const std::string& reason)
{
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        cancelRequest_.reset();
    }
    if (request_) finish(TuneWatchStatus::State::Cancelled, "Stopped: " + reason + ".");
    std::lock_guard lock(mutex_);
    if (published_.state == TuneWatchStatus::State::Waiting || published_.state == TuneWatchStatus::State::Measuring)
    {
        published_.state = TuneWatchStatus::State::Cancelled;
        published_.note = "Stopped: " + reason + ".";
    }
    busy_.store(false);
}
