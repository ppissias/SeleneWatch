#include "recording/clip_overview.h"

#include "display/preview_image.h"

#include <QtGui/QFont>
#include <QtGui/QFontMetrics>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

constexpr int kMaxMainWidth = 1280;
constexpr int kMinMainWidth = 640;
constexpr int kStripSlots = 6;
constexpr int kGap = 8;
constexpr double kLearningSeconds = 1.0;
// Active frames this close together belong to one run of activity, when
// their activity is within kLinkPixels (or four times its spread) of the
// frame before.
constexpr double kRunGapSeconds = 0.5;
constexpr double kLinkPixels = 48;
// A frame's centre and spread are measured within this distance of its
// strongest change, so a stray pixel elsewhere does not pull them.
constexpr int kWindowRadius = 128;
constexpr int kMinCrop = 32;
// A trigger mark is at least this many picture pixels across, with ticks
// this long, so it can be found in a thumbnail.
constexpr int kMinMark = 28;
constexpr int kTick = 14;
// Activity that moves less than this stays in place.
constexpr double kMinMovePixels = 3;
// Samples taken to set the strip's levels.
constexpr size_t kLevelSamples = 65536;

const QColor kRise(255, 140, 0);
const QColor kFall(40, 150, 255);
const QColor kMark(255, 220, 0);
const QColor kBackground(20, 20, 24);
const QColor kText(230, 230, 230);
const QColor kDimText(160, 160, 170);

// One value per pixel, as the detector reads it: Mono16 at its significant
// bits, RGB as the mean of its channels, a Bayer mosaic as it is.
float sampleAt(const Frame& frame, size_t i, int shift)
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

int shiftOf(const Frame& frame)
{
    return frame.format == PixelFormat::Mono16 && frame.alignment == BitAlignment::High ? 16 - frame.significantBits : 0;
}

bool sameLayout(const Frame& a, const Frame& b)
{
    return a.width == b.width && a.height == b.height && a.format == b.format && a.significantBits == b.significantBits
        && a.alignment == b.alignment && a.bayer == b.bayer && a.data.size() == b.data.size();
}

// A value for the strip. A Bayer mosaic is averaged over its 2x2 cell, so
// the filter pattern does not show when the crop is enlarged.
float displaySample(const Frame& frame, int x, int y, int shift)
{
    if (frame.bayer == BayerPattern::None || frame.format == PixelFormat::RGB888)
        return sampleAt(frame, static_cast<size_t>(y) * frame.width + x, shift);
    const int x0 = std::min(x & ~1, frame.width - 2), y0 = std::min(y & ~1, frame.height - 2);
    if (x0 < 0 || y0 < 0) return sampleAt(frame, static_cast<size_t>(y) * frame.width + x, shift);
    float sum = 0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) sum += sampleAt(frame, static_cast<size_t>(y0 + dy) * frame.width + x0 + dx, shift);
    return sum / 4;
}

