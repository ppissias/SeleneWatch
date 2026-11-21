#include "auto_tune.h"

#include "sky_exposure.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

// One stored sample as a number, and the largest one the frame can hold.
struct Samples
{
    const Frame& frame;
    int shift{0};
    double maxCode{255};
    explicit Samples(const Frame& f) : frame(f)
    {
        if (f.format == PixelFormat::Mono16)
        {
            shift = f.alignment == BitAlignment::High ? 16 - f.significantBits : 0;
            maxCode = (1 << f.significantBits) - 1;
        }
    }
    [[nodiscard]] double at(size_t i) const
    {
        const uint8_t* data = frame.data.data();
        switch (frame.format)
        {
        case PixelFormat::Mono8: return data[i];
        case PixelFormat::Mono16: return (data[2 * i] | (data[2 * i + 1] << 8)) >> shift;
        case PixelFormat::RGB888: return (data[3 * i] + data[3 * i + 1] + data[3 * i + 2]) / 3.0;
        }
        return 0;
    }
};

bool frameIsWhole(const Frame& frame)
{
    return frame.width > 0 && frame.height > 0
        && frame.data.size() == frameBytes(frame.width, frame.height, frame.format);
}

double percentile(std::vector<double>& values, double share)
{
    const auto at = values.begin() + static_cast<ptrdiff_t>(std::min(values.size() - 1,
        static_cast<size_t>(share * static_cast<double>(values.size()))));
    std::nth_element(values.begin(), at, values.end());
    return *at;
}

std::string percent(double share)
{
    return std::format("{:.1f} %", 100 * share);
}

std::string milliseconds(double microseconds)
{
    return std::format("{:g} ms", std::round(microseconds / 10) / 100);
}

std::string number(double value)
{
    return std::format("{:g}", std::round(value * 100) / 100);
}

} // namespace

TuneWorkflow tuneWorkflow(Target target, const DetectionSettings& settings)
{
    if (target == Target::Sky) return TuneWorkflow::Sky;
    return settings.darkening ? TuneWorkflow::MoonTransits : TuneWorkflow::MoonFlashes;
}

