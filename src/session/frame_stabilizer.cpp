#include "session/frame_stabilizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

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

// True when an isolated bright body (planet, full Moon/Sun) sits inside dark
// sky borders within the given rectangle rather than filling its edges.
bool rectContainsIsolatedDisk(const Frame& frame, int x0, int y0, int w, int h)
{
    x0 = std::clamp(x0, 0, frame.width);
    y0 = std::clamp(y0, 0, frame.height);
    const int x1 = std::clamp(x0 + w, x0, frame.width);
    const int y1 = std::clamp(y0 + h, y0, frame.height);
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    if (rw < 32 || rh < 32) return false;

    const int ring = std::min(4, std::min(rw, rh) / 8);
    std::vector<double> border;
    border.reserve(static_cast<size_t>(rw + rh) * 4);
    for (int y = y0; y < y1; y += 2)
    {
        for (int x = x0; x < x1; x += 2)
        {
            if (x < x0 + ring || x >= x1 - ring || y < y0 + ring || y >= y1 - ring)
                border.push_back(readPixel(frame, x, y));
        }
    }
    std::vector<double> interior;
    const int step = std::max(2, std::min(rw, rh) / 64);
    for (int y = y0 + ring; y < y1 - ring; y += step)
        for (int x = x0 + ring; x < x1 - ring; x += step)
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
    for (double v : interior)
        if (v > thresh) ++bright;
    const double share = static_cast<double>(bright) / static_cast<double>(interior.size());
    return share >= 0.004 && share <= 0.78;
}

bool isIsolatedDisk(const Frame& frame)
{
    return rectContainsIsolatedDisk(frame, 0, 0, frame.width, frame.height);
}

struct CentroidSample
{
    bool valid{false};
    double cx{0}, cy{0}, weight{0};
    std::string reason;
};

CentroidSample sampleDiskCentroid(const Frame& frame)
{
    CentroidSample c;
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
        c.reason = "low_contrast";
        return c;
    }

    // Clipped limb weight keeps the centroid anchored on the geometric disk edge
    // and insensitive to interior flashes or small dark transits.
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
        c.reason = "target_missing";
        return c;
    }
    c.cx = sumWX / sumW;
    c.cy = sumWY / sumW;
    c.weight = sumW;
    if (c.cx < 4.0 || c.cx > frame.width - 5.0 || c.cy < 4.0 || c.cy > frame.height - 5.0)
    {
        c.reason = "target_on_border";
        return c;
    }
    c.valid = true;
    return c;
}

std::string sanitizeReason(const std::string& raw)
{
    if (raw.empty()) return "lock_lost";
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw)
        out.push_back((ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') ? '_' : ch);
    return out;
}

} // namespace

void FrameStabilizer::reset()
{
    width_ = height_ = bits_ = 0;
    state_ = StabilizationState::Off;
    method_ = StabilizationMethod::None;
    initialized_ = false;
    refCx_ = refCy_ = refWeight_ = 0.0;
    lock_.reset();
    anchorDx_ = anchorDy_ = 0.0;
    preferredApplied_ = false;
    appliedPreferred_ = {};
    lastDx_ = lastDy_ = 0.0;
    graceElapsedSec_ = 0.0;
    sinceRefreshSec_ = 0.0;
    reacquireStreak_ = 0;
    reacquireDx_ = reacquireDy_ = 0.0;
}

bool FrameStabilizer::sameLayout(const Frame& frame) const
{
    return width_ == frame.width && height_ == frame.height && format_ == frame.format
        && bits_ == frame.significantBits && alignment_ == frame.alignment && bayer_ == frame.bayer;
}

bool FrameStabilizer::initDiskCentroid(const Frame& frame)
{
    const auto c = sampleDiskCentroid(frame);
    if (!c.valid) return false;
    refCx_ = c.cx;
    refCy_ = c.cy;
    refWeight_ = c.weight;
    method_ = StabilizationMethod::DiskCentroid;
    lock_.reset();
    return true;
}