float median(BudgetVector<float>& values)
{
    const auto middle = values.begin() + static_cast<ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

double seconds(std::chrono::system_clock::duration duration)
{
    return std::chrono::duration<double>(duration).count();
}

// The display's midtones transfer: m = 0.5 is linear.
double midtones(double m, double value)
{
    if (value <= 0) return 0;
    if (value >= 1) return 1;
    return (m - 1) * value / ((2 * m - 1) * value - m);
}

// What one frame showed.
struct FrameActivity
{
    uint64_t index{0};
    std::chrono::system_clock::time_point time{};
    // Learning frames are not measured.
    bool learning{true};
    // Pixels that changed by K.
    int changed{0};
    // The strongest change, signed, in units of the noise, and where it is.
    double peak{0};
    int peakX{0}, peakY{0};
    // Centre and spread (root mean square distance per axis) of the changed
    // pixels near the strongest one, weighted by their change.
    double centreX{0}, centreY{0}, spread{0};
};

// What the whole clip showed.
struct Activity
{
    int width{0}, height{0};
    // The largest rise and the largest fall of each pixel over the clip, in
    // units of its noise. Zero where it never changed by K.
    BudgetVector<float> rise, fall;
    std::vector<FrameActivity> frames;
    // Time from the first frame to the first measured one.
    double learnedSeconds{0};
};

Activity measure(const std::vector<FramePtr>& frames, const OverviewRequest& request, const MemoryBudget& budget)
{
    const Frame& first = *frames.front();
    const size_t pixels = static_cast<size_t>(first.width) * first.height;
    const int shift = shiftOf(first);
    Activity activity{first.width, first.height,
        BudgetVector<float>(BudgetAllocator<float>(budget)), BudgetVector<float>(BudgetAllocator<float>(budget)), {}, 0};
    activity.rise.assign(pixels, 0.0f);
    activity.fall.assign(pixels, 0.0f);
    BudgetVector<float> mean(pixels, 0.0f, BudgetAllocator<float>(budget));
    BudgetVector<float> variance(pixels, 0.0f, BudgetAllocator<float>(budget));
    BudgetVector<float> grid{BudgetAllocator<float>(budget)};
    grid.reserve(4096);
    // The current frame's changed pixels, for its centre and spread.
    BudgetVector<size_t> changedAt{BudgetAllocator<size_t>(budget)};
    BudgetVector<float> changedSigma{BudgetAllocator<float>(budget)};
    // The detector's mask: those pixels are neither measured nor in the noise floor.
    BudgetVector<uint8_t> masked{BudgetAllocator<uint8_t>(budget)};
    if (!request.mask.empty())
    {
        masked.assign(pixels, 0);
        request.mask.fill(first.width, first.height, masked.data());
    }
    const auto isMasked = [&masked](size_t i) { return !masked.empty() && masked[i]; };

    for (size_t i = 0; i < pixels; ++i) mean[i] = sampleAt(first, i, shift);
    size_t learned = 1;
    const auto learningEnd = first.timestamp
        + std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::duration<double>(kLearningSeconds));
    auto previous = first.timestamp;
    activity.frames.push_back({first.frameIndex, first.timestamp});
    const float k = static_cast<float>(request.sigma);
    bool measuring = false;

    for (size_t n = 1; n < frames.size(); ++n)
    {
        const Frame& frame = *frames[n];
        FrameActivity record{frame.frameIndex, frame.timestamp};
        if (!sameLayout(frame, first))
        {
            activity.frames.push_back(record);
            continue;
        }
        const bool atOnset = request.onsetFrameIndex && frame.frameIndex >= *request.onsetFrameIndex;
        record.learning = frame.timestamp < learningEnd && !atOnset;
        if (!record.learning && !measuring)
        {
            measuring = true;
            activity.learnedSeconds = seconds(frame.timestamp - first.timestamp);
        }
        double interval = seconds(frame.timestamp - previous);
        if (!(interval > 0) || !std::isfinite(interval)) interval = 1.0 / 30;
        interval = std::min(interval, 10.0);
        previous = frame.timestamp;

        // The detector's noise floor: a robust scale of the difference on a
        // bounded grid, with the median difference removed.
        grid.clear();
        const size_t step = std::max(size_t{1}, (pixels + 4095) / 4096);
        for (size_t i = 0; i < pixels; i += step)
            if (!isMasked(i)) grid.push_back(sampleAt(frame, i, shift) - mean[i]);
        if (grid.empty())
            for (size_t i = 0; i < pixels; i += step) grid.push_back(sampleAt(frame, i, shift) - mean[i]);
        const float centre = median(grid);
        for (float& value : grid) value = std::abs(value - centre);
        const double floor = std::max(request.minNoiseAdu, 1.4826 * median(grid));
        const float floorVariance = static_cast<float>(floor * floor);
        // The detector's learning rate.
        const float alpha = static_cast<float>(std::max(1.0 / static_cast<double>(learned + 1),
            -std::expm1(-interval / request.referenceTimeConstantSeconds)));

        changedAt.clear();
        changedSigma.clear();
        float strongest = 0;
        for (size_t i = 0; i < pixels; ++i)
        {
            const float delta = sampleAt(frame, i, shift) - mean[i];
            bool hold = false;
            if (!record.learning && !isMasked(i))
            {
                const float noiseVariance = std::max(variance[i], floorVariance);
                if (delta * delta >= k * k * noiseVariance)
                {
                    // A changed pixel is held from learning, as the detector
                    // holds a firing patch: a slow light would otherwise raise
                    // its own noise and fade within a few frames.
                    hold = true;
                    const float sigma = delta / std::sqrt(noiseVariance);
                    if (sigma > 0) activity.rise[i] = std::max(activity.rise[i], sigma);
                    else activity.fall[i] = std::max(activity.fall[i], -sigma);
                    changedAt.push_back(i);
                    changedSigma.push_back(sigma);
                    if (std::abs(sigma) > strongest)
                    {
                        strongest = std::abs(sigma);
                        record.peak = sigma;
                        record.peakX = static_cast<int>(i % static_cast<size_t>(first.width));
                        record.peakY = static_cast<int>(i / static_cast<size_t>(first.width));
                    }
                }
            }
            if (hold) continue;
            mean[i] += alpha * delta;
            variance[i] = (1 - alpha) * (variance[i] + alpha * delta * delta);
        }
        if (learned < 1'000'000) ++learned;

        record.changed = static_cast<int>(changedAt.size());
        double weight = 0, sumX = 0, sumY = 0, sumXX = 0, sumYY = 0;
        for (size_t c = 0; c < changedAt.size(); ++c)
        {
            const double x = static_cast<double>(changedAt[c] % static_cast<size_t>(first.width));
            const double y = static_cast<double>(changedAt[c] / static_cast<size_t>(first.width));
            if (std::abs(x - record.peakX) > kWindowRadius || std::abs(y - record.peakY) > kWindowRadius) continue;
            const double w = std::abs(changedSigma[c]);
            weight += w; sumX += w * x; sumY += w * y; sumXX += w * x * x; sumYY += w * y * y;
        }
        if (weight > 0)
        {
            record.centreX = sumX / weight;
            record.centreY = sumY / weight;
            const double varianceX = std::max(0.0, sumXX / weight - record.centreX * record.centreX);
            const double varianceY = std::max(0.0, sumYY / weight - record.centreY * record.centreY);
            record.spread = std::sqrt((varianceX + varianceY) / 2);
        }
        activity.frames.push_back(record);
    }
    if (!measuring) activity.learnedSeconds = seconds(frames.back()->timestamp - first.timestamp);
    return activity;
}

