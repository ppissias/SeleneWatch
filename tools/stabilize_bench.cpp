#include "tools/stabilize_bench.h"

#include "detection/flash_detector.h"
#include "detection/target_defaults.h"
#include "guiding/feature_lock.h"
#include "recording/recording_reader.h"
#include "session/blip_injector.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- Shared lightweight sample reading ---

inline float readPixel(const Frame& frame, int x, int y)
{
    const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(frame.width) + static_cast<size_t>(x);
    switch (frame.format)
    {
    case PixelFormat::Mono8: return frame.data[idx];
    case PixelFormat::Mono16:
    {
        uint16_t v = 0;
        std::memcpy(&v, frame.data.data() + 2 * idx, sizeof(v));
        if (frame.alignment == BitAlignment::High && frame.significantBits < 16)
            v = static_cast<uint16_t>(v >> (16 - frame.significantBits));
        return static_cast<float>(v);
    }
    case PixelFormat::RGB888:
    {
        const uint8_t* p = frame.data.data() + 3 * idx;
        return (static_cast<float>(p[0]) + p[1] + p[2]) / 3.0f;
    }
    }
    return 0.0f;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0.0;
    const size_t idx = std::min(values.size() - 1,
        static_cast<size_t>(std::floor(p * static_cast<double>(values.size() - 1))));
    std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(idx), values.end());
    return values[idx];
}

double meanOf(const std::vector<double>& values)
{
    if (values.empty()) return 0.0;
    double s = 0.0;
    for (double v : values) s += v;
    return s / static_cast<double>(values.size());
}

// --- Shift Estimators ---

enum class EstimatorKind { Auto, FeatureLock, DiskCoG, PhaseCorr };

const char* estimatorName(EstimatorKind kind)
{
    switch (kind)
    {
    case EstimatorKind::Auto: return "auto";
    case EstimatorKind::FeatureLock: return "feature_lock";
    case EstimatorKind::DiskCoG: return "disk_cog";
    case EstimatorKind::PhaseCorr: return "phase_corr";
    }
    return "unknown";
}

struct ShiftMeasurement
{
    bool valid{false};
    double dx{0};
    double dy{0};
    double score{0};
    double elapsedMs{0};
    std::string reason;
};

// Detects whether frame shows an isolated bright disk (planet, full Moon/Sun)
// surrounded by dark sky rather than a surface filling the frame.
bool isIsolatedDisk(const Frame& frame)
{
    if (frame.width < 32 || frame.height < 32) return false;
    std::vector<double> border;
    border.reserve(static_cast<size_t>(frame.width + frame.height) * 4);
    const int ring = std::min(4, std::min(frame.width, frame.height) / 8);
    for (int y = 0; y < frame.height; y += 2)
    {
        for (int x = 0; x < frame.width; x += 2)
        {
            if (x < ring || x >= frame.width - ring || y < ring || y >= frame.height - ring)
                border.push_back(readPixel(frame, x, y));
        }
    }
    std::vector<double> interior;
    const int step = std::max(2, std::min(frame.width, frame.height) / 64);
    for (int y = ring; y < frame.height - ring; y += step)
        for (int x = ring; x < frame.width - ring; x += step)
            interior.push_back(readPixel(frame, x, y));
    if (border.empty() || interior.empty()) return false;

    const double bg = percentile(border, 0.50);
    const double borderP95 = percentile(border, 0.95);
    const double bodyP995 = percentile(interior, 0.995);
    const double maxCode = frame.format == PixelFormat::Mono16 ? ((1 << frame.significantBits) - 1) : 255.0;
    if (bodyP995 - bg < std::max(12.0, 0.04 * maxCode)) return false;
    if (borderP95 > bg + 0.25 * (bodyP995 - bg)) return false;

    const double thresh = bg + 0.25 * (bodyP995 - bg);
    size_t bright = 0;
    for (double v : interior) if (v > thresh) ++bright;
    const double share = static_cast<double>(bright) / static_cast<double>(interior.size());
    return share >= 0.004 && share <= 0.78;
}

// 1. Clipped-limb Center of Gravity centroid for isolated bright disks.
// Using a smoothly saturated limb weight makes the centroid insensitive to
// interior flashes or small dark transits while preserving ~0.05 px edge precision.
class DiskCogEstimator
{
public:
    bool init(const Frame& frame)
    {
        const auto m = measureCentroid(frame);
        if (!m.valid) return false;
        refCx_ = m.cx;
        refCy_ = m.cy;
        refWeight_ = m.weight;
        initialized_ = true;
        return true;
    }

    ShiftMeasurement measure(const Frame& frame) const
    {
        const auto t0 = std::chrono::steady_clock::now();
        ShiftMeasurement out;
        if (!initialized_) { out.reason = "uninitialized"; return out; }
        const auto m = measureCentroid(frame);
        out.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!m.valid) { out.reason = m.reason; return out; }
        const double areaRatio = refWeight_ > 0 ? m.weight / refWeight_ : 0.0;
        if (areaRatio < 0.55 || areaRatio > 1.65)
        {
            out.reason = "disk area changed drastically (clouds or clipped limb)";
            out.score = std::max(0.0, 1.0 - std::abs(1.0 - areaRatio));
            return out;
        }
        out.valid = true;
        out.dx = m.cx - refCx_;
        out.dy = m.cy - refCy_;
        out.score = std::clamp(1.0 - std::abs(1.0 - areaRatio), 0.0, 1.0);
        return out;
    }

    void recenterAnchor(int stepX, int stepY)
    {
        refCx_ += stepX;
        refCy_ += stepY;
    }

private:
    struct Centroid
    {
        bool valid{false};
        double cx{0}, cy{0}, weight{0};
        std::string reason;
    };

    static Centroid measureCentroid(const Frame& frame)
    {
        Centroid c;
        const bool bayer = frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
        const int stride = (frame.width > 640 || frame.height > 640 || bayer) ? 2 : 1;

        std::vector<double> border;
        border.reserve(static_cast<size_t>(frame.width + frame.height) * 2 / stride + 16);
        const int ring = std::min(4, std::min(frame.width, frame.height) / 8);
        for (int x = 0; x < frame.width; x += stride * 2)
        {
            border.push_back(readPixel(frame, x, 0));
            border.push_back(readPixel(frame, x, frame.height - 1));
        }
        for (int y = ring; y < frame.height - ring; y += stride * 2)
        {
            border.push_back(readPixel(frame, 0, y));
            border.push_back(readPixel(frame, frame.width - 1, y));
        }
        const double bg = percentile(border, 0.50);

        std::vector<double> grid;
        const int gStep = std::max(stride, std::min(frame.width, frame.height) / 48);
        grid.reserve(2500);
        for (int y = 0; y < frame.height; y += gStep)
            for (int x = 0; x < frame.width; x += gStep)
                grid.push_back(readPixel(frame, x, y));
        const double hi = percentile(grid, 0.995);
        const double maxCode = frame.format == PixelFormat::Mono16 ? ((1 << frame.significantBits) - 1) : 255.0;
        if (hi - bg < std::max(12.0, 0.04 * maxCode))
        {
            c.reason = "disk contrast too low";
            return c;
        }
        const double thresh = bg + 0.25 * (hi - bg);
        const double invRamp = 1.0 / std::max(4.0, 0.20 * (hi - thresh));

        double sumW = 0.0, sumWX = 0.0, sumWY = 0.0;
        for (int y = 0; y < frame.height; y += stride)
        {
            for (int x = 0; x < frame.width; x += stride)
            {
                const double v = readPixel(frame, x, y);
                if (v <= thresh) continue;
                const double w = std::min(1.0, (v - thresh) * invRamp);
                sumW += w;
                sumWX += w * x;
                sumWY += w * y;
            }
        }
        if (sumW < 12.0)
        {
            c.reason = "too few bright disk pixels";
            return c;
        }
        c.cx = sumWX / sumW;
        c.cy = sumWY / sumW;
        c.weight = sumW;
        if (c.cx < 4.0 || c.cx > frame.width - 5.0 || c.cy < 4.0 || c.cy > frame.height - 5.0)
        {
            c.reason = "disk centroid on frame border";
            return c;
        }
        c.valid = true;
        return c;
    }

    bool initialized_{false};
    double refCx_{0}, refCy_{0}, refWeight_{0};
};

// 2. Auto-picked FeatureLock NCC estimator with anchor-preserving template refresh.
class AutoFeatureLockEstimator
{
public:
    bool init(const Frame& frame, std::string& reason)
    {
        const int side = std::clamp(std::min(frame.width, frame.height) / 4, 64, 128);
        const int box = (side / 2) * 2;
        const int margin = std::clamp(std::min(frame.width, frame.height) / 8, 24, 96);
        if (frame.width < box + 2 * margin || frame.height < box + 2 * margin)
        {
            const int fallbackMargin = std::max(8, std::min(frame.width - box, frame.height - box) / 4);
            if (frame.width < box + 2 * fallbackMargin || frame.height < box + 2 * fallbackMargin)
            {
                reason = "frame too small for FeatureLock";
                return false;
            }
            return pickBestBox(frame, box, fallbackMargin, reason);
        }
        return pickBestBox(frame, box, margin, reason);
    }