bool FrameStabilizer::initFeatureLock(const Frame& frame, std::optional<LockRegion> preferredLockRegion)
{
    std::string refusal;
    if (preferredLockRegion && preferredLockRegion->width >= FeatureLock::kMinTemplateSide
        && preferredLockRegion->height >= FeatureLock::kMinTemplateSide)
    {
        auto locked = FeatureLock::take(frame, *preferredLockRegion, refusal);
        if (locked)
        {
            lock_ = std::move(locked);
            anchorDx_ = 0.0;
            anchorDy_ = 0.0;
            method_ = StabilizationMethod::FeatureLock;
            preferredApplied_ = true;
            appliedPreferred_ = *preferredLockRegion;
            return true;
        }
    }

    const int side = std::clamp(std::min(frame.width, frame.height) / 4, 64, 128);
    const int box = (side / 2) * 2;
    int margin = std::clamp(std::min(frame.width, frame.height) / 8, 24, 96);
    if (frame.width < box + 2 * margin || frame.height < box + 2 * margin)
        margin = std::max(8, std::min(frame.width - box, frame.height - box) / 4);
    if (frame.width < box + 2 * margin || frame.height < box + 2 * margin) return false;

    struct Candidate { LockRegion r; double variance; };
    std::vector<Candidate> candidates;
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
            int count = 0, saturated = 0;
            for (int y = ry; y < ry + box; y += 4)
            {
                for (int x = rx; x < rx + box; x += 4)
                {
                    const double v = readPixel(frame, x, y);
                    sum += v; sumSq += v * v; ++count;
                    if (v >= maxCode - 1.0) ++saturated;
                }
            }
            if (count == 0 || static_cast<double>(saturated) / count > 0.15) continue;
            const double m = sum / count;
            candidates.push_back({{rx, ry, box, box}, std::max(0.0, sumSq / count - m * m)});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.variance > b.variance; });

    for (const auto& c : candidates)
    {
        auto locked = FeatureLock::take(frame, c.r, refusal);
        if (locked)
        {
            lock_ = std::move(locked);
            anchorDx_ = 0.0;
            anchorDy_ = 0.0;
            method_ = StabilizationMethod::FeatureLock;
            preferredApplied_ = false;
            return true;
        }
    }
    return false;
}

bool FrameStabilizer::initEstimator(const Frame& frame, std::optional<LockRegion> preferredLockRegion)
{
    initialized_ = false;
    method_ = StabilizationMethod::None;
    const bool isolatedFrame = isIsolatedDisk(frame);
    // If the observer drew a guiding box around an isolated planet/disk (dark
    // sky borders inside the box as well), DiskCentroid is faster and immune to
    // planetary rotation. If the box lies on lunar terrain (surface touching
    // its edges), honour the observer's chosen Feature Lock crater patch.
    if (preferredLockRegion)
    {
        const bool boxAroundDisk = isolatedFrame
            && rectContainsIsolatedDisk(frame, preferredLockRegion->x, preferredLockRegion->y,
                preferredLockRegion->width, preferredLockRegion->height);
        if (boxAroundDisk && initDiskCentroid(frame))
        {
            preferredApplied_ = true;
            appliedPreferred_ = *preferredLockRegion;
            initialized_ = true;
            return true;
        }
        if (initFeatureLock(frame, preferredLockRegion))
        {
            initialized_ = true;
            return true;
        }
    }
    // Without a terrain lock box: isolated bright disk -> DiskCentroid.
    if (isolatedFrame && initDiskCentroid(frame))
    {
        initialized_ = true;
        return true;
    }
    // Extended lunar surface without a lock box -> auto-picked FeatureLock patch.
    if (initFeatureLock(frame, std::nullopt))
    {
        initialized_ = true;
        return true;
    }
    return false;
}

FrameStabilizer::Measurement FrameStabilizer::measureDiskCentroid(const Frame& frame) const
{
    const auto t0 = std::chrono::steady_clock::now();
    Measurement out;
    const auto c = sampleDiskCentroid(frame);
    out.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!c.valid) { out.reason = c.reason; return out; }
    const double areaRatio = refWeight_ > 0.0 ? c.weight / refWeight_ : 0.0;
    if (areaRatio < 0.55 || areaRatio > 1.65)
    {
        out.reason = "disk_area_changed";
        out.score = std::max(0.0, 1.0 - std::abs(1.0 - areaRatio));
        return out;
    }
    out.valid = true;
    out.dx = c.cx - refCx_;
    out.dy = c.cy - refCy_;
    out.score = std::clamp(1.0 - std::abs(1.0 - areaRatio), 0.0, 1.0);
    return out;
}