bool isActive(const FrameActivity& frame, int minPixels)
{
    return !frame.learning && frame.changed >= std::max(1, minPixels);
}

std::optional<size_t> positionOf(const Activity& activity, uint64_t index)
{
    for (size_t p = 0; p < activity.frames.size(); ++p)
        if (activity.frames[p].index == index) return p;
    return {};
}

// The detector's peak frame, else the frame with the strongest change, else
// the onset frame, else the last frame.
size_t mainPosition(const Activity& activity, const OverviewRequest& request)
{
    if (request.peakFrameIndex)
        if (const auto position = positionOf(activity, *request.peakFrameIndex)) return *position;
    std::optional<size_t> best;
    for (size_t p = 0; p < activity.frames.size(); ++p)
        if (isActive(activity.frames[p], request.minPixels)
            && (!best || std::abs(activity.frames[p].peak) > std::abs(activity.frames[*best].peak))) best = p;
    if (best) return *best;
    if (request.onsetFrameIndex)
        if (const auto position = positionOf(activity, *request.onsetFrameIndex)) return *position;
    return activity.frames.size() - 1;
}

// Whether activity on two frames is one thing: near enough to have moved
// from one place to the other. Seeing lights a different edge on each frame;
// those are not linked.
bool linked(const FrameActivity& a, const FrameActivity& b)
{
    const double reach = std::max<double>(kLinkPixels, 4 * std::max(a.spread, b.spread));
    return std::hypot(a.centreX - b.centreX, a.centreY - b.centreY) <= reach;
}

// The run of activity around the main frame, in clip order: linked active
// frames no more than kRunGapSeconds apart. Activity elsewhere in between is
// passed over. Empty when no activity is that close to the main frame.
std::vector<size_t> runAround(const Activity& activity, size_t main, int minPixels)
{
    const auto& frames = activity.frames;
    std::optional<size_t> seed;
    if (isActive(frames[main], minPixels)) seed = main;
    else
        for (size_t p = 0; p < frames.size(); ++p)
        {
            if (!isActive(frames[p], minPixels)) continue;
            const double distance = std::abs(seconds(frames[p].time - frames[main].time));
            if (distance > kRunGapSeconds) continue;
            if (!seed || distance < std::abs(seconds(frames[*seed].time - frames[main].time))) seed = p;
        }
    if (!seed) return {};
    std::vector<size_t> run{*seed};
    for (size_t p = *seed; p-- > 0;)
    {
        if (seconds(frames[run.front()].time - frames[p].time) > kRunGapSeconds) break;
        if (isActive(frames[p], minPixels) && linked(frames[p], frames[run.front()])) run.insert(run.begin(), p);
    }
    for (size_t p = *seed + 1; p < frames.size(); ++p)
    {
        if (seconds(frames[p].time - frames[run.back()].time) > kRunGapSeconds) break;
        if (isActive(frames[p], minPixels) && linked(frames[p], frames[run.back()])) run.push_back(p);
    }
    return run;
}

// The colour for a pixel's change, and how strongly it covers the frame.
// False when the pixel did not change by K.
bool changeColour(float rise, float fall, double k, QColor& colour, double& opacity)
{
    if (rise <= 0 && fall <= 0) return false;
    const double strength = std::max(rise, fall);
    colour = rise >= fall ? kRise : kFall;
    opacity = 0.45 + 0.45 * std::min(1.0, (strength - k) / (2 * k));
    return true;
}

QRgb blend(QRgb base, const QColor& colour, double opacity)
{
    const auto mix = [opacity](int under, int over) {
        return static_cast<int>(std::lround(under * (1 - opacity) + over * opacity));
    };
    return qRgb(mix(qRed(base), colour.red()), mix(qGreen(base), colour.green()), mix(qBlue(base), colour.blue()));
}