    ShiftMeasurement measure(const Frame& frame, double expectedTotalDx, double expectedTotalDy, int radiusPx) const
    {
        const auto t0 = std::chrono::steady_clock::now();
        ShiftMeasurement out;
        if (!lock_) { out.reason = "uninitialized"; return out; }
        const LockMatch m = lock_->find(frame, expectedTotalDx - anchorDx_, expectedTotalDy - anchorDy_, radiusPx);
        out.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        out.valid = m.valid;
        out.dx = anchorDx_ + m.dx;
        out.dy = anchorDy_ + m.dy;
        out.score = m.score;
        out.reason = m.reason;
        return out;
    }

    // Refreshes the template at the current validated scene position while
    // preserving the exact sub-pixel anchor (zero jump across multi-hour rotation).
    bool refreshTemplate(const Frame& frame, double currentTotalDx, double currentTotalDy)
    {
        if (!lock_) return false;
        const bool bayer = frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
        int shiftX = bayer ? 2 * static_cast<int>(std::lround((currentTotalDx - anchorDx_) / 2.0))
                           : static_cast<int>(std::lround(currentTotalDx - anchorDx_));
        int shiftY = bayer ? 2 * static_cast<int>(std::lround((currentTotalDy - anchorDy_) / 2.0))
                           : static_cast<int>(std::lround(currentTotalDy - anchorDy_));
        LockRegion next = lock_->region();
        next.x += shiftX;
        next.y += shiftY;
        if (next.x < 16 || next.y < 16 || next.x + next.width > frame.width - 16 || next.y + next.height > frame.height - 16)
            return false;
        std::string refusal;
        auto fresh = FeatureLock::take(frame, next, refusal);
        if (!fresh) return false;
        lock_ = std::move(fresh);
        anchorDx_ = currentTotalDx;
        anchorDy_ = currentTotalDy;
        return true;
    }

    void recenterAnchor(int stepX, int stepY)
    {
        anchorDx_ -= stepX;
        anchorDy_ -= stepY;
    }

    [[nodiscard]] LockRegion region() const { return lock_ ? lock_->region() : LockRegion{}; }

private:
    bool pickBestBox(const Frame& frame, int box, int margin, std::string& reason)
    {
        struct Candidate { LockRegion r; double score; };
        std::vector<Candidate> cands;
        const int spanX = frame.width - 2 * margin - box;
        const int spanY = frame.height - 2 * margin - box;
        const int nx = std::max(1, std::min(7, spanX / (box / 2) + 1));
        const int ny = std::max(1, std::min(7, spanY / (box / 2) + 1));
        const double maxCode = frame.format == PixelFormat::Mono16 ? ((1 << frame.significantBits) - 1) : 255.0;
        for (int gy = 0; gy < ny; ++gy)
        {
            for (int gx = 0; gx < nx; ++gx)
            {
                int rx = margin + (nx > 1 ? gx * spanX / (nx - 1) : 0);
                int ry = margin + (ny > 1 ? gy * spanY / (ny - 1) : 0);
                rx = (rx / 2) * 2;
                ry = (ry / 2) * 2;
                double sum = 0.0, sumSq = 0.0;
                int count = 0, sat = 0;
                for (int y = ry; y < ry + box; y += 4)
                {
                    for (int x = rx; x < rx + box; x += 4)
                    {
                        const double v = readPixel(frame, x, y);
                        sum += v; sumSq += v * v; ++count;
                        if (v >= maxCode - 1.0) ++sat;
                    }
                }
                if (count == 0 || static_cast<double>(sat) / count > 0.15) continue;
                const double m = sum / count;
                const double var = std::max(0.0, sumSq / count - m * m);
                cands.push_back({{rx, ry, box, box}, var});
            }
        }
        std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        std::string lastRefusal = "no candidate boxes";
        for (const auto& c : cands)
        {
            auto locked = FeatureLock::take(frame, c.r, lastRefusal);
            if (locked)
            {
                lock_ = std::move(locked);
                anchorDx_ = 0.0;
                anchorDy_ = 0.0;
                return true;
            }
        }
        reason = lastRefusal;
        return false;
    }

    std::optional<FeatureLock> lock_;
    double anchorDx_{0.0}, anchorDy_{0.0};
};

// 3. Downsampled 2D Phase Correlation estimator using OpenCV (cv::phaseCorrelate).
class PhaseCorrEstimator
{
public:
    explicit PhaseCorrEstimator(int targetSize = 256) : targetSize_(targetSize) {}

    bool init(const Frame& frame)
    {
        const bool bayer = frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
        const int minSide = std::min(frame.width, frame.height);
        ds_ = std::max(1, minSide / targetSize_);
        if (bayer && ds_ % 2 != 0) ds_ = std::max(2, ds_ + 1);
        const int availW = frame.width / ds_;
        const int availH = frame.height / ds_;
        w_ = cv::getOptimalDFTSize(std::min(targetSize_, availW));
        h_ = cv::getOptimalDFTSize(std::min(targetSize_, availH));
        if (w_ > availW) w_ = availW;
        if (h_ > availH) h_ = availH;
        if (w_ < 32 || h_ < 32) return false;

        roiX_ = ((frame.width - w_ * ds_) / 2 / 2) * 2;
        roiY_ = ((frame.height - h_ * ds_) / 2 / 2) * 2;
        ref_.create(h_, w_, CV_32FC1);
        cur_.create(h_, w_, CV_32FC1);
        cv::createHanningWindow(hann_, cv::Size(w_, h_), CV_32F);
        fillBuffer(frame, ref_);
        anchorDx_ = 0.0;
        anchorDy_ = 0.0;
        initialized_ = true;
        return true;
    }

    ShiftMeasurement measure(const Frame& frame)
    {
        const auto t0 = std::chrono::steady_clock::now();
        ShiftMeasurement out;
        if (!initialized_) { out.reason = "uninitialized"; return out; }
        fillBuffer(frame, cur_);
        double response = 0.0;
        const cv::Point2d shift = cv::phaseCorrelate(ref_, cur_, hann_, &response);
        out.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        out.score = response;
        if (!std::isfinite(shift.x) || !std::isfinite(shift.y) || response < 0.12)
        {
            out.reason = "phase correlation response too low";
            return out;
        }
        out.valid = true;
        out.dx = anchorDx_ + shift.x * ds_;
        out.dy = anchorDy_ + shift.y * ds_;
        return out;
    }

    void refreshReference(const Frame& frame, double curTotalDx, double curTotalDy)
    {
        if (!initialized_) return;
        fillBuffer(frame, ref_);
        anchorDx_ = curTotalDx;
        anchorDy_ = curTotalDy;
    }

    void recenterAnchor(int stepX, int stepY)
    {
        anchorDx_ -= stepX;
        anchorDy_ -= stepY;
    }

private:
    void fillBuffer(const Frame& frame, cv::Mat& dst) const
    {
        const float inv = 1.0f / static_cast<float>(ds_ * ds_);
        for (int y = 0; y < h_; ++y)
        {
            float* row = dst.ptr<float>(y);
            const int sy0 = roiY_ + y * ds_;
            if (ds_ == 1)
            {
                for (int x = 0; x < w_; ++x) row[x] = readPixel(frame, roiX_ + x, sy0);
            }
            else
            {
                for (int x = 0; x < w_; ++x)
                {
                    const int sx0 = roiX_ + x * ds_;
                    float sum = 0.0f;
                    for (int dy = 0; dy < ds_; ++dy)
                        for (int dx = 0; dx < ds_; ++dx)
                            sum += readPixel(frame, sx0 + dx, sy0 + dy);
                    row[x] = sum * inv;
                }
            }
        }
    }

    int targetSize_{256};
    int ds_{1}, w_{0}, h_{0}, roiX_{0}, roiY_{0};
    cv::Mat ref_, cur_, hann_;
    double anchorDx_{0.0}, anchorDy_{0.0};
    bool initialized_{false};
};

// --- 3-Tier Stabilization State Machine (Feature-50) ---

enum class StabilizerState { Uninitialized, Locked, GraceHold, LostFallback };

const char* stateName(StabilizerState s)
{
    switch (s)
    {
    case StabilizerState::Uninitialized: return "uninitialized";
    case StabilizerState::Locked: return "locked";
    case StabilizerState::GraceHold: return "grace_hold";
    case StabilizerState::LostFallback: return "lost_fallback";
    }
    return "unknown";
}

struct StabilizerStep
{
    StabilizerState state{StabilizerState::Uninitialized};
    double dx{0.0}, dy{0.0};
    double score{0.0};
    double estimatorMs{0.0};
    bool applyShift{false};
    bool resetDetector{false};
    int recenterStepX{0};
    int recenterStepY{0};
    std::string event; // registration_locked / registration_lost / recenter / template_refresh
};

class ShiftStabilizer
{
public:
    double maxStepPx{12.0};
    double graceSeconds{0.5};
    int reacquireFrames{3};
    double recenterMarginPx{32.0};
    double templateRefreshSeconds{60.0};

