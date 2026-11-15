#include "guiding/feature_lock.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace {

// The native value of one pixel: ADU for mono and raw Bayer data, the mean
// of the three channels for RGB.
float sample(const Frame& frame, int x, int y)
{
    const size_t index = static_cast<size_t>(y) * static_cast<size_t>(frame.width) + static_cast<size_t>(x);
    switch (frame.format)
    {
    case PixelFormat::Mono8:
        return frame.data[index];
    case PixelFormat::Mono16:
    {
        uint16_t word = 0;
        std::memcpy(&word, frame.data.data() + index * 2, sizeof(word));
        if (frame.alignment == BitAlignment::High && frame.significantBits < 16)
            word = static_cast<uint16_t>(word >> (16 - frame.significantBits));
        return word;
    }
    case PixelFormat::RGB888:
    {
        const uint8_t* pixel = frame.data.data() + index * 3;
        return (static_cast<float>(pixel[0]) + pixel[1] + pixel[2]) / 3.0f;
    }
    }
    return 0;
}

double highestCode(const Frame& frame)
{
    return frame.format == PixelFormat::Mono16 ? std::ldexp(1.0, frame.significantBits) - 1.0 : 255.0;
}

bool isMosaic(const Frame& frame)
{
    return frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
}

// The mean of each binning x binning block. Block (c, r) starts at native
// pixel (x0 + c * binning, y0 + r * binning).
std::vector<float> binnedBlock(const Frame& frame, int x0, int y0, int columns, int rows, int binning)
{
    std::vector<float> out(static_cast<size_t>(columns) * static_cast<size_t>(rows));
    const float scale = 1.0f / static_cast<float>(binning * binning);
    for (int r = 0; r < rows; ++r)
    {
        for (int c = 0; c < columns; ++c)
        {
            float sum = 0;
            for (int j = 0; j < binning; ++j)
            {
                for (int i = 0; i < binning; ++i)
                {
                    sum += sample(frame, x0 + c * binning + i, y0 + r * binning + j);
                }
            }
            out[static_cast<size_t>(r) * columns + c] = sum * scale;
        }
    }
    return out;
}

double median(std::vector<double> values)
{
    if (values.empty()) return 0;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// Pixel noise of an image with structure. The 3 x 3 operator is a second
// difference across times a second difference down, so smooth shading,
// ramps, and gentle curvature give nearly nothing, while independent noise
// of standard deviation s gives 6 s. The median of its magnitude is not
// moved by the few pixels on sharp detail.
double pixelNoise(const std::vector<float>& values, int columns, int rows)
{
    std::vector<double> responses;
    responses.reserve(static_cast<size_t>(columns) * rows);
    auto at = [&](int x, int y) { return static_cast<double>(values[static_cast<size_t>(y) * columns + x]); };
    for (int y = 1; y + 1 < rows; ++y)
    {
        for (int x = 1; x + 1 < columns; ++x)
        {
            const double response = at(x - 1, y - 1) - 2 * at(x, y - 1) + at(x + 1, y - 1)
                - 2 * at(x - 1, y) + 4 * at(x, y) - 2 * at(x + 1, y)
                + at(x - 1, y + 1) - 2 * at(x, y + 1) + at(x + 1, y + 1);
            responses.push_back(std::abs(response));
        }
    }
    return 1.4826 * median(std::move(responses)) / 6.0;
}

int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int ceilDiv(int a, int b)
{
    return -floorDiv(-a, b);
}

// Offset of a parabola's vertex through three equally spaced scores.
double vertex(double before, double peak, double after)
{
    const double curvature = before - 2.0 * peak + after;
    if (curvature >= 0) return 0;
    return std::clamp(0.5 * (before - after) / curvature, -0.5, 0.5);
}

} // namespace