// Colours the changed pixels over the picture area. Each picture pixel takes
// the strongest change in the block of frame pixels it covers, so a one-pixel
// flash survives a reduction.
void paintChange(QImage& canvas, const QRect& area, const Activity& activity, const QRect& source, double k, double dim = 1.0)
{
    for (int oy = 0; oy < area.height(); ++oy)
    {
        auto* row = reinterpret_cast<QRgb*>(canvas.scanLine(area.top() + oy));
        const int y0 = source.top() + oy * source.height() / area.height();
        const int y1 = std::max(y0 + 1, source.top() + (oy + 1) * source.height() / area.height());
        for (int ox = 0; ox < area.width(); ++ox)
        {
            const int x0 = source.left() + ox * source.width() / area.width();
            const int x1 = std::max(x0 + 1, source.left() + (ox + 1) * source.width() / area.width());
            float rise = 0, fall = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                {
                    const size_t i = static_cast<size_t>(y) * activity.width + x;
                    rise = std::max(rise, activity.rise[i]);
                    fall = std::max(fall, activity.fall[i]);
                }
            QRgb& pixel = row[area.left() + ox];
            if (dim < 1.0)
                pixel = qRgb(static_cast<int>(qRed(pixel) * dim), static_cast<int>(qGreen(pixel) * dim), static_cast<int>(qBlue(pixel) * dim));
            QColor colour;
            double opacity = 0;
            if (changeColour(rise, fall, k, colour, opacity)) pixel = blend(pixel, colour, opacity);
        }
    }
}

// Display levels shared by every crop of the strip, so their brightness can
// be compared. A dark crop is brightened until its median is at a quarter of
// the range; a bright one is left linear.
struct Levels
{
    double black{0}, white{1}, m{0.5};
};

Levels stripLevels(const std::vector<const Frame*>& frames, const std::vector<QRect>& crops, int shift, const MemoryBudget& budget)
{
    size_t total = 0;
    for (const QRect& crop : crops) total += static_cast<size_t>(crop.width()) * crop.height();
    const size_t step = std::max(size_t{1}, total / kLevelSamples);
    BudgetVector<float> values{BudgetAllocator<float>(budget)};
    values.reserve(std::min(total, kLevelSamples + crops.size()));
    size_t counter = 0;
    for (size_t c = 0; c < frames.size(); ++c)
        for (int y = crops[c].top(); y <= crops[c].bottom(); ++y)
            for (int x = crops[c].left(); x <= crops[c].right(); ++x)
                if (counter++ % step == 0) values.push_back(displaySample(*frames[c], x, y, shift));
    Levels levels;
    if (values.empty()) return levels;
    std::sort(values.begin(), values.end());
    levels.black = values[values.size() / 100];
    levels.white = values.back();
    if (levels.white <= levels.black) levels.white = levels.black + 1;
    const double centre = (values[values.size() / 2] - levels.black) / (levels.white - levels.black);
    if (centre > 0 && centre < 0.25) levels.m = std::clamp(midtones(0.25, centre), 0.000001, 0.999999);
    return levels;
}

// Draws one crop of a frame into the cell, enlarged without smoothing.
void paintCrop(QImage& canvas, const QRect& cell, const Frame& frame, const QRect& crop, const Levels& levels, int shift)
{
    for (int oy = 0; oy < cell.height(); ++oy)
    {
        auto* row = reinterpret_cast<QRgb*>(canvas.scanLine(cell.top() + oy));
        const int y = crop.top() + oy * crop.height() / cell.height();
        for (int ox = 0; ox < cell.width(); ++ox)
        {
            const int x = crop.left() + ox * crop.width() / cell.width();
            const double value = (displaySample(frame, x, y, shift) - levels.black) / (levels.white - levels.black);
            const int grey = static_cast<int>(std::lround(255 * midtones(levels.m, std::clamp(value, 0.0, 1.0))));
            row[cell.left() + ox] = qRgb(grey, grey, grey);
        }
    }
}

// A square crop of the given side around a centre, inside the frame.
QRect cropAround(double centreX, double centreY, int side, int width, int height)
{
    const int x = std::clamp(static_cast<int>(std::lround(centreX - side / 2.0)), 0, width - side);
    const int y = std::clamp(static_cast<int>(std::lround(centreY - side / 2.0)), 0, height - side);
    return {x, y, side, side};
}

QString signedSeconds(double value)
{
    return QString("%1%2 s").arg(value >= 0 ? "+" : "−").arg(std::abs(value), 0, 'f', 3);
}