FrameStabilizer::Measurement FrameStabilizer::measure(
    const Frame& frame, double expectedDx, double expectedDy, int searchRadiusPx) const
{
    if (method_ == StabilizationMethod::DiskCentroid) return measureDiskCentroid(frame);
    Measurement out;
    if (method_ != StabilizationMethod::FeatureLock || !lock_)
    {
        out.reason = "uninitialized";
        return out;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const LockMatch m = lock_->find(frame, expectedDx - anchorDx_, expectedDy - anchorDy_, searchRadiusPx);
    out.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    out.valid = m.valid;
    out.dx = anchorDx_ + m.dx;
    out.dy = anchorDy_ + m.dy;
    out.score = m.score;
    out.reason = m.reason;
    return out;
}

bool FrameStabilizer::refreshFeatureLock(const Frame& frame, double currentDx, double currentDy)
{
    if (method_ != StabilizationMethod::FeatureLock || !lock_) return false;
    const bool bayer = frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
    const int shiftX = bayer ? 2 * static_cast<int>(std::lround((currentDx - anchorDx_) / 2.0))
                             : static_cast<int>(std::lround(currentDx - anchorDx_));
    const int shiftY = bayer ? 2 * static_cast<int>(std::lround((currentDy - anchorDy_) / 2.0))
                             : static_cast<int>(std::lround(currentDy - anchorDy_));
    LockRegion next = lock_->region();
    next.x += shiftX;
    next.y += shiftY;
    if (next.x < 16 || next.y < 16 || next.x + next.width > frame.width - 16 || next.y + next.height > frame.height - 16)
        return false;
    std::string refusal;
    auto fresh = FeatureLock::take(frame, next, refusal);
    if (!fresh) return false;
    lock_ = std::move(fresh);
    anchorDx_ = currentDx;
    anchorDy_ = currentDy;
    return true;
}

void FrameStabilizer::adoptPreferredRegion(const Frame& frame, LockRegion region, StepResult& out)
{
    // Guiding box drawn around an isolated planet/disk: keep/use DiskCentroid
    // so planetary rotation never degrades stabilization.
    if (isIsolatedDisk(frame) && rectContainsIsolatedDisk(frame, region.x, region.y, region.width, region.height))
    {
        preferredApplied_ = true;
        appliedPreferred_ = region;
        if (method_ == StabilizationMethod::DiskCentroid && initialized_) return;
        if (initDiskCentroid(frame))
        {
            initialized_ = true;
            if (state_ == StabilizationState::Locked || state_ == StabilizationState::GraceHold)
            {
                refCx_ -= lastDx_;
                refCy_ -= lastDy_;
            }
            else
            {
                lastDx_ = lastDy_ = 0.0;
                graceElapsedSec_ = 0.0;
                reacquireStreak_ = 0;
                state_ = StabilizationState::Locked;
                out.resetDetector = true;
                out.logEvent = "registration_locked method=disk_centroid";
            }
        }
        return;
    }

    std::string refusal;
    auto fresh = FeatureLock::take(frame, region, refusal);
    if (!fresh) return;
    lock_ = std::move(fresh);
    method_ = StabilizationMethod::FeatureLock;
    initialized_ = true;
    preferredApplied_ = true;
    appliedPreferred_ = region;
    sinceRefreshSec_ = 0.0;
    if (state_ == StabilizationState::Locked || state_ == StabilizationState::GraceHold)
    {
        // Anchor-preserving handoff: keep the detector's reference untouched with zero jump.
        anchorDx_ = lastDx_;
        anchorDy_ = lastDy_;
    }
    else
    {
        anchorDx_ = anchorDy_ = 0.0;
        lastDx_ = lastDy_ = 0.0;
        graceElapsedSec_ = 0.0;
        reacquireStreak_ = 0;
        state_ = StabilizationState::Locked;
        out.resetDetector = true;
        out.logEvent = "registration_locked method=feature_lock";
    }
}

FrameStabilizer::StepResult FrameStabilizer::step(
    const Frame& frame, double frameIntervalSec, std::optional<LockRegion> preferredLockRegion)
{
    StepResult out;
    if (!(frameIntervalSec > 0.0) || !std::isfinite(frameIntervalSec)) frameIntervalSec = 1.0 / 30.0;
    frameIntervalSec = std::min(frameIntervalSec, 1.0);

    if (state_ == StabilizationState::Off || !sameLayout(frame))
    {
        reset();
        width_ = frame.width; height_ = frame.height; bits_ = frame.significantBits;
        format_ = frame.format; alignment_ = frame.alignment; bayer_ = frame.bayer;
        if (initEstimator(frame, preferredLockRegion))
        {
            state_ = StabilizationState::Locked;
            out.shift = {ShiftMode::Subpixel, 0.0, 0.0, state_, method_, 0.0};
            out.logEvent = std::string("registration_locked method=") + stabilizationMethodName(method_);
        }
        else
        {
            state_ = StabilizationState::LostFallback;
            out.shift = {ShiftMode::Off, 0.0, 0.0, state_, StabilizationMethod::None, 0.0};
            out.logEvent = "registration_lost reason=no_lockable_feature";
        }
        return out;
    }

    // If the observer sets or moves a Feature Lock box while running, adopt it smoothly.
    const auto regionChanged = [&]() {
        if (!preferredLockRegion) return false;
        if (!preferredApplied_) return true;
        return preferredLockRegion->x != appliedPreferred_.x || preferredLockRegion->y != appliedPreferred_.y
            || preferredLockRegion->width != appliedPreferred_.width || preferredLockRegion->height != appliedPreferred_.height;
    };
    if (regionChanged())
    {
        adoptPreferredRegion(frame, *preferredLockRegion, out);
        if (out.resetDetector)
        {
            out.shift = {ShiftMode::Subpixel, 0.0, 0.0, state_, method_, 0.0};
            return out;
        }
    }

    const int searchRadius = (state_ == StabilizationState::Locked) ? 16 : 32;
    const Measurement m = initialized_ ? measure(frame, lastDx_, lastDy_, searchRadius) : Measurement{};
    const bool plausible = m.valid && (state_ == StabilizationState::LostFallback
        || std::hypot(m.dx - lastDx_, m.dy - lastDy_) <= maxStepPx);

    if (state_ == StabilizationState::Locked || state_ == StabilizationState::GraceHold)
    {
        if (plausible)
        {
            state_ = StabilizationState::Locked;
            graceElapsedSec_ = 0.0;
            lastDx_ = m.dx;
            lastDy_ = m.dy;
            sinceRefreshSec_ += frameIntervalSec;

            // Tier 3: in-place integer recenter when accumulated drift exceeds margin.
            if (std::abs(lastDx_) >= recenterMarginPx || std::abs(lastDy_) >= recenterMarginPx)
            {
                const bool bayer = frame.bayer != BayerPattern::None && frame.format != PixelFormat::RGB888;
                const int sx = bayer ? 2 * static_cast<int>(std::trunc(lastDx_ / 2.0)) : static_cast<int>(std::trunc(lastDx_));
                const int sy = bayer ? 2 * static_cast<int>(std::trunc(lastDy_ / 2.0)) : static_cast<int>(std::trunc(lastDy_));
                if (sx != 0 || sy != 0)
                {
                    lastDx_ -= sx;
                    lastDy_ -= sy;
                    if (method_ == StabilizationMethod::DiskCentroid) { refCx_ += sx; refCy_ += sy; }
                    else if (method_ == StabilizationMethod::FeatureLock) { anchorDx_ -= sx; anchorDy_ -= sy; }
                    out.recenterStepX = sx;
                    out.recenterStepY = sy;
                }
            }

            // Periodic anchor-preserving template refresh for multi-hour rotation / illumination drift.
            if (method_ == StabilizationMethod::FeatureLock
                && (sinceRefreshSec_ >= templateRefreshSeconds
                    || (m.score >= 0.72 && m.score < 0.85 && sinceRefreshSec_ >= 5.0)))
            {
                if (refreshFeatureLock(frame, lastDx_, lastDy_)) sinceRefreshSec_ = 0.0;
            }

            out.shift = {ShiftMode::Subpixel, lastDx_, lastDy_, state_, method_, m.elapsedMs};
            return out;
        }

        // Tier 1: transient loss -> reject bad estimate and hold last valid (dx, dy).
        graceElapsedSec_ += frameIntervalSec;
        if (graceElapsedSec_ <= graceSeconds)
        {
            state_ = StabilizationState::GraceHold;
            out.shift = {ShiftMode::Subpixel, lastDx_, lastDy_, state_, method_, m.elapsedMs};
            return out;
        }

        // Tier 2: sustained loss -> cleanly reset detector to unstabilized fallback.
        state_ = StabilizationState::LostFallback;
        reacquireStreak_ = 0;
        lastDx_ = lastDy_ = 0.0;
        out.resetDetector = true;
        out.shift = {ShiftMode::Off, 0.0, 0.0, state_, method_, m.elapsedMs};
        out.logEvent = "registration_lost reason=" + sanitizeReason(m.valid ? "step_too_large" : m.reason);
        return out;
    }

    // LostFallback: attempt clean re-acquisition after consecutive consistent matches.
    if (!initialized_) initEstimator(frame, preferredLockRegion);
    if (initialized_)
    {
        if (m.valid && (reacquireStreak_ == 0 || std::hypot(m.dx - reacquireDx_, m.dy - reacquireDy_) <= maxStepPx))
        {
            ++reacquireStreak_;
            reacquireDx_ = m.dx;
            reacquireDy_ = m.dy;
        }
        else if (!m.valid && method_ == StabilizationMethod::FeatureLock)
        {
            reacquireStreak_ = 0;
            initEstimator(frame, preferredLockRegion);
        }
        else
        {
            reacquireStreak_ = 0;
        }

        if (reacquireStreak_ >= reacquireFrames && initEstimator(frame, preferredLockRegion))
        {
            state_ = StabilizationState::Locked;
            lastDx_ = lastDy_ = 0.0;
            graceElapsedSec_ = 0.0;
            sinceRefreshSec_ = 0.0;
            reacquireStreak_ = 0;
            out.resetDetector = true;
            out.shift = {ShiftMode::Subpixel, 0.0, 0.0, state_, method_, m.elapsedMs};
            out.logEvent = std::string("registration_locked method=") + stabilizationMethodName(method_);
            return out;
        }
    }

    out.shift = {ShiftMode::Off, 0.0, 0.0, StabilizationState::LostFallback, method_, m.elapsedMs};
    return out;
}