    explicit ShiftStabilizer(EstimatorKind requested = EstimatorKind::Auto) : requested_(requested) {}

    [[nodiscard]] EstimatorKind activeEstimator() const { return active_; }
    [[nodiscard]] LockRegion lockRegion() const { return lockEst_.region(); }

    StabilizerStep step(const Frame& frame, double frameIntervalSec)
    {
        StabilizerStep out;
        if (state_ == StabilizerState::Uninitialized)
        {
            initOnFrame(frame);
            if (initialized_)
            {
                state_ = StabilizerState::Locked;
                lastDx_ = lastDy_ = 0.0;
                sinceRefreshSec_ = 0.0;
                out.state = state_;
                out.applyShift = true;
                out.event = "registration_locked";
            }
            else
            {
                state_ = StabilizerState::LostFallback;
                out.state = state_;
                out.applyShift = false;
                out.event = "registration_lost reason=init_failed";
            }
            return out;
        }

        const int searchRadius = (state_ == StabilizerState::Locked) ? 16 : 32;
        ShiftMeasurement m = runEstimator(frame, lastDx_, lastDy_, searchRadius);
        out.estimatorMs = m.elapsedMs;
        out.score = m.score;

        const bool plausible = m.valid && (state_ == StabilizerState::LostFallback
            || std::hypot(m.dx - lastDx_, m.dy - lastDy_) <= maxStepPx);

        if (state_ == StabilizerState::Locked || state_ == StabilizerState::GraceHold)
        {
            if (plausible)
            {
                state_ = StabilizerState::Locked;
                graceElapsedSec_ = 0.0;
                lastDx_ = m.dx;
                lastDy_ = m.dy;
                sinceRefreshSec_ += frameIntervalSec;

                // Tier 3: In-place recenter if accumulated drift exceeds margin.
                if (std::abs(lastDx_) >= recenterMarginPx || std::abs(lastDy_) >= recenterMarginPx)
                {
                    const bool bayer = frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
                    int sx = bayer ? 2 * static_cast<int>(std::trunc(lastDx_ / 2.0)) : static_cast<int>(std::trunc(lastDx_));
                    int sy = bayer ? 2 * static_cast<int>(std::trunc(lastDy_ / 2.0)) : static_cast<int>(std::trunc(lastDy_));
                    if (sx != 0 || sy != 0)
                    {
                        lastDx_ -= sx;
                        lastDy_ -= sy;
                        recenterAnchors(sx, sy);
                        out.recenterStepX = sx;
                        out.recenterStepY = sy;
                        out.event = "recenter";
                    }
                }

                // Periodic template refresh (handles multi-hour rotation & illumination change).
                if (active_ == EstimatorKind::FeatureLock
                    && (sinceRefreshSec_ >= templateRefreshSeconds || (m.score >= 0.72 && m.score < 0.85 && sinceRefreshSec_ >= 5.0)))
                {
                    if (lockEst_.refreshTemplate(frame, lastDx_, lastDy_))
                    {
                        sinceRefreshSec_ = 0.0;
                        if (out.event.empty()) out.event = "template_refresh";
                    }
                }
                else if (active_ == EstimatorKind::PhaseCorr && sinceRefreshSec_ >= templateRefreshSeconds)
                {
                    phaseEst_.refreshReference(frame, lastDx_, lastDy_);
                    sinceRefreshSec_ = 0.0;
                    if (out.event.empty()) out.event = "template_refresh";
                }

                out.state = state_;
                out.dx = lastDx_;
                out.dy = lastDy_;
                out.applyShift = true;
                return out;
            }

            // Tier 1: Transient loss -> reject bad shift and hold last valid (dx, dy).
            graceElapsedSec_ += frameIntervalSec;
            if (graceElapsedSec_ <= graceSeconds)
            {
                state_ = StabilizerState::GraceHold;
                out.state = state_;
                out.dx = lastDx_;
                out.dy = lastDy_;
                out.applyShift = true;
                return out;
            }

            // Tier 2: Sustained loss -> cleanly reset detector to unstabilized fallback.
            state_ = StabilizerState::LostFallback;
            reacquireStreak_ = 0;
            lastDx_ = lastDy_ = 0.0;
            out.state = state_;
            out.applyShift = false;
            out.resetDetector = true;
            out.event = "registration_lost reason=" + (m.reason.empty() ? "jump_rejected" : m.reason);
            return out;
        }

        // LostFallback state: try to re-acquire cleanly.
        if (!initialized_) initOnFrame(frame);
        if (initialized_)
        {
            if (m.valid && (reacquireStreak_ == 0 || std::hypot(m.dx - reacquireDx_, m.dy - reacquireDy_) <= maxStepPx))
            {
                ++reacquireStreak_;
                reacquireDx_ = m.dx;
                reacquireDy_ = m.dy;
            }
            else if (!m.valid && active_ != EstimatorKind::DiskCoG)
            {
                // Re-anchor template/phase on the newly cleared frame before counting matches.
                reacquireStreak_ = 0;
                initOnFrame(frame);
            }
            else
            {
                reacquireStreak_ = 0;
            }
            if (reacquireStreak_ >= reacquireFrames)
            {
                // Re-anchor cleanly at (0, 0) on this frame so the detector starts fresh and aligned.
                initOnFrame(frame);
                if (initialized_)
                {
                    state_ = StabilizerState::Locked;
                    lastDx_ = lastDy_ = 0.0;
                    graceElapsedSec_ = 0.0;
                    sinceRefreshSec_ = 0.0;
                    reacquireStreak_ = 0;
                    out.state = state_;
                    out.applyShift = true;
                    out.resetDetector = true;
                    out.event = "registration_locked";
                    return out;
                }
            }
        }
        out.state = StabilizerState::LostFallback;
        out.applyShift = false;
        return out;
    }

private:
    void initOnFrame(const Frame& frame)
    {
        initialized_ = false;
        std::string reason;
        if (requested_ == EstimatorKind::DiskCoG)
        {
            active_ = EstimatorKind::DiskCoG;
            initialized_ = cogEst_.init(frame);
            return;
        }
        if (requested_ == EstimatorKind::FeatureLock)
        {
            active_ = EstimatorKind::FeatureLock;
            initialized_ = lockEst_.init(frame, reason);
            return;
        }
        if (requested_ == EstimatorKind::PhaseCorr)
        {
            active_ = EstimatorKind::PhaseCorr;
            initialized_ = phaseEst_.init(frame);
            return;
        }
        // Auto selection: isolated disk -> DiskCoG, else FeatureLock -> PhaseCorr fallback.
        if (isIsolatedDisk(frame) && cogEst_.init(frame))
        {
            active_ = EstimatorKind::DiskCoG;
            initialized_ = true;
            return;
        }
        if (lockEst_.init(frame, reason))
        {
            active_ = EstimatorKind::FeatureLock;
            initialized_ = true;
            return;
        }
        if (phaseEst_.init(frame))
        {
            active_ = EstimatorKind::PhaseCorr;
            initialized_ = true;
            return;
        }
    }

    ShiftMeasurement runEstimator(const Frame& frame, double expDx, double expDy, int radiusPx)
    {
        switch (active_)
        {
        case EstimatorKind::DiskCoG: return cogEst_.measure(frame);
        case EstimatorKind::FeatureLock: return lockEst_.measure(frame, expDx, expDy, radiusPx);
        case EstimatorKind::PhaseCorr: return phaseEst_.measure(frame);
        case EstimatorKind::Auto: break;
        }
        return {};
    }

    void recenterAnchors(int sx, int sy)
    {
        switch (active_)
        {
        case EstimatorKind::DiskCoG: cogEst_.recenterAnchor(sx, sy); break;
        case EstimatorKind::FeatureLock: lockEst_.recenterAnchor(sx, sy); break;
        case EstimatorKind::PhaseCorr: phaseEst_.recenterAnchor(sx, sy); break;
        case EstimatorKind::Auto: break;
        }
    }

    EstimatorKind requested_{EstimatorKind::Auto};
    EstimatorKind active_{EstimatorKind::Auto};
    bool initialized_{false};
    StabilizerState state_{StabilizerState::Uninitialized};
    DiskCogEstimator cogEst_;
    AutoFeatureLockEstimator lockEst_;
    PhaseCorrEstimator phaseEst_;
    double lastDx_{0.0}, lastDy_{0.0};
    double graceElapsedSec_{0.0};
    double sinceRefreshSec_{0.0};
    int reacquireStreak_{0};
    double reacquireDx_{0.0}, reacquireDy_{0.0};
};

// --- Helper for injecting a synthetic Gaussian flash onto a frame ---