// Draws a trigger mark: a box with a margin around the patch and four ticks
// pointing at it, yellow over a dark outline so it shows on any background.
void drawMark(QPainter& painter, const QRectF& patch, int number)
{
    const double width = std::max<double>(kMinMark, patch.width() + 12);
    const double height = std::max<double>(kMinMark, patch.height() + 12);
    const QRectF box(patch.center().x() - width / 2, patch.center().y() - height / 2, width, height);
    const QPointF centre = box.center();
    const QLineF ticks[] = {
        {centre.x(), box.top(), centre.x(), box.top() - kTick},
        {centre.x(), box.bottom(), centre.x(), box.bottom() + kTick},
        {box.left(), centre.y(), box.left() - kTick, centre.y()},
        {box.right(), centre.y(), box.right() + kTick, centre.y()},
    };
    for (const auto& [colour, penWidth] : {std::pair{QColor(0, 0, 0), 5}, std::pair{kMark, 3}})
    {
        painter.setPen(QPen(colour, penWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
        for (const QLineF& tick : ticks) painter.drawLine(tick);
    }
    if (number > 0)
    {
        const QPointF at(box.right() + 4, box.top() - 4);
        painter.setPen(QColor(0, 0, 0));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) painter.drawText(at + QPointF(dx, dy), QString::number(number));
        painter.setPen(kMark);
        painter.drawText(at, QString::number(number));
    }
}

// What the run of activity looked like, for the suggested kind (Feature-15).
struct RunShape
{
    bool any{false};
    bool rise{true};
    bool moved{false};
    int frames{0};
    double seconds{0};
    double pixelsPerSecond{0};
    // The centres stay near the line from the first to the last.
    bool straight{false};
    // The strength jumps up and down again at least twice: a strobe.
    bool blinking{false};
};

// The kind a clip probably shows, with the reason. The thresholds are first
// values: a meteor crosses at 3 degrees a second or more (a quarter of the
// frame's width a second without an image scale), a satellite more slowly.
std::pair<QString, QString> suggestKind(const RunShape& run, const OverviewRequest& request, int width, int height)
{
    if (request.largeBrightening)
        return {"lightning", "a large share of the frame brightened at once: lightning, or a fireball's flare"};
    if (request.marks.size() >= 8)
    {
        int left = width, top = height, right = 0, bottom = 0;
        for (const QRect& mark : request.marks)
        {
            left = std::min(left, mark.center().x()); right = std::max(right, mark.center().x());
            top = std::min(top, mark.center().y()); bottom = std::max(bottom, mark.center().y());
        }
        if (right - left >= width / 4 && bottom - top >= height / 4)
            return {"seeing", QString("%1 triggers scattered over the frame").arg(request.marks.size())};
    }
    if (!run.any) return {"unclear", "no activity was found"};
    const double degrees = request.arcsecPerPixel ? run.pixelsPerSecond * *request.arcsecPerPixel / 3600 : 0;
    const double widths = run.pixelsPerSecond / width;
    const QString speed = request.arcsecPerPixel ? QString("%1°/s").arg(degrees, 0, 'f', 2)
                                                 : QString("%1 frame widths/s").arg(widths, 0, 'f', 2);
    const QString time = QString("%1 s").arg(run.seconds, 0, 'f', run.seconds < 1 ? 3 : 1);
    if (!run.moved)
    {
        if (run.rise && run.seconds <= 0.2)
            return {"flash", QString("a rise in place, %1 %2 over %3").arg(run.frames).arg(run.frames == 1 ? "frame" : "frames", time)};
        return {"unclear", QString("a %1 in place for %2").arg(run.rise ? "rise" : "fall", time)};
    }
    if (!run.rise) return {"dark transit", QString("a fall moving %1 for %2").arg(speed, time)};
    const bool fast = request.arcsecPerPixel ? degrees >= 3 : widths >= 0.25;
    if (fast && run.straight && run.seconds <= 3) return {"meteor", QString("a straight, fast rise at %1 for %2").arg(speed, time)};
    if (run.blinking) return {"aircraft", QString("a blinking light moving %1 for %2").arg(speed, time)};
    if (!fast && run.straight && run.seconds >= 2)
        return {"satellite", QString("a steady light moving straight at %1 for %2").arg(speed, time)};
    return {"unclear", QString("a rise moving %1 for %2").arg(speed, time)};
}

} // namespace