TuneAim tuneAim(TuneWorkflow workflow, double skyMaxExposureUs)
{
    switch (workflow)
    {
    case TuneWorkflow::MoonFlashes:
        // Earthshine at 3-10 %; a flash lasts 15-100 ms, so a longer
        // exposure adds background but no flash light.
        return {0.03, 0.10, 0.06, 10'000, 40'000, false};
    case TuneWorkflow::MoonTransits:
        // The brightest sunlit rims at 60-80 %; 10 ms keeps a 0.5 s ISS pass sharp.
        return {0.60, 0.80, 0.70, 0, 10'000, true};
    case TuneWorkflow::Sky:
    {
        // The exposure follower's own band, so the two cannot disagree.
        const SkyExposureRule rule;
        return {rule.low, rule.high, rule.aim, 0, skyMaxExposureUs, false};
    }
    }
    return {};
}

bool TuneArea::fits(const Frame& frame) const
{
    return inside.empty() || (width == frame.width && height == frame.height);
}

TuneArea skyArea(const DetectionMask& mask, int width, int height)
{
    TuneArea area;
    area.width = width;
    area.height = height;
    if (mask.empty() || width <= 0 || height <= 0) return area;
    std::vector<uint8_t> masked(static_cast<size_t>(width) * height, 0);
    mask.fill(width, height, masked.data());
    area.inside.resize(masked.size());
    std::transform(masked.begin(), masked.end(), area.inside.begin(), [](uint8_t m) { return uint8_t(m ? 0 : 1); });
    return area;
}

std::optional<BrightnessSample> measureBrightness(const Frame& frame, const TuneArea& area, bool brightest)
{
    if (!frameIsWhole(frame) || !area.fits(frame)) return std::nullopt;
    const Samples samples(frame);
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    const size_t inside = area.wholeFrame() ? pixels
        : static_cast<size_t>(std::count_if(area.inside.begin(), area.inside.end(), [](uint8_t v) { return v != 0; }));
    if (inside == 0) return std::nullopt;
    // At most about 16384 samples, spread evenly over the area.
    const size_t step = std::max(size_t{1}, inside / 16384);
    std::vector<double> values;
    values.reserve(std::min(inside, size_t{16384}) + 1);
    size_t seen = 0, atZero = 0, atMax = 0;
    for (size_t i = 0; i < pixels; ++i)
    {
        if (!area.wholeFrame() && !area.inside[i]) continue;
        if (seen++ % step) continue;
        const double value = samples.at(i);
        values.push_back(value);
        if (value <= 0) ++atZero;
        if (value >= samples.maxCode) ++atMax;
    }
    BrightnessSample result;
    result.samples = values.size();
    result.atZero = static_cast<double>(atZero) / static_cast<double>(values.size());
    result.atMax = static_cast<double>(atMax) / static_cast<double>(values.size());
    result.level = percentile(values, brightest ? 0.999 : 0.5) / samples.maxCode;
    return result;
}

TuneArea EarthshineArea::pixels(int width, int height) const
{
    TuneArea area;
    area.width = width;
    area.height = height;
    area.inside.assign(static_cast<size_t>(width) * height, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const int column = x / kCell, row = y / kCell;
            if (column < columns && row < rows && cells[static_cast<size_t>(row) * columns + column])
                area.inside[static_cast<size_t>(y) * width + x] = 1;
        }
    return area;
}

EarthshineArea findEarthshine(const Frame& frame, const PixelBox& box)
{
    EarthshineArea area;
    if (!frameIsWhole(frame)) { area.problem = "There is no frame to measure."; area.boxOnly = true; return area; }
    const Samples samples(frame);
    const int k = EarthshineArea::kCell;
    area.columns = (frame.width + k - 1) / k;
    area.rows = (frame.height + k - 1) / k;
    const size_t cellTotal = static_cast<size_t>(area.columns) * area.rows;
    area.cells.assign(cellTotal, 0);

    // Each cell's mean, and whether any of its pixels is saturated.
    std::vector<double> sum(cellTotal, 0);
    std::vector<int> count(cellTotal, 0);
    std::vector<uint8_t> saturated(cellTotal, 0);
    for (int y = 0; y < frame.height; ++y)
        for (int x = 0; x < frame.width; ++x)
        {
            const size_t cell = static_cast<size_t>(y / k) * area.columns + x / k;
            const double value = samples.at(static_cast<size_t>(y) * frame.width + x);
            sum[cell] += value;
            ++count[cell];
            if (value >= samples.maxCode) saturated[cell] = 1;
        }
    std::vector<double> mean(cellTotal, 0);
    for (size_t i = 0; i < cellTotal; ++i) mean[i] = count[i] ? sum[i] / count[i] : 0;

    // The box's cells, clamped to the frame.
    const int x0 = std::clamp(box.x, 0, frame.width - 1) / k, y0 = std::clamp(box.y, 0, frame.height - 1) / k;
    const int x1 = std::clamp(box.x + box.width - 1, 0, frame.width - 1) / k;
    const int y1 = std::clamp(box.y + box.height - 1, 0, frame.height - 1) / k;
    std::vector<size_t> boxCells;
    std::vector<double> boxMeans;
    for (int row = y0; row <= y1; ++row)
        for (int column = x0; column <= x1; ++column)
        {
            const size_t cell = static_cast<size_t>(row) * area.columns + column;
            if (saturated[cell]) continue;
            boxCells.push_back(cell);
            boxMeans.push_back(mean[cell]);
        }
    const auto useBox = [&](std::string problem) {
        area.boxOnly = true;
        area.problem = std::move(problem);
        std::fill(area.cells.begin(), area.cells.end(), uint8_t{0});
        for (size_t cell : boxCells) area.cells[cell] = 1;
        area.cellCount = static_cast<int>(boxCells.size());
        area.shareOfFrame = static_cast<double>(area.cellCount) / static_cast<double>(cellTotal);
        return area;
    };
    if (boxCells.empty())
    {
        area.problem = "The box is on the saturated sunlit side: draw it on the earthshine.";
        area.boxOnly = true;
        return area;
    }
    area.boxLevel = percentile(boxMeans, 0.5) / samples.maxCode;

    std::vector<double> unsaturated;
    for (size_t i = 0; i < cellTotal; ++i)
        if (!saturated[i]) unsaturated.push_back(mean[i]);
    const double boxMean = area.boxLevel * samples.maxCode;
    const double sky = percentile(unsaturated, 0.02);
    area.skyLevel = sky / samples.maxCode;
    const double low = sky + 0.5 * (boxMean - sky);
    const double high = sky + 3.0 * (boxMean - sky);
    // The sky beyond the Moon's edge is much darker than the earthshine and
    // reaches the frame's border. Without it (a tight crop) the darkest
    // cells are lunar terrain, a mare, and the area cannot be grown.
    bool skyAtBorder = false;
    for (int column = 0; column < area.columns && !skyAtBorder; ++column)
        for (const int row : {0, area.rows - 1})
            skyAtBorder |= !saturated[static_cast<size_t>(row) * area.columns + column]
                && mean[static_cast<size_t>(row) * area.columns + column] < low;
    for (int row = 0; row < area.rows && !skyAtBorder; ++row)
        for (const int column : {0, area.columns - 1})
            skyAtBorder |= !saturated[static_cast<size_t>(row) * area.columns + column]
                && mean[static_cast<size_t>(row) * area.columns + column] < low;
    if (!(sky < 0.6 * boxMean) || !skyAtBorder)
        return useBox("No darker sky beyond the Moon's edge is in view: measuring the box alone.");
    const auto earthshine = [&](size_t cell) { return !saturated[cell] && mean[cell] >= low && mean[cell] <= high; };
    std::vector<size_t> pending;
    for (size_t cell : boxCells)
        if (earthshine(cell) && !area.cells[cell]) { area.cells[cell] = 1; pending.push_back(cell); }
    while (!pending.empty())
    {
        const size_t cell = pending.back();
        pending.pop_back();
        const int column = static_cast<int>(cell % area.columns), row = static_cast<int>(cell / area.columns);
        const int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
        for (int n = 0; n < 4; ++n)
        {
            const int c = column + dx[n], r = row + dy[n];
            if (c < 0 || r < 0 || c >= area.columns || r >= area.rows) continue;
            const size_t next = static_cast<size_t>(r) * area.columns + c;
            if (!area.cells[next] && earthshine(next)) { area.cells[next] = 1; pending.push_back(next); }
        }
    }
    area.cellCount = static_cast<int>(std::count(area.cells.begin(), area.cells.end(), uint8_t{1}));
    area.shareOfFrame = static_cast<double>(area.cellCount) / static_cast<double>(cellTotal);
    if (area.cellCount < static_cast<int>(boxCells.size()))
        return useBox("The earthshine could not be told from its surroundings: measuring the box alone.");
    if (area.shareOfFrame > 0.9)
        return useBox("The area would cover nearly the whole frame: measuring the box alone.");
    return area;
}

CameraSuggestion suggestCamera(const CameraTuneInput& in)
{
    CameraSuggestion out;
    const TuneAim aim = tuneAim(in.workflow, in.skyMaxExposureUs);
    const char* what = in.workflow == TuneWorkflow::MoonFlashes ? "The earthshine"
        : in.workflow == TuneWorkflow::MoonTransits ? "The brightest sunlit areas" : "The sky";
    if (!in.measured)
    {
        out.reachable = false;
        out.verdict = "No samples to measure: the mask covers the whole frame. No brightness suggestion is made.";
        return out;
    }
    const BrightnessSample& m = *in.measured;
    // Saturated sunlit pixels hide how bright they really are.
    const bool saturated = aim.brightest && m.atMax > 1e-4;
    out.inAim = !saturated && m.level >= aim.low && m.level <= aim.high;
    out.verdict = std::format("{} is at {} of full scale; the aim is {:.0f}-{:.0f} %.", what, percent(m.level),
        100 * aim.low, 100 * aim.high);
    if (saturated) out.verdict += std::format(" {} of it is saturated.", percent(m.atMax));
    if (out.inAim) out.verdict += " It is in the aim.";
    if (m.atZero > 0.001)
        out.warnings.push_back(std::format("{} of the measured pixels are at 0: raise the Offset. A pixel at 0 cannot get "
            "darker, so its noise is under-estimated.", percent(m.atZero)));
    if (in.workflow == TuneWorkflow::MoonFlashes && m.atMax > 0.01)
        out.warnings.push_back(std::format("{} of the earthshine area is saturated: draw the box away from the sunlit side.",
            percent(m.atMax)));

    if (!in.camera.exposure)
    {
        out.reachable = false;
        out.verdict += " Not a camera: only the sensitivity (K) can be tuned.";
        return out;
    }
    const TuneControl exposure = *in.camera.exposure;
    const double lowest = std::max(exposure.minimum, aim.minExposureUs);
    const double longest = std::min(exposure.maximum, aim.maxExposureUs > 0 ? aim.maxExposureUs : exposure.maximum);
    double newExposure = std::clamp(exposure.value, std::min(lowest, longest), longest);

    // The level at another exposure: through the black level when two
    // measurements at the same gain give it, else in proportion.
    const double gainNow = in.camera.gain ? in.camera.gain->value : 0;
    double pedestal = 0, slope = m.level / std::max(exposure.value, 1.0);
    bool twoPoint = false;
    if (in.previous && std::abs(in.previous->gain - gainNow) < 1e-9
        && std::abs(in.previous->exposureUs - exposure.value) > 0.01 * exposure.value)
    {
        const double s = (m.level - in.previous->brightness.level) / (exposure.value - in.previous->exposureUs);
        const double p = m.level - s * exposure.value;
        if (s > 0 && p >= 0 && p < std::min(m.level, in.previous->brightness.level))
        {
            pedestal = p;
            slope = s;
            twoPoint = true;
        }
    }
    const auto predict = [&](double us) { return pedestal + slope * us; };
    const auto exposureFor = [&](double level, double from) {
        double wanted = (level - pedestal) / slope;
        if (!twoPoint) wanted = std::clamp(wanted, from / 4, from * 4); // A bounded trial step.
        return std::clamp(wanted, std::min(lowest, longest), longest);
    };

    double predicted = saturated ? 1.0 : predict(newExposure);
    double newGain = gainNow;
    const bool hasGain = in.camera.gain && in.camera.gain->maximum > in.camera.gain->minimum;
    const double gainRange = hasGain ? in.camera.gain->maximum - in.camera.gain->minimum : 0;
    double gainStep = in.followGainStep > 0 ? in.followGainStep : gainRange / 20;
    if (gainRange >= 20) gainStep = std::max(1.0, std::round(gainStep));
    const auto gainSteps = [&](double ratio) {
        const double stops = std::abs(std::log2(std::max(ratio, 1e-6)));
        const int count = stops <= 1 ? 1 : stops <= 2 ? 2 : 3;
        return std::min(count * gainStep, gainRange / 4);
    };
    const auto raiseGain = [&](double ratio) {
        if (!hasGain || newGain >= in.camera.gain->maximum) return false;
        newGain = std::min(in.camera.gain->maximum, newGain + gainSteps(ratio));
        return true;
    };
    const auto lowerGain = [&](double ratio) {
        if (!hasGain || newGain <= in.camera.gain->minimum) return false;
        newGain = std::max(in.camera.gain->minimum, newGain - gainSteps(ratio));
        return true;
    };

    const bool tooBright = saturated || predicted > aim.high;
    const bool tooDark = !tooBright && predicted < aim.low;
    const double ratio = saturated ? 0.5 : aim.aim / std::max(predicted, 1e-6);
    bool moved = newExposure != exposure.value;
    if (in.workflow == TuneWorkflow::MoonFlashes)
    {
        // Gain first: the exposure stays where flashes are caught.
        if (tooDark && !raiseGain(ratio) && newExposure < longest) { newExposure = exposureFor(aim.aim, newExposure); moved = true; }
        if (tooBright && !lowerGain(ratio) && newExposure > lowest) { newExposure = exposureFor(aim.aim, newExposure); moved = true; }
    }
    else if (tooDark)
    {
        // A darker scene: a longer exposure first, then more gain.
        if (newExposure < longest)
        {
            newExposure = exposureFor(aim.aim, newExposure);
            moved = true;
            if (predict(newExposure) < aim.low && newExposure >= longest) raiseGain(aim.aim / std::max(predict(newExposure), 1e-6));
        }
        else raiseGain(ratio);
    }
    else if (tooBright)
    {
        // A brighter scene: less gain first, then a shorter exposure.
        if (!lowerGain(ratio) && newExposure > lowest)
        {
            newExposure = saturated ? std::max(std::min(lowest, longest), newExposure / 2) : exposureFor(aim.aim, newExposure);
            moved = true;
        }
    }
    const bool changed = moved || newGain != gainNow;
    if ((tooDark || tooBright) && !changed)
    {
        out.reachable = false;
        out.verdict += std::format(" The camera cannot reach the aim: the exposure is at its limit ({}) and the gain {}.",
            milliseconds(newExposure), hasGain ? (tooDark ? "at its highest" : "at its lowest") : "cannot change");
    }

    const char* exposureWhy = in.workflow == TuneWorkflow::MoonFlashes ? "Kept between 10 and 40 ms: flashes last 15-100 ms"
        : in.workflow == TuneWorkflow::MoonTransits ? "At most 10 ms, so a fast transit stays sharp"
        : "Longer first for a darker sky, up to the longest exposure for meteors";
    TuneRow exposureRow{TuneRow::Kind::Exposure, "Exposure", milliseconds(exposure.value), milliseconds(newExposure),
        newExposure != exposure.value ? exposureWhy : "Kept", newExposure, 0, newExposure != exposure.value};
    if (twoPoint && newExposure != exposure.value) exposureRow.why += " (the black level was measured)";
    out.rows.push_back(exposureRow);
    if (in.camera.gain)
    {
        TuneRow gainRow{TuneRow::Kind::Gain, "Gain", number(gainNow), number(newGain),
            newGain == gainNow ? "Kept" : std::string(newGain > gainNow ? "Raised" : "Lowered")
                + " by a bounded step; gain changes are checked by measuring again", newGain, 0, newGain != gainNow};
        out.rows.push_back(gainRow);
    }
    if (m.atZero > 0.001 && in.camera.offset)
    {
        const TuneControl offset = *in.camera.offset;
        const double value = std::min(offset.maximum, offset.value + std::max(1.0, std::round((offset.maximum - offset.minimum) / 32)));
        if (value > offset.value)
            out.rows.push_back({TuneRow::Kind::Offset, "Offset", number(offset.value), number(value),
                "Pixels at 0: a higher black level keeps them above 0", value, 0, true});
    }
    if (in.workflow == TuneWorkflow::Sky && in.follow)
    {
        const double from = std::max(exposure.minimum, 100.0);
        const double to = std::max(from, std::min(exposure.maximum, in.skyMaxExposureUs));
        out.rows.push_back({TuneRow::Kind::FollowExposure, "Follower: exposure",
            std::format("{} to {} ms", number(in.followFromMs), number(in.followToMs)),
            std::format("{} to {} ms", number(from / 1000), number(to / 1000)),
            "From the camera's shortest, for a bright dusk, to the longest for meteors", from / 1000, to / 1000, true});
        if (hasGain)
        {
            const double upTo = std::min(in.camera.gain->maximum, newGain + gainRange / 4);
            const double step = std::max(1.0, std::round(gainRange / 20));
            out.rows.push_back({TuneRow::Kind::FollowGain, "Follower: gain",
                in.followGainStep > 0 ? std::format("up to {} in steps of {}", number(in.followMaxGain), number(in.followGainStep))
                    : std::string("fixed"),
                std::format("up to {} in steps of {}", number(upTo), number(step)),
                "Room for a dark sky once the exposure is at its longest; a quarter of the camera's range at most",
                upTo, step, true});
        }
    }
    return out;
}

std::vector<double> kValuesToTest(double centre, int count)
{
    std::vector<double> values;
    for (const double k : {centre, centre + 1, centre - 1, centre + 2})
    {
        if (static_cast<int>(values.size()) >= count) break;
        if (k != centre && k < kMinimumK) continue;
        if (std::find(values.begin(), values.end(), k) == values.end()) values.push_back(k);
    }
    return values;
}

std::vector<TuneEventGroup> groupEvents(const std::vector<KRowResult>& rows)
{
    std::vector<TuneEventGroup> groups;
    const auto touching = [](const TuneEvent& a, const TuneEvent& b) {
        return a.change == b.change && (a.frameIndex > b.frameIndex ? a.frameIndex - b.frameIndex : b.frameIndex - a.frameIndex) <= 1
            && a.x <= b.x + b.width && b.x <= a.x + a.width && a.y <= b.y + b.height && b.y <= a.y + a.height;
    };
    for (size_t row = 0; row < rows.size(); ++row)
        for (size_t index = 0; index < rows[row].kept.size(); ++index)
        {
            const TuneEvent& event = rows[row].kept[index];
            auto found = std::find_if(groups.begin(), groups.end(), [&](const TuneEventGroup& group) {
                if (!touching(group.first, event)) return false;
                return std::none_of(group.members.begin(), group.members.end(), [&](const auto& m) { return m.first == row; });
            });
            if (found == groups.end()) groups.push_back({event, {{row, index}}});
            else found->members.emplace_back(row, index);
        }
    std::sort(groups.begin(), groups.end(), [](const TuneEventGroup& a, const TuneEventGroup& b) {
        return a.first.frameIndex < b.first.frameIndex;
    });
    return groups;
}

uint64_t remainingEvents(const std::vector<KRowResult>& rows, size_t row,
    const std::vector<TuneEventGroup>& groups, const std::vector<bool>& excluded)
{
    uint64_t removed = 0;
    for (size_t g = 0; g < groups.size() && g < excluded.size(); ++g)
        if (excluded[g])
            removed += static_cast<uint64_t>(std::count_if(groups[g].members.begin(), groups[g].members.end(),
                [row](const auto& m) { return m.first == row; }));
    return rows[row].events - std::min(rows[row].events, removed);
}

KChoice chooseK(const std::vector<KRowResult>& rows, const std::vector<TuneEventGroup>& groups,
    const std::vector<bool>& excluded, double watchSeconds)
{
    std::vector<size_t> order(rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rows[a].k < rows[b].k; });
    bool anyEligible = false;
    for (size_t i : order)
    {
        const KRowResult& row = rows[i];
        if (row.frames == 0 || row.seconds < 0.8 * watchSeconds || row.limitedFrames > 0 || !row.stopped.empty()) continue;
        anyEligible = true;
        if (remainingEvents(rows, i, groups, excluded) == 0) return {row.k, {}};
    }
    if (!anyEligible)
        return {std::nullopt, "No K value was measured over the whole watch without a skipped patch search, "
            "so no quiet K can be recommended. Watch again, or test higher K values."};
    return {std::nullopt, "Every K value tested had events. Exclude any that were real, or test higher K values."};
}

