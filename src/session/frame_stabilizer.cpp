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
    source_ = StabilizationSource::None;
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
    const DiskCentroidSample c = FeatureLock::measureDiskCentroid(frame, {0, 0, frame.width, frame.height});
    if (!c.valid) return false;
    refCx_ = c.cx;
    refCy_ = c.cy;
    refWeight_ = c.weight;
    method_ = StabilizationMethod::DiskCentroid;
    source_ = StabilizationSource::IsolatedDisk;
    lock_.reset();
    return true;
}

bool FrameStabilizer::initFeatureLock(const Frame& frame, std::optional<LockRegion> preferredLockRegion)
{
    std::string refusal;
    if (preferredLockRegion && preferredLockRegion->width >= FeatureLock::kMinTemplateSide
        && preferredLockRegion->height >= FeatureLock::kMinTemplateSide)
    {
        auto locked = FeatureLock::take(frame, *preferredLockRegion, refusal, false);
        if (locked)
        {
            lock_ = std::move(locked);
            anchorDx_ = 0.0;
            anchorDy_ = 0.0;
            method_ = StabilizationMethod::FeatureLock;
            source_ = StabilizationSource::GuidingBox;
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
        auto locked = FeatureLock::take(frame, c.r, refusal, false);
        if (locked)
        {
            lock_ = std::move(locked);
            anchorDx_ = 0.0;
            anchorDy_ = 0.0;
            method_ = StabilizationMethod::FeatureLock;
            source_ = StabilizationSource::AutoPatch;
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
    source_ = StabilizationSource::None;
    const bool isolatedFrame = FeatureLock::isIsolatedDisk(frame);
    // Harmonised with FeatureLock: a guiding box drawn around an isolated
    // planet/disk uses DiskCentroid (rotation-invariant); a guiding box on
    // lunar terrain uses Feature Lock on the observer's chosen crater patch.
    if (preferredLockRegion)
    {
        const bool boxAroundDisk = isolatedFrame && FeatureLock::isIsolatedDisk(frame, *preferredLockRegion);
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
    if (isolatedFrame && initDiskCentroid(frame))
    {
        initialized_ = true;
        return true;
    }
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
    const DiskCentroidSample c = FeatureLock::measureDiskCentroid(frame, {0, 0, frame.width, frame.height});
    out.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!c.valid) { out.reason = c.reason; return out; }
    const double areaRatio = refWeight_ > 0.0 ? c.weight / refWeight_ : 0.0;
    if (areaRatio < 0.65 || areaRatio > 1.45)
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
    auto fresh = FeatureLock::take(frame, next, refusal, false);
    if (!fresh) return false;
    lock_ = std::move(fresh);
    anchorDx_ = currentDx;
    anchorDy_ = currentDy;
    return true;
}

void FrameStabilizer::adoptPreferredRegion(const Frame& frame, LockRegion region, StepResult& out)
{
    if (FeatureLock::isIsolatedDisk(frame) && FeatureLock::isIsolatedDisk(frame, region))
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
    auto fresh = FeatureLock::take(frame, region, refusal, false);
    if (!fresh) return;
    lock_ = std::move(fresh);
    method_ = StabilizationMethod::FeatureLock;
    source_ = StabilizationSource::GuidingBox;
    initialized_ = true;
    preferredApplied_ = true;
    appliedPreferred_ = region;
    sinceRefreshSec_ = 0.0;
    if (state_ == StabilizationState::Locked || state_ == StabilizationState::GraceHold)
    {
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

    const auto stamp = [&](FrameShift s) {
        populateLockBounds(s);
        return s;
    };

    if (state_ == StabilizationState::Off || !sameLayout(frame))
    {
        reset();
        width_ = frame.width; height_ = frame.height; bits_ = frame.significantBits;
        format_ = frame.format; alignment_ = frame.alignment; bayer_ = frame.bayer;
        if (initEstimator(frame, preferredLockRegion))
        {
            state_ = StabilizationState::Locked;
            out.shift = stamp({ShiftMode::Subpixel, 0.0, 0.0, state_, method_, source_, 0.0});
            out.logEvent = std::string("registration_locked method=") + stabilizationMethodName(method_);
        }
        else
        {
            state_ = StabilizationState::LostFallback;
            out.shift = {ShiftMode::Off, 0.0, 0.0, state_, StabilizationMethod::None, StabilizationSource::None, 0.0};
            out.logEvent = "registration_lost reason=no_lockable_feature";
        }
        return out;
    }

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
            out.shift = stamp({ShiftMode::Subpixel, 0.0, 0.0, state_, method_, source_, 0.0});
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

            if (method_ == StabilizationMethod::FeatureLock
                && (sinceRefreshSec_ >= templateRefreshSeconds
                    || (m.score >= 0.72 && m.score < 0.85 && sinceRefreshSec_ >= 5.0)))
            {
                if (refreshFeatureLock(frame, lastDx_, lastDy_)) sinceRefreshSec_ = 0.0;
            }

            out.shift = stamp({ShiftMode::Subpixel, lastDx_, lastDy_, state_, method_, source_, m.elapsedMs});
            return out;
        }

        graceElapsedSec_ += frameIntervalSec;
        if (graceElapsedSec_ <= graceSeconds)
        {
            state_ = StabilizationState::GraceHold;
            out.shift = stamp({ShiftMode::Subpixel, lastDx_, lastDy_, state_, method_, source_, m.elapsedMs});
            return out;
        }

        state_ = StabilizationState::LostFallback;
        reacquireStreak_ = 0;
        lastDx_ = lastDy_ = 0.0;
        out.resetDetector = true;
        out.shift = {ShiftMode::Off, 0.0, 0.0, state_, method_, source_, m.elapsedMs};
        out.logEvent = "registration_lost reason=" + sanitizeReason(m.valid ? "step_too_large" : m.reason);
        return out;
    }

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
            out.shift = stamp({ShiftMode::Subpixel, 0.0, 0.0, state_, method_, source_, m.elapsedMs});
            out.logEvent = std::string("registration_locked method=") + stabilizationMethodName(method_);
            return out;
        }
    }

    out.shift = {ShiftMode::Off, 0.0, 0.0, StabilizationState::LostFallback, method_, source_, m.elapsedMs};
    return out;
}

void FrameStabilizer::populateLockBounds(FrameShift& shift) const
{
    if (shift.state != StabilizationState::Locked && shift.state != StabilizationState::GraceHold)
        return;
    if (method_ == StabilizationMethod::FeatureLock && lock_)
    {
        const LockRegion r = lock_->region();
        shift.lockX = r.x + static_cast<int>(std::lround(shift.dx - anchorDx_));
        shift.lockY = r.y + static_cast<int>(std::lround(shift.dy - anchorDy_));
        shift.lockWidth = r.width;
        shift.lockHeight = r.height;
    }
    else if (method_ == StabilizationMethod::DiskCentroid && refWeight_ > 0.0)
    {
        const double cx = refCx_ + shift.dx;
        const double cy = refCy_ + shift.dy;
        const int radius = std::clamp(static_cast<int>(std::lround(std::sqrt(refWeight_ / 3.14159265358979) * 1.15)), 24, 240);
        shift.lockX = static_cast<int>(std::lround(cx)) - radius;
        shift.lockY = static_cast<int>(std::lround(cy)) - radius;
        shift.lockWidth = radius * 2;
        shift.lockHeight = radius * 2;
    }
}