std::optional<FeatureLock> FeatureLock::take(const Frame& frame, LockRegion region, std::string& refusal)
{
    const int x0 = std::max(region.x, 0);
    const int y0 = std::max(region.y, 0);
    const int x1 = std::min(region.x + region.width, frame.width);
    const int y1 = std::min(region.y + region.height, frame.height);
    if (x1 - x0 < kMinTemplateSide || y1 - y0 < kMinTemplateSide)
    {
        refusal = std::format("The region must lie in the frame and be at least {0} x {0} pixels.", kMinTemplateSide);
        return std::nullopt;
    }

    const bool mosaic = isMosaic(frame);
    int binning = (std::max(x1 - x0, y1 - y0) + kMaxTemplateSide - 1) / kMaxTemplateSide;
    if (mosaic) binning = std::max(2, binning + binning % 2);
    const int columns = (x1 - x0) / binning;
    const int rows = (y1 - y0) / binning;
    if (columns < kMinTemplateSide || rows < kMinTemplateSide)
    {
        refusal = std::format("The region is too narrow for its length: binned by {} it is {} x {} pixels, "
            "at least {} needed on each side.", binning, columns, rows, kMinTemplateSide);
        return std::nullopt;
    }

    FeatureLock lock;
    lock.region_ = {x0, y0, columns * binning, rows * binning};
    lock.binning_ = binning;
    lock.templateWidth_ = columns;
    lock.templateHeight_ = rows;
    lock.frameWidth_ = frame.width;
    lock.frameHeight_ = frame.height;
    lock.format_ = frame.format;
    lock.bayer_ = frame.bayer;
    LockQuality& quality = lock.quality_;

    const double highest = highestCode(frame);
    size_t saturated = 0;
    for (int y = y0; y < y0 + rows * binning; ++y)
        for (int x = x0; x < x0 + columns * binning; ++x)
            if (sample(frame, x, y) >= highest - 0.5) ++saturated;
    quality.saturatedFraction = static_cast<double>(saturated) / (static_cast<double>(columns) * rows * binning * binning);
    if (quality.saturatedFraction > kMaxSaturatedFraction)
    {
        refusal = std::format("The region is mostly saturated ({:.0f}% of its pixels at the highest code). "
            "Choose a region with visible detail.", 100.0 * quality.saturatedFraction);
        return std::nullopt;
    }

    std::vector<float> values = binnedBlock(frame, x0, y0, columns, rows, binning);
    double mean = 0;
    for (float value : values) mean += value;
    mean /= static_cast<double>(values.size());
    double variance = 0;
    for (float value : values) variance += (value - mean) * (value - mean);
    variance /= static_cast<double>(values.size());

    // One quantisation step of the native samples, averaged over a bin, is
    // the least noise a measured value can carry.
    const double quantisation = 1.0 / std::sqrt(12.0) / binning;
    quality.noiseAdu = std::max(quantisation, pixelNoise(values, columns, rows));
    quality.contrast = std::sqrt(variance) / quality.noiseAdu;
    if (quality.contrast < kMinContrast)
    {
        refusal = std::format("Too little contrast: the detail is {:.1f} times the pixel noise, at least {:.0f} needed. "
            "Choose a region with craters or other clear detail.", quality.contrast, kMinContrast);
        return std::nullopt;
    }

    // Gradient structure: the eigenvalues of the summed outer products of
    // the gradient, less the share that the noise alone contributes.
    double gxx = 0, gyy = 0, gxy = 0;
    int counted = 0;
    for (int y = 1; y + 1 < rows; ++y)
    {
        for (int x = 1; x + 1 < columns; ++x)
        {
            const double ix = 0.5 * (values[y * columns + x + 1] - values[y * columns + x - 1]);
            const double iy = 0.5 * (values[(y + 1) * columns + x] - values[(y - 1) * columns + x]);
            gxx += ix * ix;
            gyy += iy * iy;
            gxy += ix * iy;
            ++counted;
        }
    }
    const double noiseShare = counted * quality.noiseAdu * quality.noiseAdu / 2.0;
    const double half = 0.5 * (gxx + gyy);
    const double spread = std::sqrt(std::max(0.0, half * half - (gxx * gyy - gxy * gxy)));
    const double strong = half + spread - noiseShare;
    const double weak = half - spread - noiseShare;
    quality.structureRatio = strong > 0 ? std::max(0.0, weak) / strong : 0.0;
    if (quality.structureRatio < kMinStructureRatio)
    {
        refusal = std::format("Structure in one direction only (ratio {:.2f}, at least {:.2f} needed). A straight edge, "
            "limb or terminator fixes the position across it but not along it. Choose a region with detail in both directions.",
            quality.structureRatio, kMinStructureRatio);
        return std::nullopt;
    }

    lock.template_.resize(values.size());
    for (size_t i = 0; i < values.size(); ++i)
    {
        lock.template_[i] = static_cast<float>(values[i] - mean);
        lock.energy_ += static_cast<double>(lock.template_[i]) * lock.template_[i];
    }
    return lock;
}

bool FeatureLock::sameLayout(const Frame& frame) const
{
    return frame.width == frameWidth_ && frame.height == frameHeight_ && frame.format == format_ && frame.bayer == bayer_;
}

