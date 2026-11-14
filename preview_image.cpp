#include "preview_image.h"

#include <algorithm>
#include <cmath>
#include <limits>

QImage makeBudgetImage(int width, int height, QImage::Format format, MemoryBudget budget)
{
    const int channels = format == QImage::Format_Grayscale8 ? 1 : format == QImage::Format_RGB888 ? 3 : 4;
    if (width <= 0 || height <= 0 || width > (std::numeric_limits<int>::max() - 3) / channels) return {};
    const int stride = (width * channels + 3) & ~3;
    auto storage = std::make_unique<BudgetVector<uint8_t>>(static_cast<size_t>(stride) * height,
        uint8_t{0}, BudgetAllocator<uint8_t>(std::move(budget)));
    QImage result(storage->data(), width, height, stride, format,
        [](void* data) { delete static_cast<BudgetVector<uint8_t>*>(data); }, storage.get());
    if (result.isNull()) return {};
    storage.release();
    return result;
}

namespace {

// R=0, G=1, B=2 in each top-left 2x2 filter cell.
int channelAt(BayerPattern pattern, int x, int y)
{
    constexpr int cells[][4] = {{0,1,1,2}, {1,0,2,1}, {1,2,0,1}, {2,1,1,0}};
    return cells[static_cast<int>(pattern) - 1][(y & 1) * 2 + (x & 1)];
}

double sampleAt(const Frame& frame, size_t index, int channel = -1)
{
    if (frame.format == PixelFormat::Mono8) return frame.data[index];
    if (frame.format == PixelFormat::RGB888)
    {
        if (channel >= 0) return frame.data[3 * index + channel];
        return (frame.data[3 * index] + frame.data[3 * index + 1] + frame.data[3 * index + 2]) / 3.0;
    }
    const int shift = frame.alignment == BitAlignment::High ? 16 - frame.significantBits : 0;
    return (frame.data[2 * index] | (frame.data[2 * index + 1] << 8)) >> shift;
}

double colourAt(const Frame& frame, int x, int y, int channel)
{
    if (channelAt(frame.bayer, x, y) == channel) return sampleAt(frame, static_cast<size_t>(y) * frame.width + x);
    double sum = 0;
    int count = 0;
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
    {
        const int sx = x + dx, sy = y + dy;
        if (sx < 0 || sx >= frame.width || sy < 0 || sy >= frame.height || channelAt(frame.bayer, sx, sy) != channel) continue;
        sum += sampleAt(frame, static_cast<size_t>(sy) * frame.width + sx);
        ++count;
    }
    return count ? sum / count : sampleAt(frame, static_cast<size_t>(y) * frame.width + x);
}

double midtones(double m, double value)
{
    if (value <= 0) return 0;
    if (value >= 1) return 1;
    return (m - 1) * value / ((2 * m - 1) * value - m);
}

} // namespace

QImage frameToPreviewImage(const Frame& frame, DisplaySettings settings, DisplayLevels* levels)
{
    if (frame.width <= 0 || frame.height <= 0 || bytesPerPixel(frame.format) == 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format)
        || frame.significantBits < 1 || frame.significantBits > 16 || !settings.valid()
        || static_cast<int>(frame.bayer) < 0 || static_cast<int>(frame.bayer) > 4) return {};
    const MemoryBudget budget = frame.data.get_allocator().budget;
    const int maximum = frame.format == PixelFormat::Mono16 ? (1 << frame.significantBits) - 1 : 255;
    double black = settings.mode == DisplayMode::Window ? settings.black * maximum : 0;
    double white = settings.mode == DisplayMode::Window ? settings.white * maximum : maximum;
    double median = 0, m = 0.5;
    if (settings.mode == DisplayMode::Auto)
    {
        BudgetVector<double> grid{BudgetAllocator<double>(budget)};
        const size_t count = static_cast<size_t>(frame.width) * frame.height;
        const size_t step = std::max(size_t{1}, (count + 8191) / 8192);
        grid.reserve(std::min(count, size_t{8192}));
        size_t sample = 0;
        for (size_t base = 0; base < count; base += step, ++sample)
        {
            // Walk within each stratum so an even stride does not always
            // sample the same Bayer column/filter phase.
            grid.push_back(sampleAt(frame, std::min(base + sample % step, count - 1)));
        }
        std::sort(grid.begin(), grid.end());
        black = grid[(grid.size() - 1) / 1000];
        white = grid[(grid.size() - 1) * 999 / 1000];
        median = grid[grid.size() / 2];
        if (white <= black) { black = 0; white = maximum; }
        const double centre = std::clamp((median - black) / (white - black), 0.0, 1.0);
        if (centre > 0 && centre < 1) m = std::clamp(midtones(0.25, centre), 0.000001, 0.999999);
    }
    if (levels) *levels = {black, white, median};
    const bool bayer = settings.debayer && frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
    const bool colour = bayer || frame.format == PixelFormat::RGB888;
    QImage image = makeBudgetImage(frame.width, frame.height,
        colour ? QImage::Format_RGB888 : QImage::Format_Grayscale8, budget);
    if (image.isNull()) return {};
    const auto convert = [&](double value) {
        int display;
        if (settings.mode == DisplayMode::Linear)
        {
            const int shift = frame.format == PixelFormat::Mono16 ? std::max(0, frame.significantBits - 8) : 0;
            display = std::clamp(static_cast<int>(value) >> shift, 0, 255);
        }
        else display = static_cast<int>(std::round(255 * midtones(m, std::clamp((value - black) / (white - black), 0.0, 1.0))));
        return static_cast<uchar>(settings.invert ? 255 - display : display);
    };
    for (int y = 0; y < frame.height; ++y)
    {
        uchar* row = image.scanLine(y);
        for (int x = 0; x < frame.width; ++x)
        {
            const size_t i = static_cast<size_t>(y) * frame.width + x;
            for (int c = 0; c < (colour ? 3 : 1); ++c)
                row[(colour ? 3 : 1) * x + c] = convert(bayer ? colourAt(frame, x, y, c) : sampleAt(frame, i, colour ? c : -1));
        }
    }
    return image;
}