void paintFlash(Frame& frame, double cx, double cy, double sigmaPx, double amplitudeAdu)
{
    const int r = static_cast<int>(std::ceil(3.5 * sigmaPx));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx)) - r);
    const int x1 = std::min(frame.width - 1, static_cast<int>(std::ceil(cx)) + r);
    const int y0 = std::max(0, static_cast<int>(std::floor(cy)) - r);
    const int y1 = std::min(frame.height - 1, static_cast<int>(std::ceil(cy)) + r);
    const double inv2s2 = 1.0 / (2.0 * sigmaPx * sigmaPx);
    const double maxCode = frame.format == PixelFormat::Mono16 ? ((1 << frame.significantBits) - 1) : 255.0;
    const int bitShift = frame.format == PixelFormat::Mono16 && frame.alignment == BitAlignment::High
        ? 16 - frame.significantBits : 0;

    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            const double d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            const double add = amplitudeAdu * std::exp(-d2 * inv2s2);
            if (add < 0.25) continue;
            const size_t i = static_cast<size_t>(y) * frame.width + x;
            switch (frame.format)
            {
            case PixelFormat::Mono8:
                frame.data[i] = static_cast<uint8_t>(std::clamp(std::lround(frame.data[i] + add), 0L, 255L));
                break;
            case PixelFormat::Mono16:
            {
                uint16_t raw = 0;
                std::memcpy(&raw, frame.data.data() + 2 * i, 2);
                const double val = static_cast<double>(raw >> bitShift) + add;
                const uint16_t clamped = static_cast<uint16_t>(std::clamp(std::lround(val), 0L, static_cast<long>(maxCode))) << bitShift;
                std::memcpy(frame.data.data() + 2 * i, &clamped, 2);
                break;
            }
            case PixelFormat::RGB888:
                for (int c = 0; c < 3; ++c)
                    frame.data[3 * i + c] = static_cast<uint8_t>(std::clamp(std::lround(frame.data[3 * i + c] + add), 0L, 255L));
                break;
            }
        }
    }
}

// --- Subcommand 1: Real recording benchmark ---