ClipOverview makeClipOverview(const std::vector<FramePtr>& frames, const OverviewRequest& request, MemoryBudget budget)
{
    if (frames.empty() || !frames.front() || frames.front()->width <= 0 || frames.front()->height <= 0
        || frames.front()->data.size() != frameBytes(frames.front()->width, frames.front()->height, frames.front()->format))
        throw std::invalid_argument("an overview needs frames");
    const Activity activity = measure(frames, request, budget);
    const Frame& first = *frames.front();
    const int width = first.width, height = first.height;
    const int shift = shiftOf(first);
    const double k = request.sigma;
    ClipOverview result;

    // What changed anywhere in the clip.
    int left = width, top = height, right = -1, bottom = -1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const size_t i = static_cast<size_t>(y) * width + x;
            if (activity.rise[i] <= 0 && activity.fall[i] <= 0) continue;
            ++result.changedPixels;
            left = std::min(left, x); top = std::min(top, y); right = std::max(right, x); bottom = std::max(bottom, y);
        }
    if (result.changedPixels) result.changedBounds = QRect(QPoint(left, top), QPoint(right, bottom));
    for (const FrameActivity& frame : activity.frames) result.activeFrames += isActive(frame, request.minPixels);

    const size_t main = mainPosition(activity, request);
    result.mainFrameIndex = activity.frames[main].index;
    result.mainPosition = main;
    const Frame& mainFrame = *frames[main];
    const auto reference = request.referenceTime.value_or(mainFrame.timestamp);

    // The strip: before, the run's first frame, the main frame, its last
    // frame, and after. Each crop follows the activity on its frame.
    struct Cell { size_t position; QString role; };
    std::vector<Cell> cells;
    const std::vector<size_t> run = runAround(activity, main, request.minPixels);
    int crop = 0;
    if (!run.empty())
    {
        const size_t firstActive = run.front(), lastActive = run.back();
        result.hasActiveRun = true;
        result.firstActivePosition = firstActive;
        result.lastActivePosition = lastActive;
        const bool mainInRun = std::find(run.begin(), run.end(), main) != run.end();
        if (firstActive > 0) cells.push_back({firstActive - 1, "before"});
        cells.push_back({firstActive, firstActive == main ? "peak" : "first"});
        if (mainInRun && main != firstActive && main != lastActive) cells.push_back({main, "peak"});
        if (lastActive != firstActive) cells.push_back({lastActive, lastActive == main ? "peak" : "last"});
        if (lastActive + 1 < activity.frames.size()) cells.push_back({lastActive + 1, "after"});

        double extent = 0;
        for (size_t p : run) extent = std::max(extent, 4 * activity.frames[p].spread + 16);
        const FrameActivity& centreFrame = activity.frames[mainInRun ? main : firstActive];
        for (const QRect& mark : request.marks)
            if (std::abs(mark.center().x() - centreFrame.centreX) <= kWindowRadius
                && std::abs(mark.center().y() - centreFrame.centreY) <= kWindowRadius)
                extent = std::max(extent, static_cast<double>(std::max(mark.width(), mark.height()) + 16));
        crop = std::clamp(static_cast<int>(std::ceil(extent)), std::min(kMinCrop, std::min(width, height)), std::min(width, height));

        // A cell outside the run takes its crop from the run frame nearest to it.
        const auto nearestInRun = [&run](size_t position) {
            size_t best = run.front();
            for (size_t p : run)
                if ((p > position ? p - position : position - p) < (best > position ? best - position : position - best)) best = p;
            return best;
        };
        for (const Cell& cell : cells)
        {
            const FrameActivity& at = activity.frames[nearestInRun(cell.position)];
            result.stripFrames.push_back(activity.frames[cell.position].index);
            result.stripCentres.push_back(QPoint(static_cast<int>(std::lround(at.centreX)), static_cast<int>(std::lround(at.centreY))));
        }

        const FrameActivity& start = activity.frames[firstActive];
        const FrameActivity& end = activity.frames[lastActive];
        const double duration = seconds(end.time - start.time);
        const int runFrames = static_cast<int>(run.size());
        double strongest = 0;
        for (size_t p : run)
            if (std::abs(activity.frames[p].peak) > std::abs(strongest)) strongest = activity.frames[p].peak;
        result.activity = QString("Activity at K %1: %2 %3%4, strongest %5 %6 σ")
            .arg(k, 0, 'g', 4).arg(runFrames).arg(runFrames == 1 ? "frame" : "frames")
            .arg(runFrames > 1 ? QString(" over %1 s").arg(duration, 0, 'f', 3) : QString())
            .arg(strongest >= 0 ? "rise" : "fall").arg(std::abs(strongest), 0, 'f', 1);
        // Motion needs three frames: two can be seeing on neighbouring edges.
        const double dx = end.centreX - start.centreX, dy = end.centreY - start.centreY;
        const bool apart = std::hypot(dx, dy) >= kMinMovePixels;
        if (apart && runFrames >= 3 && duration > 0)
        {
            result.movedX = dx;
            result.movedY = dy;
            result.speed = std::hypot(dx, dy) / duration;
            QStringList parts;
            if (std::abs(dx) >= 1) parts << QString("%1 px %2").arg(std::abs(dx), 0, 'f', 0).arg(dx > 0 ? "right" : "left");
            if (std::abs(dy) >= 1) parts << QString("%1 px %2").arg(std::abs(dy), 0, 'f', 0).arg(dy > 0 ? "down" : "up");
            result.activity += QString("; moved %1, %2 px/s").arg(parts.join(" and ")).arg(result.speed, 0, 'f', 0);
            if (request.arcsecPerPixel)
                result.activity += QString(" (%1°/s)").arg(result.speed * *request.arcsecPerPixel / 3600, 0, 'f', 3);
        }
        else if (!apart) result.activity += "; in place";

        // The shape of the run, for the suggested kind.
        RunShape shape;
        shape.any = true;
        shape.rise = strongest >= 0;
        shape.moved = apart && runFrames >= 3 && duration > 0;
        shape.frames = runFrames;
        shape.seconds = duration;
        shape.pixelsPerSecond = result.speed;
        const double length = std::hypot(dx, dy);
        double furthest = 0;
        for (size_t p : run)
        {
            const auto& f = activity.frames[p];
            if (length > 0)
                furthest = std::max(furthest, std::abs((f.centreX - start.centreX) * dy - (f.centreY - start.centreY) * dx) / length);
        }
        shape.straight = length > 0 && furthest <= std::max(3.0, 0.05 * length);
        std::vector<double> strengths;
        for (size_t p : run) strengths.push_back(std::abs(activity.frames[p].peak));
        std::vector<double> sorted = strengths;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(sorted.size() / 2), sorted.end());
        const double typical = sorted[sorted.size() / 2];
        int bursts = 0;
        bool high = false;
        for (double strength : strengths)
        {
            const bool now = strength >= 2.5 * typical;
            if (now && !high) ++bursts;
            high = now;
        }
        shape.blinking = bursts >= 2 && duration >= 1.5;
        std::tie(result.suggestedKind, result.kindReason) = suggestKind(shape, request, width, height);
    }
    else
    {
        result.activity = QString("No activity at K %1 after the first %2 s, which the overview uses to learn.")
            .arg(k, 0, 'g', 4).arg(activity.learnedSeconds, 0, 'f', 1);
        std::tie(result.suggestedKind, result.kindReason) = suggestKind(RunShape{}, request, width, height);
    }

    // Layout: the main view, the strip, the caption.
    int viewWidth = width, viewHeight = height;
    if (width > kMaxMainWidth)
    {
        viewWidth = kMaxMainWidth;
        viewHeight = std::max(1, static_cast<int>(std::lround(static_cast<double>(height) * kMaxMainWidth / width)));
    }
    else if (width < kMinMainWidth)
    {
        const int factor = (kMinMainWidth + width - 1) / width;
        viewWidth = width * factor;
        viewHeight = height * factor;
    }
    const double scaleX = static_cast<double>(viewWidth) / width, scaleY = static_cast<double>(viewHeight) / height;
    const int cellSide = (viewWidth - (kStripSlots + 1) * kGap) / kStripSlots;
    QFont labelFont;
    labelFont.setPixelSize(12);
    QFont captionFont;
    captionFont.setPixelSize(14);
    const int labelLine = QFontMetrics(labelFont).height();
    const int captionLine = QFontMetrics(captionFont).height() + 3;
    const int stripTop = viewHeight + kGap;
    const int stripHeight = cells.empty() ? 0 : cellSide + 2 * labelLine + 2 * kGap;
    QStringList caption = request.caption;
    caption << result.activity;
    caption << QString("Suggested: %1 — %2").arg(result.suggestedKind, result.kindReason);
    const int captionTop = stripTop + stripHeight;
    const int pictureHeight = captionTop + static_cast<int>(caption.size()) * captionLine + kGap;

    QImage canvas = makeBudgetImage(viewWidth, pictureHeight, QImage::Format_RGB32, budget);
    if (canvas.isNull()) throw MemoryLimitError();
    canvas.fill(kBackground);

    // The main view: the frame stretched as the display's automatic stretch
    // does, then the change over it.
    const QRect view(0, 0, viewWidth, viewHeight);
    {
        const QImage base = frameToPreviewImage(mainFrame, DisplaySettings{DisplayMode::Auto, 0, 1, false, true});
        QPainter painter(&canvas);
        if (viewWidth < width) painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (!base.isNull()) painter.drawImage(view, base);
    }
    paintChange(canvas, view, activity, QRect(0, 0, width, height), k);

    // The strip's crops, and the change around the main frame.
    std::vector<QRect> stripCrops;
    std::vector<const Frame*> stripSources;
    if (!cells.empty())
    {
        for (size_t c = 0; c < cells.size(); ++c)
        {
            stripCrops.push_back(cropAround(result.stripCentres[c].x(), result.stripCentres[c].y(), crop, width, height));
            stripSources.push_back(frames[cells[c].position].get());
        }
        const Levels levels = stripLevels(stripSources, stripCrops, shift, budget);
        for (size_t c = 0; c < cells.size(); ++c)
            paintCrop(canvas, QRect(kGap + static_cast<int>(c) * (cellSide + kGap), stripTop, cellSide, cellSide),
                *stripSources[c], stripCrops[c], levels, shift);
        // The last slot: the clip's change around the main frame, over that
        // frame at half brightness, in the crop of the cell nearest to it.
        size_t mainCell = 0;
        const auto distance = [main](size_t position) { return position > main ? position - main : main - position; };
        for (size_t c = 1; c < cells.size(); ++c)
            if (distance(cells[c].position) < distance(cells[mainCell].position)) mainCell = c;
        const QRect changeCell(kGap + (kStripSlots - 1) * (cellSide + kGap), stripTop, cellSide, cellSide);
        paintCrop(canvas, changeCell, mainFrame, stripCrops[mainCell], levels, shift);
        paintChange(canvas, changeCell, activity, stripCrops[mainCell], k, 0.5);
    }

    QPainter painter(&canvas);
    // The mask, hatched: the detector did not look there.
    for (const MaskShape& shape : request.mask.shapes())
    {
        QPolygonF area;
        for (const MaskPoint& point : shape) area << QPointF(point.x * scaleX, point.y * scaleY);
        QPainterPath path;
        path.addPolygon(area);
        path.closeSubpath();
        painter.fillPath(path, QColor(0, 0, 0, 110));
        painter.fillPath(path, QBrush(QColor(200, 60, 60, 150), Qt::BDiagPattern));
    }
    painter.setRenderHint(QPainter::Antialiasing);
    for (size_t m = 0; m < request.marks.size(); ++m)
    {
        const QRect& mark = request.marks[m];
        painter.setFont(labelFont);
        drawMark(painter, QRectF(mark.x() * scaleX, mark.y() * scaleY, mark.width() * scaleX, mark.height() * scaleY),
            request.marks.size() > 1 ? static_cast<int>(m) + 1 : 0);
    }
    painter.setRenderHint(QPainter::Antialiasing, false);

    painter.setFont(labelFont);
    for (size_t c = 0; c < cells.size(); ++c)
    {
        const int x = kGap + static_cast<int>(c) * (cellSide + kGap);
        const Frame& frame = *stripSources[c];
        painter.setPen(QPen(kDimText, 1));
        painter.drawRect(QRect(x, stripTop, cellSide, cellSide).adjusted(-1, -1, 0, 0));
        painter.setPen(kText);
        painter.drawText(QRect(x, stripTop + cellSide + 2, cellSide, labelLine), Qt::AlignLeft | Qt::AlignVCenter,
            QString("%1  %2").arg(cells[c].role, signedSeconds(seconds(frame.timestamp - reference))));
        painter.setPen(kDimText);
        painter.drawText(QRect(x, stripTop + cellSide + 2 + labelLine, cellSide, labelLine), Qt::AlignLeft | Qt::AlignVCenter,
            QString("frame %1").arg(frame.frameIndex));
    }
    if (!cells.empty())
    {
        const int x = kGap + (kStripSlots - 1) * (cellSide + kGap);
        painter.setPen(QPen(kDimText, 1));
        painter.drawRect(QRect(x, stripTop, cellSide, cellSide).adjusted(-1, -1, 0, 0));
        painter.setPen(kText);
        painter.drawText(QRect(x, stripTop + cellSide + 2, cellSide, labelLine), Qt::AlignLeft | Qt::AlignVCenter, "change");
        painter.setPen(kDimText);
        painter.drawText(QRect(x, stripTop + cellSide + 2 + labelLine, cellSide, labelLine), Qt::AlignLeft | Qt::AlignVCenter,
            QString("%1 px around the peak").arg(crop));
    }

    painter.setFont(captionFont);
    const QFontMetrics captionMetrics(captionFont);
    for (int line = 0; line < caption.size(); ++line)
    {
        painter.setPen(line + 1 == caption.size() ? kRise.lighter(130) : kText);
        painter.drawText(QRect(kGap, captionTop + line * captionLine, viewWidth - 2 * kGap, captionLine), Qt::AlignLeft | Qt::AlignVCenter,
            captionMetrics.elidedText(caption[line], Qt::ElideRight, viewWidth - 2 * kGap));
    }
    painter.end();
    result.image = canvas;
    return result;
}

