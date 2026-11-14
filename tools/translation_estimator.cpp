#include "tools/translation_estimator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace { constexpr int kBin = 8; }

FramePtr registrationInterior(const FramePtr& source, int margin, int dx, int dy)
{
    if (!source || margin < 0 || source->width <= 2LL*margin || source->height <= 2LL*margin
        || dx < -margin || dx > margin || dy < -margin || dy > margin || source->bayer != BayerPattern::None
        || bytesPerPixel(source->format)==0 || source->data.size()!=frameBytes(source->width,source->height,source->format))
        throw std::invalid_argument("registration interior is outside the source or has an unsupported layout");
    auto frame = std::make_shared<Frame>(source->data.get_allocator().budget);
    frame->width = source->width - 2*margin; frame->height = source->height - 2*margin;
    frame->format = source->format; frame->significantBits = source->significantBits;
    frame->alignment = source->alignment; frame->bayer = source->bayer;
    frame->frameIndex = source->frameIndex; frame->timestamp = source->timestamp;
    frame->steadyTimestamp = source->steadyTimestamp; frame->testSource = true;
    frame->syntheticFlash = source->syntheticFlash; frame->blip = source->blip;
    if (frame->blip) { frame->blip->settings.x -= margin+dx; frame->blip->settings.y -= margin+dy; }
    const size_t bytes = static_cast<size_t>(bytesPerPixel(frame->format));
    frame->data.resize(frameBytes(frame->width,frame->height,frame->format));
    for (int y=0; y<frame->height; ++y)
        std::copy_n(source->data.data()+(static_cast<size_t>(y+margin+dy)*source->width+margin+dx)*bytes,
            static_cast<size_t>(frame->width)*bytes,frame->data.data()+static_cast<size_t>(y)*frame->width*bytes);
    return frame;
}

TranslationEstimator::TranslationEstimator(const Frame& reference, int maxShift)
    : width_(reference.width), height_(reference.height), bits_(reference.significantBits),
      maxShift_(std::clamp(maxShift, 0, 256)), margin_((maxShift_ / kBin + 2) * kBin),
      format_(reference.format), alignment_(reference.alignment)
{
    if (maxShift < 0 || maxShift > 256) throw std::invalid_argument("registration search must be between 0 and 256 pixels");
    if (width_ <= 2 * margin_ + kBin || height_ <= 2 * margin_ + kBin)
        throw std::invalid_argument("frame too small for the registration search margin");
    read(reference);
    coarse_ = grid(binned_, width_ / kBin, height_ / kBin, margin_ / kBin, 1);
    fine_ = grid(gray_, width_, height_, margin_, kBin);
}

void TranslationEstimator::read(const Frame& frame)
{
    if (frame.width != width_ || frame.height != height_ || frame.format != format_
        || frame.significantBits != bits_ || frame.alignment != alignment_ || frame.bayer != BayerPattern::None
        || bits_ < 1 || bits_ > 16 || bytesPerPixel(format_) == 0
        || frame.data.size() != frameBytes(width_, height_, format_))
        throw std::invalid_argument("registration requires a consistent mono/RGB layout; raw Bayer phase shifts are outside this experiment");
    gray_.resize(static_cast<size_t>(width_) * height_);
    const int shift = format_ == PixelFormat::Mono16 && alignment_ == BitAlignment::High ? 16 - bits_ : 0;
    for (size_t i = 0; i < gray_.size(); ++i)
    {
        if (format_ == PixelFormat::Mono8) gray_[i] = frame.data[i];
        else if (format_ == PixelFormat::RGB888) gray_[i] = (frame.data[3*i] + frame.data[3*i+1] + frame.data[3*i+2]) / 3.0f;
        else gray_[i] = static_cast<float>((frame.data[2*i] | (frame.data[2*i+1] << 8)) >> shift);
    }
    const int bw = width_ / kBin, bh = height_ / kBin;
    binned_.assign(static_cast<size_t>(bw) * bh, 0);
    for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x)
    {
        double sum = 0;
        for (int dy = 0; dy < kBin; ++dy) for (int dx = 0; dx < kBin; ++dx)
            sum += gray_[static_cast<size_t>(y*kBin+dy) * width_ + x*kBin+dx];
        binned_[static_cast<size_t>(y) * bw + x] = static_cast<float>(sum / (kBin*kBin));
    }
}

TranslationEstimator::Grid TranslationEstimator::grid(const std::vector<float>& image, int width, int height, int margin, int step)
{
    Grid result;
    double sum = 0;
    for (int y = margin; y < height - margin; y += step) for (int x = margin; x < width - margin; x += step)
    {
        const float value = image[static_cast<size_t>(y) * width + x];
        result.points.push_back({x,y,value}); sum += value;
    }
    const double mean = sum / result.points.size();
    for (auto& point : result.points)
    {
        point.reference = static_cast<float>(point.reference - mean);
        result.energy += static_cast<double>(point.reference) * point.reference;
    }
    return result;
}

double TranslationEstimator::score(const Grid& reference, const std::vector<float>& image, int width, int dx, int dy)
{
    double sum = 0, squares = 0, product = 0;
    for (const auto& point : reference.points)
    {
        const double value = image[static_cast<size_t>(point.y + dy) * width + point.x + dx];
        sum += value; squares += value * value; product += point.reference * value;
    }
    const double energy = squares - sum * sum / reference.points.size();
    if (energy <= 1e-8 || reference.energy <= 1e-8) return -2;
    return std::clamp(product / std::sqrt(energy * reference.energy), -1.0, 1.0);
}

FrameTranslation TranslationEstimator::estimate(const Frame& frame)
{
    read(frame);
    FrameTranslation best;
    double coarseBest = -2;
    struct Candidate { int x,y; double score; };
    std::vector<Candidate> coarseScores;
    const int extent = maxShift_ / kBin;
    for (int y = -extent; y <= extent; ++y) for (int x = -extent; x <= extent; ++x)
    {
        const double value = score(coarse_, binned_, width_ / kBin, x, y);
        coarseScores.push_back({x*kBin,y*kBin,value});
        if (value > coarseBest) { coarseBest = value; best.dx = x*kBin; best.dy = y*kBin; }
    }
    const int cx = best.dx, cy = best.dy;
    double fineBest = -2;
    for (int y = std::max(-maxShift_, cy-kBin+1); y <= std::min(maxShift_, cy+kBin-1); ++y)
        for (int x = std::max(-maxShift_, cx-kBin+1); x <= std::min(maxShift_, cx+kBin-1); ++x)
        {
            const double value = score(fine_, gray_, width_, x, y);
            if (value > fineBest) { fineBest = value; best.dx = x; best.dy = y; }
        }
    double distant = -2;
    for (const auto& item : coarseScores)
        if (std::max(std::abs(item.x-best.dx), std::abs(item.y-best.dy)) >= 2*kBin) distant = std::max(distant, item.score);
    best.correlation = std::max(0.0, fineBest);
    best.peakGap = coarseBest - distant;
    best.valid = fineBest >= 0.7 && best.peakGap > 1e-5;
    if (!best.valid) best.dx = best.dy = 0;
    return best;
}