LockMatch FeatureLock::find(const Frame& frame, double expectedDx, double expectedDy, int radiusPixels) const
{
    LockMatch match;
    if (!sameLayout(frame))
    {
        match.reason = "the frame layout differs from the locked one";
        return match;
    }

    const int b = binning_;
    // Candidate positions step by whole bins from a base near the expected
    // offset. On a mosaic the base is even, so every block holds the same
    // colour cell arrangement as the template's blocks.
    int baseDx = static_cast<int>(std::lround(expectedDx));
    int baseDy = static_cast<int>(std::lround(expectedDy));
    if (bayer_ != BayerPattern::None && format_ != PixelFormat::RGB888)
    {
        baseDx = 2 * static_cast<int>(std::lround(expectedDx / 2.0));
        baseDy = 2 * static_cast<int>(std::lround(expectedDy / 2.0));
    }
    const int radius = std::max(1, ceilDiv(radiusPixels, b));
    const int originX = region_.x + baseDx;
    const int originY = region_.y + baseDy;
    const int kMin = std::max(-radius, ceilDiv(-originX, b));
    const int kMax = std::min(radius, floorDiv(frame.width - region_.width - originX, b));
    const int lMin = std::max(-radius, ceilDiv(-originY, b));
    const int lMax = std::min(radius, floorDiv(frame.height - region_.height - originY, b));
    if (kMin > kMax || lMin > lMax)
    {
        match.reason = "the expected position is outside the frame";
        return match;
    }

    const int columns = kMax - kMin + templateWidth_;
    const int rows = lMax - lMin + templateHeight_;
    std::vector<float> image = binnedBlock(frame, originX + kMin * b, originY + lMin * b, columns, rows, b);
    // Centring the values keeps the window sums well conditioned on bright data.
    double offset = 0;
    for (float value : image) offset += value;
    offset /= static_cast<double>(image.size());
    for (float& value : image) value = static_cast<float>(value - offset);

    // Integral images of the values and of their squares give each window's
    // mean and variance in four reads.
    const size_t stride = static_cast<size_t>(columns) + 1;
    std::vector<double> sum(stride * (rows + 1), 0.0), squares(stride * (rows + 1), 0.0);
    for (int y = 0; y < rows; ++y)
    {
        double rowSum = 0, rowSquares = 0;
        for (int x = 0; x < columns; ++x)
        {
            const double value = image[static_cast<size_t>(y) * columns + x];
            rowSum += value;
            rowSquares += value * value;
            sum[(y + 1) * stride + x + 1] = sum[y * stride + x + 1] + rowSum;
            squares[(y + 1) * stride + x + 1] = squares[y * stride + x + 1] + rowSquares;
        }
    }
    auto boxed = [&](const std::vector<double>& table, int x, int y) {
        return table[(y + templateHeight_) * stride + x + templateWidth_] - table[y * stride + x + templateWidth_]
            - table[(y + templateHeight_) * stride + x] + table[y * stride + x];
    };

    const int width = kMax - kMin + 1;
    const int height = lMax - lMin + 1;
    const double count = static_cast<double>(templateWidth_) * templateHeight_;
    std::vector<double> scores(static_cast<size_t>(width) * height, -1.0);
    int best = 0;
    for (int l = 0; l < height; ++l)
    {
        for (int k = 0; k < width; ++k)
        {
            // The template has zero mean, so the window's own mean drops out of the cross term.
            double cross = 0;
            for (int j = 0; j < templateHeight_; ++j)
            {
                const float* row = image.data() + static_cast<size_t>(l + j) * columns + k;
                const float* reference = template_.data() + static_cast<size_t>(j) * templateWidth_;
                for (int i = 0; i < templateWidth_; ++i) cross += static_cast<double>(reference[i]) * row[i];
            }
            const double windowSum = boxed(sum, k, l);
            const double variance = boxed(squares, k, l) - windowSum * windowSum / count;
            const double score = variance > 0 && energy_ > 0 ? cross / std::sqrt(energy_ * variance) : 0.0;
            scores[static_cast<size_t>(l) * width + k] = score;
            if (score > scores[best]) best = l * width + k;
        }
    }

    const int bk = best % width;
    const int bl = best / width;
    match.score = scores[best];
    double subX = 0, subY = 0;
    const bool onEdge = bk == 0 || bk == width - 1 || bl == 0 || bl == height - 1;
    if (!onEdge)
    {
        subX = vertex(scores[best - 1], match.score, scores[best + 1]);
        subY = vertex(scores[best - width], match.score, scores[best + width]);
    }
    match.dx = baseDx + (kMin + bk + subX) * b;
    match.dy = baseDy + (lMin + bl + subY) * b;
    if (onEdge) match.reason = "the best match is on the edge of the search window";
    else if (match.score < kMinLockScore) match.reason = std::format("match score {:.2f} below {:.2f}", match.score, kMinLockScore);
    match.valid = match.reason.empty();
    return match;
}