FlashWidth flashWidth(double seeingArcsec, std::optional<double> arcsecPerPixel, double fallbackPixels)
{
    if (arcsecPerPixel && *arcsecPerPixel > 0 && seeingArcsec > 0) return {seeingArcsec / *arcsecPerPixel, true};
    return {fallbackPixels, false};
}

std::vector<int> minPatchValuesToTest(int current)
{
    std::vector<int> values;
    for (const double share : {1.0, 2.0 / 3, 1.0 / 2, 1.0 / 3})
    {
        const int value = std::max(kMinimumPatch, static_cast<int>(std::lround(current * share)));
        if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
    }
    return values;
}

MinPatchChoice chooseMinPatch(const std::vector<KRowResult>& rows, const std::vector<TuneEventGroup>& groups,
    const std::vector<bool>& excluded, double watchSeconds, std::optional<int> footprint)
{
    std::vector<size_t> order(rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rows[a].minPixels < rows[b].minPixels; });
    std::optional<int> quiet;
    bool anyEligible = false;
    for (size_t i : order)
    {
        const KRowResult& row = rows[i];
        if (row.frames == 0 || row.seconds < 0.8 * watchSeconds || row.limitedFrames > 0 || !row.stopped.empty()) continue;
        anyEligible = true;
        if (remainingEvents(rows, i, groups, excluded) == 0) { quiet = row.minPixels; break; }
    }
    if (!anyEligible)
        return {std::nullopt, false, "No minimum patch was measured over the whole watch without a skipped patch search. "
            "Watch again."};
    if (!quiet)
        return {std::nullopt, false, "Every minimum patch tested had events. Exclude any that were real, or raise K first."};
    if (footprint && *quiet > *footprint)
        return {quiet, true, std::format("Noise needs a minimum patch of {} px, but a just-detectable flash covers only {} px: "
            "flashes that faint cannot be told from noise at this K.", *quiet, *footprint)};
    return {quiet, false, footprint ? std::format("No events left in {:.0f} s, and a just-detectable flash covers {} px.",
        watchSeconds, *footprint) : std::format("No events left in {:.0f} s. The flash footprint was not measured.", watchSeconds)};
}