int runRecordingBench(const QStringList& args)
{
    QTextStream out(stdout), err(stderr);
    if (args.size() < 5)
    {
        err << "Usage: SeleneWatchProbe stabilize-bench recording INPUT OUTPUT.json [--watch shadows|flashes|planet] [--estimator auto|lock|cog|phase] [--limit N] [--inject-flash] [--inject-shadow]\n";
        return 2;
    }
    const QString inputPath = args[3];
    const QString outputPath = args[4];
    QString watchStr = "shadows";
    EstimatorKind estKind = EstimatorKind::Auto;
    int frameLimit = 0;
    bool injectFlash = false;
    bool injectShadow = false;

    for (int i = 5; i < args.size(); ++i)
    {
        if (args[i] == "--watch" && i + 1 < args.size()) watchStr = args[++i];
        else if (args[i] == "--estimator" && i + 1 < args.size())
        {
            const QString e = args[++i];
            if (e == "lock") estKind = EstimatorKind::FeatureLock;
            else if (e == "cog") estKind = EstimatorKind::DiskCoG;
            else if (e == "phase") estKind = EstimatorKind::PhaseCorr;
            else estKind = EstimatorKind::Auto;
        }
        else if (args[i] == "--limit" && i + 1 < args.size()) frameLimit = args[++i].toInt();
        else if (args[i] == "--inject-flash") injectFlash = true;
        else if (args[i] == "--inject-shadow") injectShadow = true;
    }

    std::string error;
    auto reader = openRecordingReader(std::filesystem::path(inputPath.toStdWString()), 30, error);
    if (!reader) { err << "Cannot open " << inputPath << ": " << QString::fromStdString(error) << "\n"; return 1; }

    const int totalFrames = frameLimit > 0 ? std::min(frameLimit, reader->frameCount()) : reader->frameCount();
    if (totalFrames < 5) { err << "Recording has too few frames\n"; return 1; }

    DetectionSettings baseSettings;
    if (watchStr == "shadows")
    {
        baseSettings = defaultDetectionSettings(Watch::MoonShadows);
    }
    else if (watchStr == "flashes")
    {
        baseSettings = defaultDetectionSettings(Watch::MoonFlashes);
        baseSettings.ignoreAboveShare = 0.0; // Benchmark full field unless sunlit earthshine ceiling needed
    }
    else // planet (flashes on planetary disk)
    {
        baseSettings = defaultDetectionSettings(Watch::MoonFlashes);
        baseSettings.ignoreAboveShare = 0.0;
        baseSettings.minPixels = 3;
        baseSettings.oneFrame = false;
    }

    const std::vector<double> kValues = {6.0, 8.0, 12.0, 18.0};
    const std::vector<ShiftMode> modes = {ShiftMode::Off, ShiftMode::Integer, ShiftMode::Subpixel};
    const char* modeNames[3] = {"off", "integer", "subpixel"};

    struct RunStats
    {
        std::unique_ptr<FlashDetector> det;
        int totalTriggers{0};
        int postSettlingTriggers{0};
        std::vector<int> triggerFrames;
        std::vector<double> peakSigmas;
        std::vector<double> noiseFloors;
        std::vector<double> detMs;
        // Injection recovery tracking
        int injectedCount{0};
        int recoveredCount{0};
    };

    // [modeIdx][kIdx]
    std::vector<std::vector<RunStats>> stats(3);
    for (size_t m = 0; m < 3; ++m)
    {
        stats[m].resize(kValues.size());
        for (size_t ki = 0; ki < kValues.size(); ++ki)
        {
            DetectionSettings s = baseSettings;
            s.sigma = kValues[ki];
            stats[m][ki].det = std::make_unique<FlashDetector>(s);
        }
    }

    ShiftStabilizer stabilizer(estKind);
    std::vector<double> estMs;
    std::vector<double> stepMagnitudes;
    double prevDx = 0.0, prevDy = 0.0;
    bool hasPrevShift = false;
    double minDx = 0, maxDx = 0, minDy = 0, maxDy = 0;
    int lockedFrames = 0, graceFrames = 0, lostFrames = 0, recenters = 0, refreshes = 0;

    Frame frame;
    frame.data.resize(reader->frameBytes());
    std::chrono::system_clock::time_point t0{};
    std::chrono::system_clock::time_point prevTime{};

    // Pick injection schedule if requested (every 40 frames starting at frame 60, on interior target).
    struct InjectedEvent { int startFrame; int duration; double cx; double cy; double amp; };
    std::vector<InjectedEvent> injections;
    double injectCx = 0, injectCy = 0;

    for (int i = 0; i < totalFrames; ++i)
    {
        if (!reader->readFrame(i, frame, error)) { err << "Read error at frame " << i << "\n"; return 1; }
        frame.frameIndex = static_cast<uint64_t>(i);
        frame.timestamp = reader->frameTime(i);
        if (i == 0)
        {
            t0 = frame.timestamp;
            prevTime = frame.timestamp;
            // Find a bright interior pixel for optional injection (center of disk/bright terrain).
            double sumW = 0, sumX = 0, sumY = 0;
            for (int y = frame.height / 4; y < 3 * frame.height / 4; y += 4)
            {
                for (int x = frame.width / 4; x < 3 * frame.width / 4; x += 4)
                {
                    const double v = readPixel(frame, x, y);
                    sumW += v; sumX += v * x; sumY += v * y;
                }
            }
            injectCx = sumW > 0 ? sumX / sumW : frame.width * 0.5;
            injectCy = sumW > 0 ? sumY / sumW : frame.height * 0.5;

            if (injectFlash || injectShadow)
            {
                const std::vector<double> amps = injectFlash
                    ? std::vector<double>{15.0, 25.0, 40.0, 60.0}
                    : std::vector<double>{0.25, 0.45, 0.70, 0.90};
                int f = 60;
                for (size_t idx = 0; f + 10 < totalFrames && idx < 16; ++idx, f += 40)
                    injections.push_back({f, injectFlash ? 2 : 1, injectCx, injectCy, amps[idx % amps.size()]});
            }
        }
        double dt = std::chrono::duration<double>(frame.timestamp - prevTime).count();
        if (!(dt > 0.0) || dt > 1.0) dt = 1.0 / 30.0;
        prevTime = frame.timestamp;
        const double elapsedSec = std::chrono::duration<double>(frame.timestamp - t0).count();

        // Step stabilizer on raw frame BEFORE any optional injection (so injection tests detector sensitivity).
        const StabilizerStep st = stabilizer.step(frame, dt);
        if (i > 0) estMs.push_back(st.estimatorMs);
        if (st.state == StabilizerState::Locked) ++lockedFrames;
        else if (st.state == StabilizerState::GraceHold) ++graceFrames;
        else if (st.state == StabilizerState::LostFallback) ++lostFrames;
        if (st.recenterStepX != 0 || st.recenterStepY != 0) ++recenters;
        if (st.event == "template_refresh") ++refreshes;

        if (st.applyShift)
        {
            if (hasPrevShift && st.recenterStepX == 0 && st.recenterStepY == 0)
                stepMagnitudes.push_back(std::hypot(st.dx - prevDx, st.dy - prevDy));
            prevDx = st.dx; prevDy = st.dy; hasPrevShift = true;
            minDx = std::min(minDx, st.dx); maxDx = std::max(maxDx, st.dx);
            minDy = std::min(minDy, st.dy); maxDy = std::max(maxDy, st.dy);
        }
        else hasPrevShift = false;

        // Apply optional synthetic injection at target-fixed coordinate (cx + dx, cy + dy).
        int activeInjIdx = -1;
        for (size_t idx = 0; idx < injections.size(); ++idx)
        {
            const auto& inj = injections[idx];
            if (i >= inj.startFrame && i < inj.startFrame + inj.duration)
            {
                activeInjIdx = static_cast<int>(idx);
                const double tx = std::clamp(inj.cx + st.dx, 8.0, frame.width - 9.0);
                const double ty = std::clamp(inj.cy + st.dy, 8.0, frame.height - 9.0);
                if (injectFlash) paintFlash(frame, tx, ty, 1.5, inj.amp);
                else if (injectShadow) paintIssSilhouette(frame, tx, ty, 36.0, inj.amp, 25.0, 4.0);
            }
        }

        for (size_t m = 0; m < 3; ++m)
        {
            FrameShift shift;
            if (modes[m] != ShiftMode::Off && st.applyShift)
                shift = {modes[m], st.dx, st.dy};

            for (size_t ki = 0; ki < kValues.size(); ++ki)
            {
                auto& rs = stats[m][ki];
                if (modes[m] != ShiftMode::Off)
                {
                    if (st.resetDetector) rs.det->reset(false);
                    else if (st.recenterStepX != 0 || st.recenterStepY != 0)
                        rs.det->recenterReference(st.recenterStepX, st.recenterStepY, frame, shift);
                }
                const bool trig = rs.det->analyze(frame, {}, shift);
                rs.det->updateReference(frame);

                const auto& sum = rs.det->summary();
                if (i > 0)
                {
                    rs.detMs.push_back(sum.processingMs);
                    if (elapsedSec >= 2.0)
                    {
                        rs.peakSigmas.push_back(sum.peakSigma);
                        rs.noiseFloors.push_back(sum.noiseFloor);
                    }
                }
                if (trig)
                {
                    ++rs.totalTriggers;
                    if (elapsedSec >= 2.0)
                    {
                        ++rs.postSettlingTriggers;
                        if (rs.triggerFrames.size() < 30) rs.triggerFrames.push_back(i);
                    }
                    if (activeInjIdx >= 0) ++rs.recoveredCount;
                }
            }
        }
    }

    const double rmsStep = stepMagnitudes.empty() ? 0.0 : [&] {
        double s = 0.0;
        for (double v : stepMagnitudes) s += v * v;
        return std::sqrt(s / static_cast<double>(stepMagnitudes.size()));
    }();

    QJsonObject root;
    root["input"] = inputPath;
    root["watch"] = watchStr;
    root["width"] = frame.width;
    root["height"] = frame.height;
    root["frames"] = totalFrames;
    root["format"] = frame.format == PixelFormat::Mono8 ? "Mono8" : frame.format == PixelFormat::Mono16 ? "Mono16" : "RGB888";
    root["bits"] = frame.significantBits;
    root["bayer"] = frame.bayer != BayerPattern::None;
    root["estimator"] = estimatorName(stabilizer.activeEstimator());
    root["locked_frames"] = lockedFrames;
    root["grace_frames"] = graceFrames;
    root["lost_frames"] = lostFrames;
    root["recenters"] = recenters;
    root["refreshes"] = refreshes;
    root["rms_step_px"] = rmsStep;
    root["drift_span_x_px"] = maxDx - minDx;
    root["drift_span_y_px"] = maxDy - minDy;
    root["estimator_mean_ms"] = meanOf(estMs);
    root["estimator_p95_ms"] = percentile(estMs, 0.95);
    root["injected_events"] = static_cast<int>(injections.size());

    QJsonObject modesJson;
    for (size_t m = 0; m < 3; ++m)
    {
        QJsonArray kArr;
        for (size_t ki = 0; ki < kValues.size(); ++ki)
        {
            const auto& rs = stats[m][ki];
            QJsonObject ko;
            ko["k"] = kValues[ki];
            ko["total_triggers"] = rs.totalTriggers;
            ko["post_2s_triggers"] = rs.postSettlingTriggers;
            ko["median_peak_sigma"] = percentile(rs.peakSigmas, 0.50);
            ko["p95_peak_sigma"] = percentile(rs.peakSigmas, 0.95);
            ko["median_noise_floor"] = percentile(rs.noiseFloors, 0.50);
            ko["det_mean_ms"] = meanOf(rs.detMs);
            ko["det_p95_ms"] = percentile(rs.detMs, 0.95);
            ko["injected_recovered"] = rs.recoveredCount;
            QJsonArray tf;
            for (int f : rs.triggerFrames) tf.append(f);
            ko["trigger_frames"] = tf;
            kArr.append(ko);
        }
        modesJson[modeNames[m]] = kArr;
    }
    root["modes"] = modesJson;

    QFile f(outputPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

    out << QString("=== %1 (%2x%3, %4 frames, watch=%5, est=%6) ===\n")
        .arg(QString::fromStdWString(std::filesystem::path(inputPath.toStdWString()).filename().wstring()))
        .arg(frame.width).arg(frame.height).arg(totalFrames).arg(watchStr).arg(estimatorName(stabilizer.activeEstimator()));
    out << QString("Lock: %1/%2 locked, %3 grace, %4 lost, %5 recenter | rmsStep=%6 px, drift=(%7,%8) px | est=%9 ms (p95 %10)\n")
        .arg(lockedFrames).arg(totalFrames).arg(graceFrames).arg(lostFrames).arg(recenters)
        .arg(rmsStep, 0, 'f', 2).arg(maxDx - minDx, 0, 'f', 1).arg(maxDy - minDy, 0, 'f', 1)
        .arg(meanOf(estMs), 0, 'f', 2).arg(percentile(estMs, 0.95), 0, 'f', 2);
    for (size_t m = 0; m < 3; ++m)
    {
        out << QString("  %1:").arg(modeNames[m], -8);
        for (size_t ki = 0; ki < kValues.size(); ++ki)
        {
            const auto& rs = stats[m][ki];
            out << QString(" K=%1: trig=%2 (p95sig=%3)")
                .arg(kValues[ki], 0, 'f', 0).arg(rs.postSettlingTriggers).arg(percentile(rs.peakSigmas, 0.95), 0, 'f', 1);
            if (!injections.empty()) out << QString("[rec=%1/%2]").arg(rs.recoveredCount).arg(injections.size());
        }
        out << QString(" | det=%1 ms (p95 %2)\n")
            .arg(meanOf(stats[m][1].detMs), 0, 'f', 2).arg(percentile(stats[m][1].detMs, 0.95), 0, 'f', 2);
    }
    return 0;
}

// --- Subcommand 2: Controlled synthetic accuracy, Bayer, failure-mode & rotation tests ---

// Renders a textured lunar-like limb + craters scene shifted by exact continuous (dx, dy)
// and rotated by angleDeg around the frame center, with Gaussian read noise.
Frame renderSyntheticLunarFrame(int w, int h, PixelFormat fmt, BayerPattern bayer,
    double dx, double dy, double angleDeg, double brightnessScale, double noiseSigma, std::mt19937& rng, uint64_t idx)
{
    Frame frame;
    frame.width = w;
    frame.height = h;
    frame.format = fmt;
    frame.significantBits = fmt == PixelFormat::Mono16 ? 12 : 8;
    frame.alignment = BitAlignment::Low;
    frame.bayer = bayer;
    frame.frameIndex = idx;
    frame.timestamp = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{idx * 33};
    frame.data.resize(frameBytes(w, h, fmt));

    const double cx0 = w * 0.5;
    const double cy0 = h * 0.5;
    const double rad = angleDeg * kPi / 180.0;
    const double cA = std::cos(rad), sA = std::sin(rad);
    const double scale = fmt == PixelFormat::Mono16 ? 16.0 : 1.0;
    const double maxVal = fmt == PixelFormat::Mono16 ? 4095.0 : 255.0;
    std::normal_distribution<double> nDist(0.0, noiseSigma * scale);

    // Crater centers in scene space (relative to center)
    struct Crater { double x, y, r, depth; };
    static const Crater craters[] = {
        {-60, -45, 22, 48}, {35, -30, 18, 42}, {-20, 40, 26, 52},
        {70, 50, 16, 38}, {0, -5, 14, 45}, {-85, 25, 15, 36},
        {45, -75, 20, 40}, {-40, -85, 12, 35}, {80, -20, 19, 44}
    };

    const bool isBayer = bayer != BayerPattern::None && fmt != PixelFormat::RGB888;
    // Distinct per-channel Bayer gain to prove cross-channel interpolation would create artifacts.
    const double bayerGain[2][2] = {{1.00, 0.82}, {0.82, 0.65}};

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            // Inverse transform: scene point corresponding to frame pixel (x, y)
            const double px = x - cx0 - dx;
            const double py = y - cy0 - dy;
            const double sx = cA * px + sA * py;
            const double sy = -sA * px + cA * py;

            // Bright lunar disk with sharp sunlit limb at radius 0.38 * min(w,h)
            const double dist = std::hypot(sx, sy);
            const double limbR = 0.38 * std::min(w, h);
            const double limb = 1.0 / (1.0 + std::exp((dist - limbR) / 1.5));

            // Smooth surface albedo + sharp crater rims
            double surf = 120.0 + 18.0 * std::sin(sx * 0.07) * std::cos(sy * 0.06);
            for (const auto& cr : craters)
            {
                const double d = std::hypot(sx - cr.x, sy - cr.y);
                const double rim = std::exp(-((d - cr.r) * (d - cr.r)) / 9.0);
                const double bowl = d < cr.r ? (1.0 - (d / cr.r) * (d / cr.r)) : 0.0;
                surf += cr.depth * (0.9 * rim - 0.6 * bowl);
            }
            double val = (10.0 + limb * std::max(0.0, surf)) * brightnessScale * scale;
            if (isBayer) val *= bayerGain[y & 1][x & 1];
            val += nDist(rng);
            const long iv = std::clamp(std::lround(val), 0L, static_cast<long>(maxVal));

            const size_t i = static_cast<size_t>(y) * w + x;
            if (fmt == PixelFormat::Mono8) frame.data[i] = static_cast<uint8_t>(iv);
            else if (fmt == PixelFormat::Mono16)
            {
                const uint16_t u = static_cast<uint16_t>(iv);
                std::memcpy(frame.data.data() + 2 * i, &u, 2);
            }
            else
            {
                frame.data[3 * i] = static_cast<uint8_t>(iv);
                frame.data[3 * i + 1] = static_cast<uint8_t>(iv);
                frame.data[3 * i + 2] = static_cast<uint8_t>(iv);
            }
        }
    }
    return frame;
}