std::vector<size_t> selectEventGifFramePositions(size_t frameCount,
    size_t firstActive, size_t peak, size_t lastActive,
    size_t paddingBefore, size_t paddingAfter, size_t maxFrames)
{
    if (frameCount == 0 || maxFrames == 0) return {};
    const size_t maxIdx = frameCount - 1;
    firstActive = std::min(firstActive, maxIdx);
    peak = std::min(peak, maxIdx);
    lastActive = std::clamp(lastActive, firstActive, maxIdx);

    const size_t lo = std::min(peak, firstActive >= paddingBefore ? firstActive - paddingBefore : 0);
    const size_t hi = std::max(peak, lastActive + std::min(maxIdx - lastActive, paddingAfter));
    const size_t span = hi - lo + 1;

    std::vector<size_t> out;
    if (span <= maxFrames)
    {
        out.reserve(span);
        for (size_t p = lo; p <= hi; ++p) out.push_back(p);
        return out;
    }
    if (maxFrames == 1) return {peak};

    out.reserve(maxFrames);
    const size_t intervals = maxFrames - 1;
    if (peak == lo || peak == hi)
    {
        for (size_t i = 0; i < maxFrames; ++i)
            out.push_back(lo + (i * (hi - lo)) / intervals);
        return out;
    }

    const size_t totalSpan = hi - lo;
    size_t leftIntervals = static_cast<size_t>(std::clamp<long>(
        std::lround(static_cast<double>(intervals) * static_cast<double>(peak - lo) / static_cast<double>(totalSpan)),
        1L, static_cast<long>(intervals - 1)));
    leftIntervals = std::min(leftIntervals, peak - lo);
    size_t rightIntervals = intervals - leftIntervals;
    if (rightIntervals > hi - peak)
    {
        rightIntervals = hi - peak;
        leftIntervals = std::min(intervals - rightIntervals, peak - lo);
    }

    for (size_t i = 0; i < leftIntervals; ++i)
        out.push_back(lo + (i * (peak - lo)) / leftIntervals);
    out.push_back(peak);
    for (size_t j = 1; j <= rightIntervals; ++j)
        out.push_back(peak + (j * (hi - peak)) / rightIntervals);
    return out;
}