int runSyntheticBench(const QStringList& args)
{
    QTextStream out(stdout), err(stderr);
    if (args.size() < 4) { err << "Usage: SeleneWatchProbe stabilize-bench synthetic OUTPUT.json\n"; return 2; }
    const QString outputPath = args[3];
    QJsonObject root;

    // --- Test A: Sub-pixel drift + jitter across Mono8, Mono16, Bayer RGGB (flashes & shadows) ---
    QJsonArray testA;
    struct FormatCase { const char* name; PixelFormat fmt; BayerPattern bayer; };
    const FormatCase cases[] = {
        {"Mono8", PixelFormat::Mono8, BayerPattern::None},
        {"Mono16", PixelFormat::Mono16, BayerPattern::None},
        {"Bayer8_RGGB", PixelFormat::Mono8, BayerPattern::RGGB},
    };
    for (const auto& fc : cases)
    {
        for (bool shadows : {false, true})
        {
            std::mt19937 rng(42);
            std::normal_distribution<double> jitter(0.0, 0.9);
            DetectionSettings s = shadows ? defaultDetectionSettings(Watch::MoonShadows)
                                          : defaultDetectionSettings(Watch::MoonFlashes);
            s.ignoreAboveShare = 0.0;
            s.sigma = shadows ? 10.0 : 8.0;
            if (shadows) { s.minPixels = 10; s.oneFrame = true; }
            else { s.minPixels = 4; s.oneFrame = false; }

            FlashDetector dOff(s), dInt(s), dSub(s);
            ShiftStabilizer stab(EstimatorKind::Auto);
            int falseOff = 0, falseInt = 0, falseSub = 0;
            bool recOff = false, recInt = false, recSub = false;
            double coordErrSub = -1.0;
            double shiftErrSum = 0.0;
            int shiftErrCount = 0;

            const int W = 320, H = 240;
            const double eventSceneX = 160.0 + 18.0, eventSceneY = 120.0 - 14.0;
            for (int f = 0; f < 140; ++f)
            {
                // Smooth drift + seeing jitter (sub-pixel)
                const double trueDx = f == 0 ? 0.0 : 0.12 * f + jitter(rng);
                const double trueDy = f == 0 ? 0.0 : -0.08 * f + jitter(rng);
                Frame frame = renderSyntheticLunarFrame(W, H, fc.fmt, fc.bayer, trueDx, trueDy, 0.0, 1.0, 1.2, rng, f);
                const StabilizerStep st = stab.step(frame, 1.0 / 30.0);
                if (f > 0 && st.applyShift)
                {
                    shiftErrSum += std::hypot(st.dx - trueDx, st.dy - trueDy);
                    ++shiftErrCount;
                }

                // Inject real event on frames 110..111 (flash) or 110 (shadow) at scene-fixed spot
                const bool eventFrame = shadows ? (f == 110) : (f == 110 || f == 111);
                const double trueNatX = eventSceneX + trueDx;
                const double trueNatY = eventSceneY + trueDy;
                if (eventFrame)
                {
                    const double amp = fc.fmt == PixelFormat::Mono16 ? 380.0 : 28.0;
                    if (!shadows) paintFlash(frame, trueNatX, trueNatY, 1.6, amp);
                    else paintIssSilhouette(frame, trueNatX, trueNatY, 24.0, 0.70, 18.0, 2.0);
                }

                const bool tOff = dOff.analyze(frame, {}, {ShiftMode::Off, 0, 0}); dOff.updateReference(frame);
                const bool tInt = dInt.analyze(frame, {}, {ShiftMode::Integer, st.dx, st.dy}); dInt.updateReference(frame);
                const bool tSub = dSub.analyze(frame, {}, {ShiftMode::Subpixel, st.dx, st.dy}); dSub.updateReference(frame);

                if (f >= 45 && !eventFrame && f != 112)
                {
                    if (tOff) ++falseOff;
                    if (tInt) ++falseInt;
                    if (tSub) ++falseSub;
                }
                if (eventFrame)
                {
                    if (tOff) recOff = true;
                    if (tInt) recInt = true;
                    if (tSub)
                    {
                        recSub = true;
                        const auto& c = dSub.candidates().front();
                        coordErrSub = std::hypot(c.peakX - trueNatX, c.peakY - trueNatY);
                    }
                }
            }
            QJsonObject item;
            item["format"] = fc.name;
            item["kind"] = shadows ? "shadow" : "flash";
            item["estimator"] = estimatorName(stab.activeEstimator());
            item["mean_shift_err_px"] = shiftErrCount > 0 ? shiftErrSum / shiftErrCount : -1.0;
            item["false_off"] = falseOff;
            item["false_int"] = falseInt;
            item["false_sub"] = falseSub;
            item["recovered_off"] = recOff;
            item["recovered_int"] = recInt;
            item["recovered_sub"] = recSub;
            item["subpixel_coord_err_px"] = coordErrSub;
            testA.append(item);
            out << QString("[Synth-A %1 %2] est=%3 shiftErr=%4 px | false: off=%5 int=%6 sub=%7 | rec: off=%8 int=%9 sub=%10 (coordErr=%11 px)\n")
                .arg(fc.name).arg(shadows ? "shadow" : "flash").arg(estimatorName(stab.activeEstimator()))
                .arg(shiftErrCount > 0 ? shiftErrSum / shiftErrCount : -1.0, 0, 'f', 2)
                .arg(falseOff).arg(falseInt).arg(falseSub)
                .arg(recOff ? 1 : 0).arg(recInt ? 1 : 0).arg(recSub ? 1 : 0).arg(coordErrSub, 0, 'f', 2);
        }
    }
    root["test_a_drift_and_bayer"] = testA;

    // --- Test B: Failure-mode & rotation stress tests ---
    QJsonObject testB;
    {
        // B1: Lock-box occlusion (8 frames) -> GraceHold keeps detecting with 0 false triggers
        std::mt19937 rng(101);
        DetectionSettings s = defaultDetectionSettings(Watch::MoonFlashes);
        s.ignoreAboveShare = 0.0; s.sigma = 8.0; s.minPixels = 4;
        FlashDetector det(s);
        ShiftStabilizer stab(EstimatorKind::FeatureLock);
        int graceFrames = 0, lostFrames = 0, falseTrigs = 0;
        bool recoveredDuringGrace = false;
        for (int f = 0; f < 130; ++f)
        {
            const double dx = f * 0.05, dy = -f * 0.03;
            Frame frame = renderSyntheticLunarFrame(320, 240, PixelFormat::Mono8, BayerPattern::None, dx, dy, 0.0, 1.0, 1.2, rng, f);
            const LockRegion lr = stab.lockRegion();
            if (f >= 70 && f < 78 && lr.width > 0)
            {
                // Occlude the exact FeatureLock patch (plus margin) to force transient lock loss
                const int ox0 = std::max(0, lr.x - 16);
                const int oy0 = std::max(0, lr.y - 16);
                const int ox1 = std::min(320, lr.x + lr.width + 16);
                const int oy1 = std::min(240, lr.y + lr.height + 16);
                for (int y = oy0; y < oy1; ++y)
                    for (int x = ox0; x < ox1; ++x)
                        frame.data[static_cast<size_t>(y) * 320 + x] = 5;
            }
            const StabilizerStep st = stab.step(frame, 1.0 / 30.0);
            if (st.state == StabilizerState::GraceHold) ++graceFrames;
            if (st.state == StabilizerState::LostFallback) ++lostFrames;
            // Inject a flash outside the occluded lock box during grace frames 73..74
            const bool isFlash = (f == 73 || f == 74);
            const double fx = (lr.x + lr.width / 2 < 160) ? 230.0 : 90.0;
            if (isFlash) paintFlash(frame, fx + dx, 120.0 + dy, 1.6, 35.0);
            if (st.resetDetector) det.reset(false);
            const bool t = det.analyze(frame, {}, st.applyShift ? FrameShift{ShiftMode::Subpixel, st.dx, st.dy} : FrameShift{});
            det.updateReference(frame);
            if (isFlash && t) recoveredDuringGrace = true;
            else if (f >= 45 && !isFlash && f != 75 && !(f >= 70 && f < 78) && t) ++falseTrigs;
        }
        QJsonObject b1;
        b1["grace_frames"] = graceFrames; b1["lost_frames"] = lostFrames;
        b1["recovered_during_grace"] = recoveredDuringGrace; b1["false_triggers_outside_occlusion"] = falseTrigs;
        testB["b1_transient_occlusion"] = b1;
        out << QString("[Synth-B1 Occlusion] grace=%1 lost=%2 recoveredDuringGrace=%3 falseOutside=%4\n")
            .arg(graceFrames).arg(lostFrames).arg(recoveredDuringGrace ? 1 : 0).arg(falseTrigs);
    }
    {
        // B2: Sustained cloud blackout (45 frames) -> clean LostFallback, 0 false storm, clean re-lock
        std::mt19937 rng(102);
        DetectionSettings s = defaultDetectionSettings(Watch::MoonFlashes);
        s.ignoreAboveShare = 0.0; s.sigma = 8.0; s.minPixels = 4;
        FlashDetector det(s);
        ShiftStabilizer stab(EstimatorKind::FeatureLock);
        int lostFrames = 0, relockedAt = -1, falseTriggers = 0;
        bool sawLost = false;
        for (int f = 0; f < 170; ++f)
        {
            const double dx = f * 0.04, dy = f * 0.02;
            const double clouds = (f >= 60 && f < 105) ? 0.0 : 1.0;
            Frame frame = renderSyntheticLunarFrame(320, 240, PixelFormat::Mono8, BayerPattern::None, dx, dy, 0.0, clouds, 1.2, rng, f);
            const StabilizerStep st = stab.step(frame, 1.0 / 30.0);
            if (st.state == StabilizerState::LostFallback) { ++lostFrames; sawLost = true; }
            if (sawLost && relockedAt < 0 && st.state == StabilizerState::Locked) relockedAt = f;
            if (st.resetDetector) det.reset(false);
            const bool t = det.analyze(frame, {}, st.applyShift ? FrameShift{ShiftMode::Subpixel, st.dx, st.dy} : FrameShift{});
            det.updateReference(frame);
            if (f >= 45 && t) ++falseTriggers;
        }
        QJsonObject b2;
        b2["lost_frames"] = lostFrames; b2["relocked_at_frame"] = relockedAt; b2["false_triggers"] = falseTriggers;
        testB["b2_sustained_clouds"] = b2;
        out << QString("[Synth-B2 Clouds] lost=%1 relockedAt=%2 (cleared at 105) falseTriggers=%3\n")
            .arg(lostFrames).arg(relockedAt).arg(falseTriggers);
    }
    {
        // B3: Sudden 25 px wind jump -> rejected, no storm, cleanly recovers
        std::mt19937 rng(103);
        DetectionSettings s = defaultDetectionSettings(Watch::MoonFlashes);
        s.ignoreAboveShare = 0.0; s.sigma = 8.0; s.minPixels = 4;
        FlashDetector det(s);
        ShiftStabilizer stab(EstimatorKind::FeatureLock);
        int falseTriggers = 0, relockedAt = -1;
        bool sawLost = false;
        for (int f = 0; f < 150; ++f)
        {
            const double dx = f >= 60 ? 25.0 : 0.0;
            Frame frame = renderSyntheticLunarFrame(320, 240, PixelFormat::Mono8, BayerPattern::None, dx, 0.0, 0.0, 1.0, 1.2, rng, f);
            const StabilizerStep st = stab.step(frame, 1.0 / 30.0);
            if (st.state == StabilizerState::LostFallback) sawLost = true;
            if (sawLost && relockedAt < 0 && st.state == StabilizerState::Locked) relockedAt = f;
            if (st.resetDetector) det.reset(false);
            const bool t = det.analyze(frame, {}, st.applyShift ? FrameShift{ShiftMode::Subpixel, st.dx, st.dy} : FrameShift{});
            det.updateReference(frame);
            // Ignore the first frame of the jump itself if unstabilized; count triggers during fallback/recovery
            if (f >= 62 && t) ++falseTriggers;
        }
        QJsonObject b3;
        b3["saw_lost"] = sawLost; b3["relocked_at_frame"] = relockedAt; b3["false_triggers_after_jump"] = falseTriggers;
        testB["b3_wind_jump"] = b3;
        out << QString("[Synth-B3 WindJump 25px] sawLost=%1 relockedAt=%2 falseAfterJump=%3\n")
            .arg(sawLost ? 1 : 0).arg(relockedAt).arg(falseTriggers);
    }
    {
        // B4: Large unguided continuous drift (55 px total) -> in-place recenter, 0 false triggers, flash recovered after recenter
        std::mt19937 rng(104);
        DetectionSettings s = defaultDetectionSettings(Watch::MoonFlashes);
        s.ignoreAboveShare = 0.0; s.sigma = 8.0; s.minPixels = 4;
        FlashDetector det(s);
        ShiftStabilizer stab(EstimatorKind::DiskCoG);
        int recenters = 0, falseTriggers = 0;
        bool recoveredAfterRecenter = false;
        for (int f = 0; f < 140; ++f)
        {
            const double dx = -25.0 + 0.40 * f; // sweeps 56 px across the frame
            Frame frame = renderSyntheticLunarFrame(360, 240, PixelFormat::Mono8, BayerPattern::None, dx, 0.0, 0.0, 1.0, 1.2, rng, f);
            const StabilizerStep st = stab.step(frame, 1.0 / 30.0);
            if (st.recenterStepX != 0 || st.recenterStepY != 0)
            {
                ++recenters;
                det.recenterReference(st.recenterStepX, st.recenterStepY, frame, {ShiftMode::Subpixel, st.dx, st.dy});
            }
            const bool isFlash = (f == 120 || f == 121);
            if (isFlash) paintFlash(frame, 180.0 + dx, 120.0, 1.6, 30.0);
            const bool t = det.analyze(frame, {}, {ShiftMode::Subpixel, st.dx, st.dy});
            det.updateReference(frame);
            if (isFlash && t) recoveredAfterRecenter = true;
            else if (f >= 45 && !isFlash && f != 122 && t) ++falseTriggers;
        }
        QJsonObject b4;
        b4["recenters"] = recenters; b4["false_triggers"] = falseTriggers; b4["recovered_after_recenter"] = recoveredAfterRecenter;
        testB["b4_large_drift_recenter"] = b4;
        out << QString("[Synth-B4 LargeDrift 56px] recenters=%1 falseTriggers=%2 recoveredAfterRecenter=%3\n")
            .arg(recenters).arg(falseTriggers).arg(recoveredAfterRecenter ? 1 : 0);
    }
    {
        // B5: Slow field rotation (polar misalignment & alt-az) with periodic template refresh
        // Simulate 10 degrees total rotation over 200 frames (0.05 deg/frame — equivalent to 1.5 deg/sec,
        // i.e. ~360x faster than alt-az or ~100,000x faster than polar-misalignment!) + jitter.
        // Also test realistic polar-misalignment + alt-az rates (0.005 deg/frame = 0.15 deg/s = 540 deg/hr).
        std::mt19937 rng(105);
        std::normal_distribution<double> jitter(0.0, 0.6);
        DetectionSettings s = defaultDetectionSettings(Watch::MoonFlashes);
        s.ignoreAboveShare = 0.0; s.sigma = 8.0; s.minPixels = 4;
        FlashDetector detFrozen(s), detRefresh(s);
        ShiftStabilizer stabFrozen(EstimatorKind::FeatureLock);
        stabFrozen.templateRefreshSeconds = 1e9; // disabled
        ShiftStabilizer stabRefresh(EstimatorKind::FeatureLock);
        stabRefresh.templateRefreshSeconds = 1.5; // refreshes as rotation accumulates

        int falseFrozen = 0, falseRefresh = 0, lostFrozen = 0, lostRefresh = 0, refreshes = 0;
        double minScoreFrozen = 1.0, minScoreRefresh = 1.0;
        for (int f = 0; f < 240; ++f)
        {
            const double angle = 0.025 * f; // 6 degrees total rotation over 240 frames!
            const double dx = f == 0 ? 0.0 : jitter(rng);
            const double dy = f == 0 ? 0.0 : jitter(rng);
            Frame frame = renderSyntheticLunarFrame(320, 240, PixelFormat::Mono8, BayerPattern::None, dx, dy, angle, 1.0, 1.2, rng, f);
            const auto stF = stabFrozen.step(frame, 1.0 / 30.0);
            const auto stR = stabRefresh.step(frame, 1.0 / 30.0);
            if (f > 0)
            {
                minScoreFrozen = std::min(minScoreFrozen, stF.score);
                minScoreRefresh = std::min(minScoreRefresh, stR.score);
            }
            if (stF.state == StabilizerState::LostFallback) ++lostFrozen;
            if (stR.state == StabilizerState::LostFallback) ++lostRefresh;
            if (stR.event == "template_refresh") ++refreshes;

            if (stF.resetDetector) detFrozen.reset(false);
            if (stR.resetDetector) detRefresh.reset(false);
            const bool tF = detFrozen.analyze(frame, {}, stF.applyShift ? FrameShift{ShiftMode::Subpixel, stF.dx, stF.dy} : FrameShift{});
            detFrozen.updateReference(frame);
            const bool tR = detRefresh.analyze(frame, {}, stR.applyShift ? FrameShift{ShiftMode::Subpixel, stR.dx, stR.dy} : FrameShift{});
            detRefresh.updateReference(frame);
            if (f >= 45)
            {
                if (tF) ++falseFrozen;
                if (tR) ++falseRefresh;
            }
        }
        QJsonObject b5;
        b5["total_rotation_deg"] = 6.0;
        b5["frozen_min_score"] = minScoreFrozen;
        b5["frozen_lost_frames"] = lostFrozen;
        b5["frozen_false_triggers"] = falseFrozen;
        b5["refresh_count"] = refreshes;
        b5["refresh_min_score"] = minScoreRefresh;
        b5["refresh_lost_frames"] = lostRefresh;
        b5["refresh_false_triggers"] = falseRefresh;
        testB["b5_field_rotation_6deg"] = b5;
        out << QString("[Synth-B5 FieldRotation 6deg] frozen: minScore=%1 lost=%2 false=%3 | refresh(%4x): minScore=%5 lost=%6 false=%7\n")
            .arg(minScoreFrozen, 0, 'f', 3).arg(lostFrozen).arg(falseFrozen)
            .arg(refreshes).arg(minScoreRefresh, 0, 'f', 3).arg(lostRefresh).arg(falseRefresh);
    }
    root["test_b_failures_and_rotation"] = testB;

    QFile f(outputPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return 0;
}

// --- Subcommand 3: Release x64 CPU cost matrix ---

int runCpuBench(const QStringList& args)
{
    QTextStream out(stdout), err(stderr);
    if (args.size() < 4) { err << "Usage: SeleneWatchProbe stabilize-bench cpu OUTPUT.json\n"; return 2; }
    const QString outputPath = args[3];

    struct SizeCase { const char* label; int w; int h; int iters; };
    const SizeCase sizes[] = {
        {"480x480", 480, 480, 120},
        {"640x480", 640, 480, 100},
        {"1280x960", 1280, 960, 60},
        {"2128x2128", 2128, 2128, 25},
    };
    struct FmtCase { const char* label; PixelFormat fmt; BayerPattern bayer; };
    const FmtCase fmts[] = {
        {"Mono8", PixelFormat::Mono8, BayerPattern::None},
        {"Mono16", PixelFormat::Mono16, BayerPattern::None},
        {"Bayer8", PixelFormat::Mono8, BayerPattern::RGGB},
        {"Bayer16", PixelFormat::Mono16, BayerPattern::RGGB},
        {"RGB888", PixelFormat::RGB888, BayerPattern::None},
    };

    QJsonArray detectorRows;
    QJsonArray estimatorRows;
    std::mt19937 rng(7);

    out << "=== CPU Benchmark: FlashDetector (analyze + updateReference) ===\n";
    for (const auto& sc : sizes)
    {
        for (const auto& fc : fmts)
        {
            Frame f0 = renderSyntheticLunarFrame(sc.w, sc.h, fc.fmt, fc.bayer, 0.0, 0.0, 0.0, 1.0, 1.5, rng, 0);
            Frame f1 = renderSyntheticLunarFrame(sc.w, sc.h, fc.fmt, fc.bayer, 2.35, -1.65, 0.0, 1.0, 1.5, rng, 1);

            const ShiftMode modes[3] = {ShiftMode::Off, ShiftMode::Integer, ShiftMode::Subpixel};
            double meanMs[3]{}, p95Ms[3]{};

            for (int m = 0; m < 3; ++m)
            {
                FlashDetector det(defaultDetectionSettings(Watch::MoonShadows));
                det.analyze(f0); det.updateReference(f0);
                // Warmup 3 frames
                for (int w = 0; w < 3; ++w)
                {
                    f1.frameIndex = static_cast<uint64_t>(w + 1);
                    f1.timestamp = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{(w + 1) * 33};
                    det.analyze(f1, {}, {modes[m], 2.35, -1.65});
                    det.updateReference(f1);
                }
                std::vector<double> times;
                times.reserve(sc.iters);
                for (int it = 0; it < sc.iters; ++it)
                {
                    f1.frameIndex = static_cast<uint64_t>(it + 10);
                    f1.timestamp = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{(it + 10) * 33};
                    const auto t0 = std::chrono::steady_clock::now();
                    det.analyze(f1, {}, {modes[m], 2.35, -1.65});
                    det.updateReference(f1);
                    times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                }
                meanMs[m] = meanOf(times);
                p95Ms[m] = percentile(times, 0.95);
            }

            QJsonObject row;
            row["size"] = sc.label;
            row["format"] = fc.label;
            row["off_mean_ms"] = meanMs[0]; row["off_p95_ms"] = p95Ms[0];
            row["int_mean_ms"] = meanMs[1]; row["int_p95_ms"] = p95Ms[1];
            row["sub_mean_ms"] = meanMs[2]; row["sub_p95_ms"] = p95Ms[2];
            row["sub_overhead_ms"] = meanMs[2] - meanMs[0];
            detectorRows.append(row);
            out << QString("  %1 %2: off=%3 ms | int=%4 ms | sub=%5 ms (overhead +%6 ms, p95 %7 ms)\n")
                .arg(sc.label, -9).arg(fc.label, -7)
                .arg(meanMs[0], 5, 'f', 2).arg(meanMs[1], 5, 'f', 2).arg(meanMs[2], 5, 'f', 2)
                .arg(meanMs[2] - meanMs[0], 5, 'f', 2).arg(p95Ms[2], 5, 'f', 2);

            // Benchmark estimators on Mono8 and Bayer8 for each frame size
            if (fc.fmt == PixelFormat::Mono8)
            {
                std::string reason;
                AutoFeatureLockEstimator lockEst;
                const bool lockOk = lockEst.init(f0, reason);
                DiskCogEstimator cogEst;
                const bool cogOk = cogEst.init(f0);
                PhaseCorrEstimator phaseEst(256);
                const bool phaseOk = phaseEst.init(f0);

                auto benchEst = [&](auto&& fn) {
                    std::vector<double> t;
                    t.reserve(sc.iters);
                    for (int w = 0; w < 3; ++w) fn();
                    for (int it = 0; it < sc.iters; ++it)
                    {
                        const auto a = std::chrono::steady_clock::now();
                        fn();
                        t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
                    }
                    return std::pair{meanOf(t), percentile(t, 0.95)};
                };

                const auto lockT = lockOk ? benchEst([&] { (void)lockEst.measure(f1, 2.0, -1.5, 16); }) : std::pair{-1.0, -1.0};
                const auto cogT = cogOk ? benchEst([&] { (void)cogEst.measure(f1); }) : std::pair{-1.0, -1.0};
                const auto phaseT = phaseOk ? benchEst([&] { (void)phaseEst.measure(f1); }) : std::pair{-1.0, -1.0};

                QJsonObject erow;
                erow["size"] = sc.label;
                erow["format"] = fc.label;
                erow["feature_lock_mean_ms"] = lockT.first; erow["feature_lock_p95_ms"] = lockT.second;
                erow["disk_cog_mean_ms"] = cogT.first; erow["disk_cog_p95_ms"] = cogT.second;
                erow["phase_corr_mean_ms"] = phaseT.first; erow["phase_corr_p95_ms"] = phaseT.second;
                estimatorRows.append(erow);
                out << QString("    Estimators (%1 %2): FeatureLock=%3 ms | DiskCoG=%4 ms | PhaseCorr256=%5 ms\n")
                    .arg(sc.label).arg(fc.label)
                    .arg(lockT.first, 5, 'f', 2).arg(cogT.first, 5, 'f', 2).arg(phaseT.first, 5, 'f', 2);
            }
        }
    }

    QJsonObject root;
    root["detector"] = detectorRows;
    root["estimators"] = estimatorRows;
    QFile f(outputPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return 0;
}

} // namespace

int runStabilizeBench(const QStringList& args)
{
    QTextStream err(stderr);
    if (args.size() < 4)
    {
        err << "Usage: SeleneWatchProbe stabilize-bench recording INPUT OUTPUT.json [...] | synthetic OUTPUT.json | cpu OUTPUT.json\n";
        return 2;
    }
    const QString sub = args[2];
    if (sub == "recording") return runRecordingBench(args);
    if (sub == "synthetic") return runSyntheticBench(args);
    if (sub == "cpu") return runCpuBench(args);
    err << "Unknown stabilize-bench subcommand: " << sub << "\n";
    return 2;
}
