#include "timelapse/stack_processor.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace {

constexpr int kLanczosRadius = 4;

int maxCodeFor(const Frame& frame)
{
    return frame.format == PixelFormat::Mono16 ? (1 << std::clamp(frame.significantBits, 1, 16)) - 1 : 255;
}

int shiftFor(const Frame& frame)
{
    return frame.alignment == BitAlignment::High ? std::max(0, 16 - frame.significantBits) : 0;
}

int maxCodeForRaw(PixelFormat format, int significantBits)
{
    return format == PixelFormat::Mono16 ? (1 << std::clamp(significantBits, 1, 16)) - 1 : 255;
}

int shiftForRaw(BitAlignment alignment, int significantBits)
{
    return alignment == BitAlignment::High ? std::max(0, 16 - significantBits) : 0;
}

int bayerToCvRgbCode(BayerPattern pattern)
{
    // SeleneWatch names patterns by top-left 2x2 cell; OpenCV names by (1,1)-offset cell.
    switch (pattern)
    {
    case BayerPattern::RGGB: return cv::COLOR_BayerBG2RGB_EA;
    case BayerPattern::GRBG: return cv::COLOR_BayerGB2RGB_EA;
    case BayerPattern::GBRG: return cv::COLOR_BayerGR2RGB_EA;
    case BayerPattern::BGGR: return cv::COLOR_BayerRG2RGB_EA;
    case BayerPattern::None: break;
    }
    return -1;
}

QRect clampRect(const QRect& r, int w, int h)
{
    if (w <= 0 || h <= 0 || !r.isValid()) return QRect(0, 0, w, h);
    const QRect clamped = r.intersected(QRect(0, 0, w, h));
    return clamped.width() >= 8 && clamped.height() >= 8 ? clamped : QRect(0, 0, w, h);
}

// Converts planar FitsImage to interleaved CV_32FC1 or CV_32FC3 (owning copy).
cv::Mat fitsToMat(const FitsImage& img)
{
    if (!img.valid()) return {};
    const size_t plane = static_cast<size_t>(img.width) * img.height;
    if (img.channels == 1)
    {
        cv::Mat m(img.height, img.width, CV_32FC1);
        std::memcpy(m.data, img.pixels.data(), plane * sizeof(float));
        return m;
    }
    std::vector<cv::Mat> ch(3);
    for (int c = 0; c < 3; ++c)
    {
        ch[c] = cv::Mat(img.height, img.width, CV_32FC1,
            const_cast<float*>(img.pixels.data() + static_cast<size_t>(c) * plane));
    }
    cv::Mat merged;
    cv::merge(ch, merged);
    return merged;
}

FitsImage matToFits(const cv::Mat& m, const FitsMetadata& meta = {})
{
    FitsImage img;
    img.width = m.cols;
    img.height = m.rows;
    img.channels = m.channels() == 3 ? 3 : 1;
    img.meta = meta;
    const size_t plane = static_cast<size_t>(img.width) * img.height;
    img.pixels.resize(plane * img.channels);
    if (img.channels == 1)
    {
        cv::Mat cont = m.isContinuous() ? m : m.clone();
        std::memcpy(img.pixels.data(), cont.data, plane * sizeof(float));
    }
    else
    {
        std::vector<cv::Mat> ch;
        cv::split(m, ch);
        for (int c = 0; c < 3; ++c)
        {
            cv::Mat cont = ch[c].isContinuous() ? ch[c] : ch[c].clone();
            std::memcpy(img.pixels.data() + static_cast<size_t>(c) * plane, cont.data, plane * sizeof(float));
        }
    }
    return img;
}

// Read-only view for mono (zero-copy) or direct merge of non-owning plane wrappers for RGB.
cv::Mat burstFrameToMat(const PreparedBurstFrame& bf)
{
    const size_t plane = static_cast<size_t>(bf.width) * bf.height;
    if (bf.channels == 1)
        return cv::Mat(bf.height, bf.width, CV_32FC1, const_cast<float*>(bf.pixels.data()));
    std::vector<cv::Mat> ch(3);
    for (int c = 0; c < 3; ++c)
    {
        ch[c] = cv::Mat(bf.height, bf.width, CV_32FC1,
            const_cast<float*>(bf.pixels.data() + static_cast<size_t>(c) * plane));
    }
    cv::Mat merged;
    cv::merge(ch, merged);
    return merged;
}

// Converts a worker-side RawBurstFrame into normalized float CV_32FC1 or CV_32FC3
// (running edge-aware Bayer debayering on the worker thread instead of acquisition).
cv::Mat rawBurstFrameToMat(const RawBurstFrame& raw)
{
    const int maxCode = maxCodeForRaw(raw.format, raw.significantBits);
    const int bitShift = shiftForRaw(raw.alignment, raw.significantBits);

    if (raw.format == PixelFormat::RGB888)
    {
        const cv::Mat full(raw.rawHeight, raw.rawWidth, CV_8UC3, const_cast<uint8_t*>(raw.data.data()));
        cv::Mat out;
        full(cv::Rect(raw.innerOffsetX, raw.innerOffsetY, raw.width, raw.height))
            .convertTo(out, CV_32FC3, 1.0 / 255.0);
        return out;
    }
    if (raw.bayer != BayerPattern::None)
    {
        const int code = bayerToCvRgbCode(raw.bayer);
        cv::Mat debayered;
        if (raw.format == PixelFormat::Mono8)
        {
            const cv::Mat bayerSub(raw.rawHeight, raw.rawWidth, CV_8UC1, const_cast<uint8_t*>(raw.data.data()));
            cv::demosaicing(bayerSub, debayered, code);
            debayered.convertTo(debayered, CV_32FC3, 1.0 / maxCode);
        }
        else
        {
            cv::Mat bayerSub(raw.rawHeight, raw.rawWidth, CV_16UC1);
            const auto* src = reinterpret_cast<const uint16_t*>(raw.data.data());
            const size_t n = static_cast<size_t>(raw.rawWidth) * raw.rawHeight;
            uint16_t* dst = bayerSub.ptr<uint16_t>(0);
            if (bitShift == 0)
            {
                std::memcpy(dst, src, n * sizeof(uint16_t));
            }
            else
            {
                for (size_t i = 0; i < n; ++i)
                    dst[i] = static_cast<uint16_t>(src[i] >> bitShift);
            }
            cv::demosaicing(bayerSub, debayered, code);
            debayered.convertTo(debayered, CV_32FC3, 1.0 / maxCode);
        }
        return debayered(cv::Rect(raw.innerOffsetX, raw.innerOffsetY, raw.width, raw.height)).clone();
    }
    if (raw.format == PixelFormat::Mono8)
    {
        const cv::Mat full(raw.rawHeight, raw.rawWidth, CV_8UC1, const_cast<uint8_t*>(raw.data.data()));
        cv::Mat out;
        full(cv::Rect(raw.innerOffsetX, raw.innerOffsetY, raw.width, raw.height))
            .convertTo(out, CV_32FC1, 1.0 / maxCode);
        return out;
    }
    cv::Mat out(raw.height, raw.width, CV_32FC1);
    const auto* src = reinterpret_cast<const uint16_t*>(raw.data.data());
    const float inv = 1.0f / static_cast<float>(maxCode);
    for (int y = 0; y < raw.height; ++y)
    {
        float* dstRow = out.ptr<float>(y);
        const uint16_t* srcRow = src + static_cast<size_t>(raw.innerOffsetY + y) * raw.rawWidth + raw.innerOffsetX;
        for (int x = 0; x < raw.width; ++x)
            dstRow[x] = static_cast<float>(srcRow[x] >> bitShift) * inv;
    }
    return out;
}

// Finds the largest axis-aligned rectangle strictly inside a binary valid mask (CV_8UC1 > 0).
// Conservative downsampling keeps cost <0.5 ms even on 4K frames while guaranteeing zero
// invalid (black) pixels inside the returned rectangle.
QRect largestInscribedRect(const cv::Mat& mask)
{
    const int fullW = mask.cols;
    const int fullH = mask.rows;
    if (fullW <= 0 || fullH <= 0) return {};

    int ds = 1;
    cv::Mat m = mask;
    if (fullW > 512 || fullH > 512)
    {
        ds = std::max(2, std::max(fullW, fullH) / 512);
        const int dw = fullW / ds;
        const int dh = fullH / ds;
        if (dw >= 4 && dh >= 4)
        {
            cv::Mat sub(dh, dw, CV_8UC1, cv::Scalar(0));
            for (int dy = 0; dy < dh; ++dy)
            {
                uint8_t* dstRow = sub.ptr<uint8_t>(dy);
                for (int dx = 0; dx < dw; ++dx)
                {
                    bool allValid = true;
                    const int y0 = dy * ds;
                    const int x0 = dx * ds;
                    for (int yy = 0; yy < ds && allValid; ++yy)
                    {
                        const uint8_t* srcRow = mask.ptr<uint8_t>(y0 + yy);
                        for (int xx = 0; xx < ds; ++xx)
                        {
                            if (srcRow[x0 + xx] == 0) { allValid = false; break; }
                        }
                    }
                    dstRow[dx] = allValid ? 255 : 0;
                }
            }
            m = std::move(sub);
        }
        else
        {
            ds = 1;
        }
    }

    const int mw = m.cols;
    const int mh = m.rows;
    std::vector<int> heights(static_cast<size_t>(mw), 0);
    std::vector<int> st;
    st.reserve(static_cast<size_t>(mw) + 1);
    int bestArea = 0;
    QRect best;
    for (int y = 0; y < mh; ++y)
    {
        const uint8_t* row = m.ptr<uint8_t>(y);
        for (int x = 0; x < mw; ++x)
            heights[static_cast<size_t>(x)] = row[x] > 0 ? heights[static_cast<size_t>(x)] + 1 : 0;
        st.clear();
        for (int x = 0; x <= mw; ++x)
        {
            const int curH = x < mw ? heights[static_cast<size_t>(x)] : 0;
            while (!st.empty() && heights[static_cast<size_t>(st.back())] > curH)
            {
                const int hTop = heights[static_cast<size_t>(st.back())];
                st.pop_back();
                const int left = st.empty() ? 0 : st.back() + 1;
                const int width = x - left;
                const int area = hTop * width;
                if (area > bestArea)
                {
                    bestArea = area;
                    best = QRect(left, y - hTop + 1, width, hTop);
                }
            }
            st.push_back(x);
        }
    }
    if (bestArea <= 0) return {};
    return QRect(best.x() * ds, best.y() * ds, best.width() * ds, best.height() * ds)
        .intersected(QRect(0, 0, fullW, fullH));
}

QRect validShiftOverlap(int w, int h, double minDx, double maxDx, double minDy, double maxDy)
{
    const int padL = maxDx > 1e-3 ? static_cast<int>(std::ceil(maxDx)) + kLanczosRadius : 0;
    const int padT = maxDy > 1e-3 ? static_cast<int>(std::ceil(maxDy)) + kLanczosRadius : 0;
    const int padR = -minDx > 1e-3 ? static_cast<int>(std::ceil(-minDx)) + kLanczosRadius : 0;
    const int padB = -minDy > 1e-3 ? static_cast<int>(std::ceil(-minDy)) + kLanczosRadius : 0;
    const int left = std::clamp(padL, 0, std::max(0, w - 2));
    const int top = std::clamp(padT, 0, std::max(0, h - 2));
    const int right = std::clamp(w - padR, left + 1, w);
    const int bottom = std::clamp(h - padB, top + 1, h);
    return QRect(left, top, right - left, bottom - top);
}

cv::Mat matLuminance(const cv::Mat& m)
{
    if (m.channels() == 1) return m;
    cv::Mat lum;
    // OpenCV RGB order (we keep R=0, G=1, B=2).
    cv::transform(m, lum, cv::Matx13f(0.2126f, 0.7152f, 0.0722f));
    return lum;
}

// Foreground mask separating illuminated target from dark sky when dark sky is present.
cv::Mat targetForegroundMask(const cv::Mat& lum, double* outTargetMean = nullptr)
{
    double minVal = 0, maxVal = 0;
    cv::minMaxLoc(lum, &minVal, &maxVal);
    const cv::Scalar allMean = cv::mean(lum);
    // Estimate dark background via 15th percentile sample on a coarse grid.
    std::vector<float> samples;
    const int stepY = std::max(1, lum.rows / 64);
    const int stepX = std::max(1, lum.cols / 64);
    samples.reserve(4200);
    for (int y = 0; y < lum.rows; y += stepY)
    {
        const float* row = lum.ptr<float>(y);
        for (int x = 0; x < lum.cols; x += stepX) samples.push_back(row[x]);
    }
    double bg = minVal;
    if (!samples.empty())
    {
        const size_t idx = samples.size() * 15 / 100;
        std::nth_element(samples.begin(), samples.begin() + static_cast<ptrdiff_t>(idx), samples.end());
        bg = samples[idx];
    }
    const double span = maxVal - bg;
    cv::Mat mask;
    if (span > 0.02 && (allMean[0] - bg) < 0.65 * span)
    {
        const double thresh = bg + 0.08 * span;
        cv::compare(lum, thresh, mask, cv::CMP_GT);
    }
    else
    {
        mask = cv::Mat(lum.size(), CV_8UC1, cv::Scalar(255));
    }
    if (cv::countNonZero(mask) < 16)
        mask = cv::Mat(lum.size(), CV_8UC1, cv::Scalar(255));
    if (outTargetMean)
        *outTargetMean = std::max(1e-5, cv::mean(lum, mask)[0]);
    return mask;
}

// Estimates subpixel shift needed to align `moving` onto `ref` using a Hann window.
// Returns (dx, dy) such that shifting `moving` by (+dx, +dy) aligns it with `ref`.
cv::Point2d estimateShiftPhase(const cv::Mat& refLum, const cv::Mat& movLum, const cv::Mat& hann, double* outResponse = nullptr)
{
    cv::Mat r, m;
    refLum.convertTo(r, CV_32F);
    movLum.convertTo(m, CV_32F);
    // Suppress broad illumination domes / shadow gradients so phase correlation
    // locks onto crisp craters and limbs rather than the patch window center.
    if (r.cols >= 24 && r.rows >= 24)
    {
        cv::Mat rLow, mLow;
        cv::GaussianBlur(r, rLow, cv::Size(21, 21), 4.0);
        cv::GaussianBlur(m, mLow, cv::Size(21, 21), 4.0);
        cv::Mat rHp = r - rLow;
        cv::Mat mHp = m - mLow;
        cv::Scalar stdR, stdM;
        cv::meanStdDev(rHp, cv::noArray(), stdR);
        cv::meanStdDev(mHp, cv::noArray(), stdM);
        if (stdR[0] > 1e-4 && stdM[0] > 1e-4)
        {
            r = std::move(rHp);
            m = std::move(mHp);
        }
    }
    r -= cv::mean(r)[0];
    m -= cv::mean(m)[0];
    const cv::Mat win = (!hann.empty() && hann.size() == r.size()) ? hann : cv::Mat();
    double response = 0;
    // phaseCorrelate(src1, src2) returns shift of src2 relative to src1.
    // Shifting `m` by -shift aligns `m` onto `r`.
    const cv::Point2d shift = cv::phaseCorrelate(r, m, win, &response);
    if (outResponse) *outResponse = response;
    if (!std::isfinite(shift.x) || !std::isfinite(shift.y) || response < 0.015)
        return {0.0, 0.0};
    return {-shift.x, -shift.y};
}

} // namespace

RawBurstFrame captureRawBurstFrame(const Frame& raw,
    const std::optional<QRect>& cropRoi,
    const std::optional<QRect>& qualityRoi)
{
    RawBurstFrame out;
    const size_t bpp = bytesPerPixel(raw.format);
    if (raw.width <= 0 || raw.height <= 0 || bpp == 0
        || raw.data.size() != frameBytes(raw.width, raw.height, raw.format))
    {
        return out;
    }

    const int maxCode = maxCodeFor(raw);
    const QRect fullRect(0, 0, raw.width, raw.height);
    const QRect crop = cropRoi ? clampRect(*cropRoi, raw.width, raw.height) : fullRect;

    int ex0 = crop.x();
    int ey0 = crop.y();
    int ew = crop.width();
    int eh = crop.height();
    if (raw.bayer != BayerPattern::None && raw.format != PixelFormat::RGB888)
    {
        // Keep even top-left coordinates and a 4 px margin so Bayer phase and
        // edge-aware demosaicing on the worker match full-frame debayering.
        constexpr int pad = 4;
        ex0 = std::max(0, (crop.x() - pad) & ~1);
        ey0 = std::max(0, (crop.y() - pad) & ~1);
        const int ex1 = std::min(raw.width, (crop.x() + crop.width() + pad + 1) & ~1);
        const int ey1 = std::min(raw.height, (crop.y() + crop.height() + pad + 1) & ~1);
        ew = ex1 - ex0;
        eh = ey1 - ey0;
    }

    out.frameIndex = raw.frameIndex;
    out.timestamp = raw.timestamp;
    out.width = crop.width();
    out.height = crop.height();
    out.rawWidth = ew;
    out.rawHeight = eh;
    out.innerOffsetX = crop.x() - ex0;
    out.innerOffsetY = crop.y() - ey0;
    out.format = raw.format;
    out.significantBits = raw.significantBits;
    out.alignment = raw.alignment;
    out.bayer = raw.bayer;
    out.rawMaxAdu = maxCode;

    // Copy only the raw bytes for the cropped region (4x-12x smaller than float32 RGB).
    out.data.resize(static_cast<size_t>(ew) * eh * bpp);
    const size_t srcStride = static_cast<size_t>(raw.width) * bpp;
    const size_t dstStride = static_cast<size_t>(ew) * bpp;
    const uint8_t* srcBase = raw.data.data() + static_cast<size_t>(ey0) * srcStride + static_cast<size_t>(ex0) * bpp;
    if (ew == raw.width && ex0 == 0)
    {
        std::memcpy(out.data.data(), srcBase, out.data.size());
    }
    else
    {
        for (int y = 0; y < eh; ++y)
            std::memcpy(out.data.data() + static_cast<size_t>(y) * dstStride,
                srcBase + static_cast<size_t>(y) * srcStride, dstStride);
    }

    // Fast sharpness scoring on the quality ROI without debayering or full-frame float copy.
    QRect qRoi(0, 0, crop.width(), crop.height());
    if (qualityRoi)
    {
        const QRect shifted = qualityRoi->translated(-crop.x(), -crop.y());
        qRoi = clampRect(shifted, crop.width(), crop.height());
    }

    const bool isBayer = (raw.bayer != BayerPattern::None && raw.format != PixelFormat::RGB888);
    const int longSide = std::max(qRoi.width(), qRoi.height());
    int step = std::max(1, longSide / 320);
    if (isBayer) step = std::max(2, (step + 1) & ~1);

    // For Bayer sensors, sample pure green pixels directly at even rows.
    const int qx0 = crop.x() + qRoi.x();
    const int qy0 = crop.y() + qRoi.y();
    int startX = qx0;
    int startY = qy0;
    if (isBayer)
    {
        const int gColParity = (raw.bayer == BayerPattern::RGGB || raw.bayer == BayerPattern::BGGR) ? 1 : 0;
        if ((startY & 1) != 0) ++startY;
        if ((startX & 1) != gColParity) ++startX;
    }
    const int endX = qx0 + qRoi.width();
    const int endY = qy0 + qRoi.height();
    const int sw = std::max(0, (endX - startX + step - 1) / step);
    const int sh = std::max(0, (endY - startY + step - 1) / step);
    if (sw < 6 || sh < 6) return out;

    cv::Mat qPatch(sh, sw, CV_32FC1);
    const float invMax = 1.0f / static_cast<float>(maxCode);
    const int bitShift = shiftFor(raw);
    if (raw.format == PixelFormat::RGB888)
    {
        for (int py = 0; py < sh; ++py)
        {
            float* dst = qPatch.ptr<float>(py);
            const uint8_t* srcRow = raw.data.data() + static_cast<size_t>(startY + py * step) * raw.width * 3;
            for (int px = 0; px < sw; ++px)
            {
                const uint8_t* p = srcRow + static_cast<size_t>(startX + px * step) * 3;
                dst[px] = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) * (1.0f / 255.0f);
            }
        }
    }
    else if (raw.format == PixelFormat::Mono8)
    {
        for (int py = 0; py < sh; ++py)
        {
            float* dst = qPatch.ptr<float>(py);
            const uint8_t* srcRow = raw.data.data() + static_cast<size_t>(startY + py * step) * raw.width;
            for (int px = 0; px < sw; ++px)
                dst[px] = static_cast<float>(srcRow[startX + px * step]) * invMax;
        }
    }
    else
    {
        const auto* src16 = reinterpret_cast<const uint16_t*>(raw.data.data());
        for (int py = 0; py < sh; ++py)
        {
            float* dst = qPatch.ptr<float>(py);
            const uint16_t* srcRow = src16 + static_cast<size_t>(startY + py * step) * raw.width;
            for (int px = 0; px < sw; ++px)
                dst[px] = static_cast<float>(srcRow[startX + px * step] >> bitShift) * invMax;
        }
    }

    double targetBrightness = 0;
    const cv::Mat fgMask = targetForegroundMask(qPatch, &targetBrightness);
    cv::Mat smoothed, lap;
    cv::GaussianBlur(qPatch, smoothed, cv::Size(5, 5), 1.0);
    cv::Laplacian(smoothed, lap, CV_32F, 3);
    cv::Mat lapSq = lap.mul(lap);
    const double rawSharpness = cv::mean(lapSq, fgMask)[0];
    // Regularize the denominator against sensor/quantization noise so thick-cloud
    // frames with tiny targetBrightness cannot outscore sharp clear frames.
    constexpr double kSharpnessBrightnessFloorSq = 0.004;
    out.sharpness = rawSharpness / (targetBrightness * targetBrightness + kSharpnessBrightnessFloorSq);
    out.targetBrightness = targetBrightness;
    return out;
}

PreparedBurstFrame prepareBurstFrame(const Frame& raw,
    const std::optional<QRect>& cropRoi,
    const std::optional<QRect>& qualityRoi)
{
    PreparedBurstFrame out;
    if (raw.width <= 0 || raw.height <= 0 || bytesPerPixel(raw.format) == 0
        || raw.data.size() != frameBytes(raw.width, raw.height, raw.format))
    {
        return out;
    }

    const int maxCode = maxCodeFor(raw);
    const QRect fullRect(0, 0, raw.width, raw.height);
    const QRect crop = cropRoi ? clampRect(*cropRoi, raw.width, raw.height) : fullRect;

    cv::Mat colorOrGray;
    if (raw.format == PixelFormat::RGB888)
    {
        const cv::Mat full(raw.height, raw.width, CV_8UC3, const_cast<uint8_t*>(raw.data.data()));
        const cv::Mat sub = full(cv::Rect(crop.x(), crop.y(), crop.width(), crop.height()));
        sub.convertTo(colorOrGray, CV_32FC3, 1.0 / 255.0);
    }
    else if (raw.bayer != BayerPattern::None)
    {
        const int code = bayerToCvRgbCode(raw.bayer);
        // Keep even top-left coordinates and a small margin before debayering so Bayer phase is preserved.
        const int pad = 4;
        const int ex0 = std::max(0, (crop.x() - pad) & ~1);
        const int ey0 = std::max(0, (crop.y() - pad) & ~1);
        const int ex1 = std::min(raw.width, (crop.x() + crop.width() + pad + 1) & ~1);
        const int ey1 = std::min(raw.height, (crop.y() + crop.height() + pad + 1) & ~1);
        const int ew = ex1 - ex0;
        const int eh = ey1 - ey0;

        cv::Mat debayered;
        if (raw.format == PixelFormat::Mono8)
        {
            const cv::Mat full(raw.height, raw.width, CV_8UC1, const_cast<uint8_t*>(raw.data.data()));
            const cv::Mat bayerSub = full(cv::Rect(ex0, ey0, ew, eh));
            cv::demosaicing(bayerSub, debayered, code);
            debayered.convertTo(debayered, CV_32FC3, 1.0 / maxCode);
        }
        else
        {
            const int bitShift = shiftFor(raw);
            cv::Mat bayerSub(eh, ew, CV_16UC1);
            const auto* src = reinterpret_cast<const uint16_t*>(raw.data.data());
            for (int y = 0; y < eh; ++y)
            {
                uint16_t* dstRow = bayerSub.ptr<uint16_t>(y);
                const uint16_t* srcRow = src + static_cast<size_t>(ey0 + y) * raw.width + ex0;
                for (int x = 0; x < ew; ++x)
                    dstRow[x] = static_cast<uint16_t>(srcRow[x] >> bitShift);
            }
            cv::demosaicing(bayerSub, debayered, code);
            debayered.convertTo(debayered, CV_32FC3, 1.0 / maxCode);
        }
        const cv::Rect inner(crop.x() - ex0, crop.y() - ey0, crop.width(), crop.height());
        colorOrGray = debayered(inner).clone();
    }
    else if (raw.format == PixelFormat::Mono8)
    {
        const cv::Mat full(raw.height, raw.width, CV_8UC1, const_cast<uint8_t*>(raw.data.data()));
        const cv::Mat sub = full(cv::Rect(crop.x(), crop.y(), crop.width(), crop.height()));
        sub.convertTo(colorOrGray, CV_32FC1, 1.0 / maxCode);
    }
    else
    {
        const int bitShift = shiftFor(raw);
        colorOrGray = cv::Mat(crop.height(), crop.width(), CV_32FC1);
        const auto* src = reinterpret_cast<const uint16_t*>(raw.data.data());
        const float inv = 1.0f / static_cast<float>(maxCode);
        for (int y = 0; y < crop.height(); ++y)
        {
            float* dstRow = colorOrGray.ptr<float>(y);
            const uint16_t* srcRow = src + static_cast<size_t>(crop.y() + y) * raw.width + crop.x();
            for (int x = 0; x < crop.width(); ++x)
                dstRow[x] = static_cast<float>(srcRow[x] >> bitShift) * inv;
        }
    }

    const cv::Mat lum = matLuminance(colorOrGray);
    QRect qRoi(0, 0, crop.width(), crop.height());
    if (qualityRoi)
    {
        const QRect shifted = qualityRoi->translated(-crop.x(), -crop.y());
        qRoi = clampRect(shifted, crop.width(), crop.height());
    }
    const cv::Mat qPatch = lum(cv::Rect(qRoi.x(), qRoi.y(), qRoi.width(), qRoi.height()));
    double targetBrightness = 0;
    const cv::Mat fgMask = targetForegroundMask(qPatch, &targetBrightness);

    cv::Mat smoothed, lap;
    cv::GaussianBlur(qPatch, smoothed, cv::Size(5, 5), 1.0);
    cv::Laplacian(smoothed, lap, CV_32F, 3);
    cv::Mat lapSq = lap.mul(lap);
    const double rawSharpness = cv::mean(lapSq, fgMask)[0];
    constexpr double kSharpnessBrightnessFloorSq = 0.004;
    const double normSharpness = rawSharpness / (targetBrightness * targetBrightness + kSharpnessBrightnessFloorSq);

    const FitsImage packed = matToFits(colorOrGray);
    out.frameIndex = raw.frameIndex;
    out.timestamp = raw.timestamp;
    out.width = packed.width;
    out.height = packed.height;
    out.channels = packed.channels;
    out.rawMaxAdu = maxCode;
    out.pixels = std::move(packed.pixels);
    out.sharpness = normSharpness;
    out.targetBrightness = targetBrightness;
    return out;
}

namespace {

BurstStackResult stackBurstImpl(size_t count, int w, int h, int ch, double rawMaxAdu,
    const std::function<double(size_t)>& sharpnessOf,
    const std::function<cv::Mat(size_t)>& matOf,
    const std::optional<QRect>& alignRoi,
    std::atomic<bool>* cancel)
{
    BurstStackResult result;
    if (count == 0)
    {
        result.error = "no frames in burst";
        return result;
    }
    if (w <= 0 || h <= 0)
    {
        result.error = "invalid burst frame size";
        return result;
    }

    // Order indices by sharpness descending so pass 1 averages the cleanest frames.
    std::vector<size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return sharpnessOf(a) > sharpnessOf(b);
    });

    const QRect roi = alignRoi ? clampRect(*alignRoi, w, h) : QRect(0, 0, w, h);
    const cv::Rect cvRoi(roi.x(), roi.y(), roi.width(), roi.height());
    cv::Mat hann;
    cv::createHanningWindow(hann, cv::Size(roi.width(), roi.height()), CV_32F);

    const cv::Mat bestMat = matOf(order.front());
    const cv::Mat bestLum = matLuminance(bestMat);
    const cv::Mat bestPatch = bestLum(cvRoi);

    // Pass 1: align top subset to sharpest frame to build a low-noise reference.
    const size_t refCount = std::min(count, std::max<size_t>(1, std::max<size_t>(4, count / 4)));
    cv::Mat refAccum = cv::Mat::zeros(h, w, CV_32FC1);
    for (size_t i = 0; i < refCount; ++i)
    {
        if (cancel && cancel->load())
        {
            result.error = "cancelled";
            return result;
        }
        const cv::Mat lum = (i == 0) ? bestLum : matLuminance(matOf(order[i]));
        cv::Point2d delta(0.0, 0.0);
        if (i > 0)
        {
            delta = estimateShiftPhase(bestPatch, lum(cvRoi), hann);
            if (std::abs(delta.x) > roi.width() * 0.4 || std::abs(delta.y) > roi.height() * 0.4)
                delta = {0.0, 0.0};
        }
        const cv::Matx23d M(1.0, 0.0, delta.x, 0.0, 1.0, delta.y);
        cv::Mat warped;
        cv::warpAffine(lum, warped, M, cv::Size(w, h), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        refAccum += warped;
    }
    const cv::Mat meanRefLum = refAccum * (1.0 / static_cast<double>(refCount));
    const cv::Mat meanRefPatch = meanRefLum(cvRoi);

    // Pass 2a: align every kept frame to meanRefLum, track common overlap, sum and sumSq.
    const int cvType = ch == 3 ? CV_32FC3 : CV_32FC1;
    cv::Mat sum = cv::Mat::zeros(h, w, cvType);
    cv::Mat sumSq = cv::Mat::zeros(h, w, cvType);
    std::vector<cv::Point2d> shifts(count);
    double minDx = 0, maxDx = 0, minDy = 0, maxDy = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (cancel && cancel->load())
        {
            result.error = "cancelled";
            return result;
        }
        const cv::Mat frameMat = matOf(i);
        const cv::Mat lum = matLuminance(frameMat);
        cv::Point2d delta = estimateShiftPhase(meanRefPatch, lum(cvRoi), hann);
        if (std::abs(delta.x) > roi.width() * 0.4 || std::abs(delta.y) > roi.height() * 0.4)
            delta = {0.0, 0.0};
        shifts[i] = delta;
        if (i == 0) { minDx = maxDx = delta.x; minDy = maxDy = delta.y; }
        else
        {
            minDx = std::min(minDx, delta.x); maxDx = std::max(maxDx, delta.x);
            minDy = std::min(minDy, delta.y); maxDy = std::max(maxDy, delta.y);
        }

        const cv::Matx23d M(1.0, 0.0, delta.x, 0.0, 1.0, delta.y);
        cv::Mat warped;
        // BORDER_REPLICATE prevents Lanczos-4 from pulling constant-0 black edges into the valid area.
        cv::warpAffine(frameMat, warped, M, cv::Size(w, h), cv::INTER_LANCZOS4, cv::BORDER_REPLICATE);
        cv::max(warped, 0.0f, warped);
        sum += warped;
        sumSq += warped.mul(warped);
    }

    const double invN = 1.0 / static_cast<double>(count);
    cv::Mat mean = sum * invN;

    // Pass 2b: sigma-clipped average when >= 6 frames to suppress hot pixels / transits.
    if (count >= 6)
    {
        cv::Mat var = sumSq * invN - mean.mul(mean);
        cv::max(var, 1e-8f, var);
        cv::Mat stdDev;
        cv::sqrt(var, stdDev);
        const cv::Mat clipTol = stdDev * 2.5 + cv::Scalar::all(0.015);

        std::vector<cv::Mat> meanCh, tolCh, clipSumCh(ch), clipWeightCh(ch);
        cv::split(mean, meanCh);
        cv::split(clipTol, tolCh);
        for (int c = 0; c < ch; ++c)
        {
            clipSumCh[c] = cv::Mat::zeros(h, w, CV_32FC1);
            clipWeightCh[c] = cv::Mat::zeros(h, w, CV_32FC1);
        }
        for (size_t i = 0; i < count; ++i)
        {
            if (cancel && cancel->load())
            {
                result.error = "cancelled";
                return result;
            }
            const cv::Mat frameMat = matOf(i);
            const cv::Matx23d M(1.0, 0.0, shifts[i].x, 0.0, 1.0, shifts[i].y);
            cv::Mat warped;
            cv::warpAffine(frameMat, warped, M, cv::Size(w, h), cv::INTER_LANCZOS4, cv::BORDER_REPLICATE);
            cv::max(warped, 0.0f, warped);

            std::vector<cv::Mat> wCh;
            cv::split(warped, wCh);
            for (int c = 0; c < ch; ++c)
            {
                cv::Mat diff, inlierMask, wMask;
                cv::absdiff(wCh[c], meanCh[c], diff);
                cv::compare(diff, tolCh[c], inlierMask, cv::CMP_LE);
                inlierMask.convertTo(wMask, CV_32FC1, 1.0 / 255.0);
                clipSumCh[c] += wCh[c].mul(wMask);
                clipWeightCh[c] += wMask;
            }
        }
        for (int c = 0; c < ch; ++c)
        {
            cv::Mat hasWeight, safeWeight;
            cv::compare(clipWeightCh[c], 0.5f, hasWeight, cv::CMP_GT);
            cv::max(clipWeightCh[c], 1.0f, safeWeight);
            cv::Mat clippedMean = clipSumCh[c] / safeWeight;
            clippedMean.copyTo(meanCh[c], hasWeight);
        }
        cv::merge(meanCh, mean);
    }

    cv::min(mean, 1.0f, mean);
    cv::max(mean, 0.0f, mean);

    result.stack = matToFits(mean);
    result.stack.meta.rawMaxAdu = rawMaxAdu;
    result.validOverlap = validShiftOverlap(w, h, minDx, maxDx, minDy, maxDy);
    result.ok = true;
    return result;
}

} // namespace

BurstStackResult stackBurst(const std::vector<PreparedBurstFrame>& kept,
    const std::optional<QRect>& alignRoi,
    std::atomic<bool>* cancel)
{
    if (kept.empty())
    {
        BurstStackResult res;
        res.error = "no frames in burst";
        return res;
    }
    return stackBurstImpl(kept.size(), kept.front().width, kept.front().height,
        kept.front().channels, kept.front().rawMaxAdu,
        [&](size_t i) { return kept[i].sharpness; },
        [&](size_t i) { return burstFrameToMat(kept[i]); },
        alignRoi, cancel);
}

BurstStackResult stackBurst(const std::vector<RawBurstFrame>& kept,
    const std::optional<QRect>& alignRoi,
    std::atomic<bool>* cancel)
{
    if (kept.empty())
    {
        BurstStackResult res;
        res.error = "no frames in burst";
        return res;
    }
    const int ch = (kept.front().format == PixelFormat::RGB888 || kept.front().bayer != BayerPattern::None) ? 3 : 1;
    return stackBurstImpl(kept.size(), kept.front().width, kept.front().height,
        ch, kept.front().rawMaxAdu,
        [&](size_t i) { return kept[i].sharpness; },
        [&](size_t i) { return rawBurstFrameToMat(kept[i]); },
        alignRoi, cancel);
}

SessionAlignResult alignStackToReference(const FitsImage& stack,
    const FitsImage& reference,
    TimelapsePreset preset,
    const std::optional<QRect>& lunarAnchor)
{
    SessionAlignResult out;
    out.aligned = stack;
    out.validOverlap = QRect(0, 0, stack.width, stack.height);
    if (!stack.valid() || !reference.valid() || stack.width != reference.width || stack.height != reference.height)
    {
        out.usedFallback = true;
        return out;
    }

    const int w = stack.width;
    const int h = stack.height;
    const cv::Mat srcMat = fitsToMat(stack);
    const cv::Mat refMat = fitsToMat(reference);
    const cv::Mat srcLum = matLuminance(srcMat);
    const cv::Mat refLum = matLuminance(refMat);

    if (preset == TimelapsePreset::Planet)
    {
        const auto centroidOf = [](const cv::Mat& lum) -> std::optional<cv::Point2d> {
            const cv::Mat mask = targetForegroundMask(lum);
            cv::Mat masked;
            lum.copyTo(masked, mask);
            const cv::Moments mom = cv::moments(masked, false);
            if (mom.m00 <= 1e-6) return std::nullopt;
            return cv::Point2d(mom.m10 / mom.m00, mom.m01 / mom.m00);
        };
        const auto cRef = centroidOf(refLum);
        const auto cSrc = centroidOf(srcLum);
        if (cRef && cSrc)
        {
            out.shiftX = cRef->x - cSrc->x;
            out.shiftY = cRef->y - cSrc->y;
        }
        else
        {
            cv::Mat hann;
            cv::createHanningWindow(hann, refLum.size(), CV_32F);
            const cv::Point2d d = estimateShiftPhase(refLum, srcLum, hann);
            out.shiftX = d.x;
            out.shiftY = d.y;
            out.usedFallback = true;
        }

        const cv::Matx23d M(1.0, 0.0, out.shiftX, 0.0, 1.0, out.shiftY);
        cv::Mat warped;
        cv::warpAffine(srcMat, warped, M, cv::Size(w, h), cv::INTER_LANCZOS4, cv::BORDER_CONSTANT, cv::Scalar::all(0));
        cv::min(warped, 1.0f, warped);
        cv::max(warped, 0.0f, warped);
        out.aligned = matToFits(warped, stack.meta);
        out.validOverlap = validShiftOverlap(w, h, out.shiftX, out.shiftX, out.shiftY, out.shiftY);
        return out;
    }

    // Moon: 2D rigid translation + rotation, anchored to lunarAnchor if selected.
    cv::Mat fullHann;
    cv::createHanningWindow(fullHann, refLum.size(), CV_32F);
    double coarseResp = 0;
    cv::Point2d coarse = estimateShiftPhase(refLum, srcLum, fullHann, &coarseResp);

    std::vector<cv::Point2d> initialShifts;
    cv::Mat mask(h, w, CV_8UC1, cv::Scalar(255));
    if (lunarAnchor && lunarAnchor->isValid())
    {
        const QRect anchor = clampRect(*lunarAnchor, w, h);
        mask.setTo(0);
        mask(cv::Rect(anchor.x(), anchor.y(), anchor.width(), anchor.height())).setTo(255);

        // If user selected a constant sunlit anchor, prefer coarse shift inside the anchor
        // so moving shadows outside the anchor cannot bias the initial translation.
        if (anchor.width() >= 32 && anchor.height() >= 32)
        {
            cv::Mat aHann;
            cv::createHanningWindow(aHann, cv::Size(anchor.width(), anchor.height()), CV_32F);
            const cv::Rect ar(anchor.x(), anchor.y(), anchor.width(), anchor.height());
            double anchorResp = 0;
            const cv::Point2d anchorShift = estimateShiftPhase(refLum(ar), srcLum(ar), aHann, &anchorResp);
            if (anchorResp > 0.04) initialShifts.push_back(anchorShift);
        }
    }
    initialShifts.push_back(coarse);
    if (std::hypot(coarse.x, coarse.y) > 1.0)
        initialShifts.emplace_back(0.0, 0.0);

    // Build coarse-to-fine Gaussian pyramid for large shifts + off-center rotation.
    std::vector<cv::Mat> pyrRef{refLum};
    std::vector<cv::Mat> pyrSrc{srcLum};
    std::vector<cv::Mat> pyrMask{mask};
    while (pyrRef.size() < 3 && pyrRef.back().cols >= 192 && pyrRef.back().rows >= 192)
    {
        cv::Mat rDown, sDown, mDown;
        cv::pyrDown(pyrRef.back(), rDown);
        cv::pyrDown(pyrSrc.back(), sDown);
        cv::resize(pyrMask.back(), mDown, rDown.size(), 0, 0, cv::INTER_NEAREST);
        if (cv::countNonZero(mDown) < 64) break;
        pyrRef.push_back(std::move(rDown));
        pyrSrc.push_back(std::move(sDown));
        pyrMask.push_back(std::move(mDown));
    }

    const int topLvl = static_cast<int>(pyrRef.size()) - 1;
    const float scaleDown = static_cast<float>(1 << topLvl);
    cv::Mat warpMatrix;
    bool eccOk = false;
    try
    {
        double bestTopCc = -2.0;
        for (const cv::Point2d& init : initialShifts)
        {
            cv::Mat cand(cv::Matx23f(
                1.0f, 0.0f, static_cast<float>(-init.x / scaleDown),
                0.0f, 1.0f, static_cast<float>(-init.y / scaleDown)));
            try
            {
                const cv::TermCriteria criteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 60, 1e-4);
                const double cc = cv::findTransformECC(pyrRef[static_cast<size_t>(topLvl)],
                    pyrSrc[static_cast<size_t>(topLvl)], cand, cv::MOTION_EUCLIDEAN, criteria,
                    pyrMask[static_cast<size_t>(topLvl)]);
                if (cc > bestTopCc)
                {
                    bestTopCc = cc;
                    warpMatrix = cand.clone();
                }
                if (bestTopCc >= 0.99) break;
            }
            catch (const cv::Exception&) {}
        }

        if (!warpMatrix.empty())
        {
            for (int lvl = topLvl - 1; lvl >= 0; --lvl)
            {
                cv::Mat scaled = warpMatrix.clone();
                scaled.at<float>(0, 2) *= 2.0f;
                scaled.at<float>(1, 2) *= 2.0f;
                try
                {
                    const int iters = lvl == 0 ? 80 : 60;
                    const cv::TermCriteria criteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, iters, 1e-4);
                    cv::findTransformECC(pyrRef[static_cast<size_t>(lvl)], pyrSrc[static_cast<size_t>(lvl)],
                        scaled, cv::MOTION_EUCLIDEAN, criteria, pyrMask[static_cast<size_t>(lvl)]);
                    warpMatrix = std::move(scaled);
                }
                catch (const cv::Exception&)
                {
                    // Keep the previous converged level's transform scaled to this level's resolution.
                    warpMatrix = std::move(scaled);
                }
            }
            const double cosT = warpMatrix.at<float>(0, 0);
            const double sinT = warpMatrix.at<float>(1, 0);
            const double angleDeg = std::atan2(sinT, cosT) * 180.0 / 3.14159265358979323846;
            const double tx = warpMatrix.at<float>(0, 2);
            const double ty = warpMatrix.at<float>(1, 2);
            if (std::isfinite(angleDeg) && std::abs(angleDeg) <= 25.0
                && std::abs(tx) < w * 0.5 && std::abs(ty) < h * 0.5)
            {
                eccOk = true;
            }
        }
    }
    catch (const cv::Exception&)
    {
        eccOk = false;
    }

    cv::Mat warped;
    if (eccOk)
    {
        cv::warpAffine(srcMat, warped, warpMatrix, cv::Size(w, h),
            cv::INTER_LANCZOS4 | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, cv::Scalar::all(0));
        // Compute forward translation of frame center and rotation angle for telemetry.
        const cv::Matx23d invM(
            warpMatrix.at<float>(0, 0), warpMatrix.at<float>(0, 1), warpMatrix.at<float>(0, 2),
            warpMatrix.at<float>(1, 0), warpMatrix.at<float>(1, 1), warpMatrix.at<float>(1, 2));
        cv::Mat fwdM;
        cv::invertAffineTransform(cv::Mat(invM), fwdM);
        const double cx = w * 0.5;
        const double cy = h * 0.5;
        const double nx = fwdM.at<double>(0, 0) * cx + fwdM.at<double>(0, 1) * cy + fwdM.at<double>(0, 2);
        const double ny = fwdM.at<double>(1, 0) * cx + fwdM.at<double>(1, 1) * cy + fwdM.at<double>(1, 2);
        out.shiftX = nx - cx;
        out.shiftY = ny - cy;
        out.rotationDeg = std::atan2(fwdM.at<double>(1, 0), fwdM.at<double>(0, 0)) * 180.0 / 3.14159265358979323846;

        // Compute the largest inscribed valid rectangle strictly inside the warped mask,
        // eroding warped black borders by the Lanczos-4 kernel radius (4 px) while keeping
        // unshifted image edges intact via BORDER_CONSTANT=255.
        cv::Mat ones(h, w, CV_8UC1, cv::Scalar(255));
        cv::Mat validMask;
        cv::warpAffine(ones, validMask, warpMatrix, cv::Size(w, h),
            cv::INTER_NEAREST | cv::WARP_INVERSE_MAP, cv::BORDER_CONSTANT, cv::Scalar(0));
        cv::Mat erodedMask;
        const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT,
            cv::Size(2 * kLanczosRadius + 1, 2 * kLanczosRadius + 1));
        cv::erode(validMask, erodedMask, kernel, cv::Point(-1, -1), 1, cv::BORDER_CONSTANT, cv::Scalar(255));
        QRect inscribed = largestInscribedRect(erodedMask);
        if (inscribed.width() < 2 || inscribed.height() < 2)
            inscribed = largestInscribedRect(validMask);
        if (inscribed.width() >= 2 && inscribed.height() >= 2)
            out.validOverlap = inscribed;
    }
    else
    {
        out.usedFallback = true;
        out.shiftX = coarse.x;
        out.shiftY = coarse.y;
        out.rotationDeg = 0.0;
        const cv::Matx23d M(1.0, 0.0, coarse.x, 0.0, 1.0, coarse.y);
        cv::warpAffine(srcMat, warped, M, cv::Size(w, h), cv::INTER_LANCZOS4, cv::BORDER_CONSTANT, cv::Scalar::all(0));
        out.validOverlap = validShiftOverlap(w, h, coarse.x, coarse.x, coarse.y, coarse.y);
    }

    cv::min(warped, 1.0f, warped);
    cv::max(warped, 0.0f, warped);
    out.aligned = matToFits(warped, stack.meta);
    return out;
}

FitsImage applyLinearSharpening(const FitsImage& unsharpened, const SharpeningSettings& settings)
{
    if (!unsharpened.valid()) return unsharpened;
    cv::Mat src = fitsToMat(unsharpened);
    if (settings.denoise > 0.0)
    {
        cv::Mat denoised;
        cv::bilateralFilter(src, denoised, 5, 0.04 * settings.denoise, 1.2 + 1.5 * settings.denoise);
        src = denoised;
    }
    if (settings.amount > 0.0 && settings.preset != SharpeningPreset::Off)
    {
        cv::Mat blurred;
        cv::GaussianBlur(src, blurred, cv::Size(0, 0), settings.radius);
        cv::Mat diff = src - blurred;
        if (settings.threshold > 0.0)
        {
            const float thresh = static_cast<float>(settings.threshold);
            cv::Mat absDiff = cv::abs(diff);
            cv::Mat excess = absDiff - cv::Scalar::all(thresh);
            cv::max(excess, 0.0f, excess);
            cv::Mat denom;
            cv::max(absDiff, 1e-6f, denom);
            cv::Mat sign;
            cv::divide(diff, denom, sign);
            diff = sign.mul(excess);
        }
        src = src + diff * static_cast<float>(settings.amount);
    }
    cv::min(src, 1.0f, src);
    cv::max(src, 0.0f, src);
    FitsImage out = matToFits(src, unsharpened.meta);
    out.meta.sharpeningProfile = settings.profileName.toStdString();
    return out;
}

namespace {

cv::Mat croppedView(const cv::Mat& m, const QRect& crop)
{
    if (!crop.isValid() || crop.isEmpty()) return m;
    const QRect r = crop.intersected(QRect(0, 0, m.cols, m.rows));
    if (r.width() >= 2 && r.height() >= 2)
        return m(cv::Rect(r.x(), r.y(), r.width(), r.height()));
    return m;
}

std::vector<float> sampleLuminanceGrid(const cv::Mat& lum, int targetGrid = 96)
{
    std::vector<float> samples;
    if (lum.empty()) return samples;
    const int stepY = std::max(1, lum.rows / targetGrid);
    const int stepX = std::max(1, lum.cols / targetGrid);
    samples.reserve(static_cast<size_t>((lum.rows + stepY - 1) / stepY)
        * static_cast<size_t>((lum.cols + stepX - 1) / stepX));
    for (int y = 0; y < lum.rows; y += stepY)
    {
        const float* row = lum.ptr<float>(y);
        for (int x = 0; x < lum.cols; x += stepX) samples.push_back(row[x]);
    }
    return samples;
}

double quantileOfSorted(const std::vector<float>& sorted, double frac)
{
    if (sorted.empty()) return 0.0;
    const double pos = std::clamp(frac, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(sorted.size() - 1, lo + 1);
    const double t = pos - static_cast<double>(lo);
    return static_cast<double>(sorted[lo]) * (1.0 - t) + static_cast<double>(sorted[hi]) * t;
}

BrightnessMatch foregroundAffineMatch(const std::vector<float>& cSorted, const std::vector<float>& rSorted)
{
    BrightnessMatch out;
    if (cSorted.empty() || rSorted.empty()) return out;

    const double rBg = quantileOfSorted(rSorted, 0.15);
    const double rHi995 = quantileOfSorted(rSorted, 0.995);
    const double rFgThresh = rBg + 0.06 * std::max(1e-4, rHi995 - rBg);
    const auto rFirstFg = std::upper_bound(rSorted.begin(), rSorted.end(), static_cast<float>(rFgThresh));
    const double fgFrac = static_cast<double>(rSorted.end() - rFirstFg) / static_cast<double>(rSorted.size());

    double cLo = quantileOfSorted(cSorted, 0.01);
    double cHi = quantileOfSorted(cSorted, 0.985);
    double rLo = quantileOfSorted(rSorted, 0.01);
    double rHi = quantileOfSorted(rSorted, 0.985);

    // Small target on dark sky (e.g. a planet disc): anchor background floor and foreground upper quantile.
    if (fgFrac < 0.30)
    {
        const double cBg = quantileOfSorted(cSorted, 0.15);
        const double cHi995 = quantileOfSorted(cSorted, 0.995);
        const double cFgThresh = cBg + 0.06 * std::max(1e-4, cHi995 - cBg);
        const auto cFirstFg = std::upper_bound(cSorted.begin(), cSorted.end(), static_cast<float>(cFgThresh));
        const ptrdiff_t rCount = rSorted.end() - rFirstFg;
        const ptrdiff_t cCount = cSorted.end() - cFirstFg;
        if (rCount >= 24 && cCount >= 24)
        {
            const std::vector<float> rFg(rFirstFg, rSorted.end());
            const std::vector<float> cFg(cFirstFg, cSorted.end());
            rLo = rBg;
            cLo = cBg;
            rHi = quantileOfSorted(rFg, 0.95);
            cHi = quantileOfSorted(cFg, 0.95);
        }
    }

    const double cSpan = cHi - cLo;
    const double rSpan = rHi - rLo;
    if (cSpan > 1e-4 && rSpan > 1e-4)
    {
        out.gain = std::clamp(rSpan / cSpan, 0.1, 15.0);
        out.offset = rLo - out.gain * cLo;
    }
    return out;
}

BrightnessMatch computeBrightnessMatchMats(const cv::Mat& curLum, const cv::Mat& refLum,
    bool alignedPixels, bool highlightGuard)
{
    BrightnessMatch match;
    if (curLum.empty() || refLum.empty()) return match;

    std::vector<float> cSorted = sampleLuminanceGrid(curLum, 80);
    std::vector<float> rSorted = sampleLuminanceGrid(refLum, 80);
    if (cSorted.empty() || rSorted.empty()) return match;
    std::sort(cSorted.begin(), cSorted.end());
    std::sort(rSorted.begin(), rSorted.end());

    bool fittedAligned = false;
    if (alignedPixels && curLum.size() == refLum.size())
    {
        const int stepY = std::max(1, curLum.rows / 64);
        const int stepX = std::max(1, curLum.cols / 80);
        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(5200);
        ys.reserve(5200);
        for (int y = 0; y < curLum.rows; y += stepY)
        {
            const float* rRow = refLum.ptr<float>(y);
            const float* cRow = curLum.ptr<float>(y);
            for (int x = 0; x < curLum.cols; x += stepX)
            {
                xs.push_back(static_cast<double>(rRow[x]));
                ys.push_back(static_cast<double>(cRow[x]));
            }
        }
        const size_t n = xs.size();
        if (n >= 32)
        {
            std::vector<double> w(n, 1.0);
            std::vector<double> res(n, 0.0);
            double alpha = 1.0, beta = 0.0;
            double mx = 0.0, my = 0.0;
            double corr = 0.0;
            for (int iter = 0; iter < 4; ++iter)
            {
                double sw = 0.0, sx = 0.0, sy = 0.0;
                for (size_t i = 0; i < n; ++i)
                {
                    sw += w[i];
                    sx += w[i] * xs[i];
                    sy += w[i] * ys[i];
                }
                if (sw < 16.0) break;
                mx = sx / sw;
                my = sy / sw;
                double varX = 0.0, varY = 0.0, covXY = 0.0;
                for (size_t i = 0; i < n; ++i)
                {
                    const double dx = xs[i] - mx;
                    const double dy = ys[i] - my;
                    varX += w[i] * dx * dx;
                    varY += w[i] * dy * dy;
                    covXY += w[i] * dx * dy;
                }
                if (varX < 1e-10 || varY < 1e-10) break;
                alpha = covXY / varX;
                beta = my - alpha * mx;
                corr = covXY / std::sqrt(varX * varY);

                for (size_t i = 0; i < n; ++i)
                    res[i] = std::abs(ys[i] - (alpha * xs[i] + beta));
                std::vector<double> tmp = res;
                const size_t mid = n / 2;
                std::nth_element(tmp.begin(), tmp.begin() + static_cast<ptrdiff_t>(mid), tmp.end());
                const double mad = tmp[mid] * 1.4826 + 1e-4;
                const double delta = 1.5 * mad;
                for (size_t i = 0; i < n; ++i)
                    w[i] = (res[i] <= delta) ? 1.0 : (delta / std::max(res[i], 1e-8));
            }
            if (corr >= 0.25 && alpha > 1e-4 && std::isfinite(alpha) && std::isfinite(beta))
            {
                match.gain = std::clamp(1.0 / alpha, 0.1, 15.0);
                match.offset = mx - match.gain * my;
                fittedAligned = true;
            }
        }
    }

    if (!fittedAligned)
        match = foregroundAffineMatch(cSorted, rSorted);

    // Highlight guard for global-only levelling: prevents a patchy cloud darkening part
    // of the frame from boosting clear highlights past the reference white ceiling.
    if (highlightGuard)
    {
        const double cLo = quantileOfSorted(cSorted, 0.01);
        const double cHi = quantileOfSorted(cSorted, 0.99);
        const double rLo = quantileOfSorted(rSorted, 0.01);
        const double rHi = quantileOfSorted(rSorted, 0.99);
        const double rSpan = std::max(1e-4, rHi - rLo);
        const double cSpan = cHi - cLo;
        if (cSpan > 1e-4 && cHi * match.gain + match.offset > rHi + 0.08 * rSpan)
        {
            const double cappedGain = std::clamp((rHi + 0.05 * rSpan - rLo) / cSpan, 0.1, match.gain);
            match.gain = cappedGain;
            match.offset = rLo - cappedGain * cLo;
        }
    }
    return match;
}

} // namespace

double measureTargetBrightness(const FitsImage& image)
{
    if (!image.valid()) return 0.0;
    const cv::Mat m = fitsToMat(image);
    const cv::Mat lum = matLuminance(m);
    double targetMean = 0;
    (void)targetForegroundMask(lum, &targetMean);
    return targetMean;
}

StackBrightnessStats measureStackBrightnessStats(const FitsImage& image, const QRect& crop)
{
    StackBrightnessStats stats;
    if (!image.valid()) return stats;
    const cv::Mat lum = matLuminance(croppedView(fitsToMat(image), crop));
    std::vector<float> samples = sampleLuminanceGrid(lum, 96);
    if (samples.empty()) return stats;
    std::sort(samples.begin(), samples.end());
    stats.p05 = quantileOfSorted(samples, 0.05);
    stats.p98 = quantileOfSorted(samples, 0.98);
    return stats;
}

int selectClearReferenceStack(const std::vector<double>& qualityScores, const std::vector<double>& contrastSpans)
{
    const size_t n = qualityScores.size();
    if (n == 0) return -1;
    if (contrastSpans.size() != n)
    {
        size_t best = 0;
        for (size_t i = 1; i < n; ++i)
            if (qualityScores[i] > qualityScores[best]) best = i;
        return static_cast<int>(best);
    }

    std::vector<double> sortedSpans = contrastSpans;
    std::sort(sortedSpans.begin(), sortedSpans.end());
    const size_t p90Idx = std::min(n - 1, (n - 1) * 90 / 100);
    const double minClearSpan = 0.70 * sortedSpans[p90Idx];

    int best = -1;
    for (size_t i = 0; i < n; ++i)
    {
        if (contrastSpans[i] < minClearSpan) continue;
        if (best < 0 || qualityScores[i] > qualityScores[static_cast<size_t>(best)]
            || (qualityScores[i] == qualityScores[static_cast<size_t>(best)]
                && contrastSpans[i] > contrastSpans[static_cast<size_t>(best)]))
        {
            best = static_cast<int>(i);
        }
    }
    if (best < 0)
    {
        best = 0;
        for (size_t i = 1; i < n; ++i)
            if (qualityScores[i] > qualityScores[static_cast<size_t>(best)]) best = static_cast<int>(i);
    }
    return best;
}

BrightnessMatch computeBrightnessMatch(const FitsImage& image, const FitsImage& reference,
    const QRect& crop, bool alignedPixels)
{
    if (!image.valid() || !reference.valid()) return {};
    const cv::Mat curLum = matLuminance(croppedView(fitsToMat(image), crop));
    const cv::Mat refLum = matLuminance(croppedView(fitsToMat(reference), crop));
    return computeBrightnessMatchMats(curLum, refLum, alignedPixels, /*highlightGuard=*/true);
}

void computeAutoStretchLevels(const FitsImage& image, double& outBlack, double& outWhite, const QRect& crop)
{
    outBlack = 0.0;
    outWhite = 1.0;
    if (!image.valid()) return;
    const cv::Mat lum = matLuminance(croppedView(fitsToMat(image), crop));
    std::vector<float> grid = sampleLuminanceGrid(lum, 96);
    if (grid.empty()) return;
    std::sort(grid.begin(), grid.end());
    outBlack = std::clamp(quantileOfSorted(grid, 0.001), 0.0, 0.99);
    outWhite = std::clamp(quantileOfSorted(grid, 0.999), outBlack + 0.01, 1.0);
}

namespace {

cv::Mat prepareLevelledFitsMat(const FitsImage& image,
    double black, double white, double brightnessScale,
    const QRect& crop,
    const FitsImage* reference,
    BrightnessLevellingMode levelling,
    bool alignedPixels)
{
    cv::Mat m = croppedView(fitsToMat(image), crop).clone();

    if (reference && reference->valid() && levelling != BrightnessLevellingMode::Off)
    {
        if (reference != &image)
        {
            const cv::Mat refMat = croppedView(fitsToMat(*reference), crop);
            const cv::Mat curLum = matLuminance(m);
            const cv::Mat refLum = matLuminance(refMat);
            const bool doPatchy = (levelling == BrightnessLevellingMode::GlobalAndPatchy)
                && alignedPixels && m.size() == refMat.size();
            const BrightnessMatch bm = computeBrightnessMatchMats(curLum, refLum, alignedPixels, /*highlightGuard=*/true);
            m = m * static_cast<float>(bm.gain) + cv::Scalar::all(bm.offset);

            if (doPatchy && m.cols >= 16 && m.rows >= 16)
            {
                const cv::Mat mLum = matLuminance(m);
                const int gw = std::clamp(m.cols / 24, 4, 12);
                const int gh = std::clamp(m.rows / 24, 4, 9);
                cv::Mat mSmall, rSmall;
                cv::resize(mLum, mSmall, cv::Size(gw, gh), 0, 0, cv::INTER_AREA);
                cv::resize(refLum, rSmall, cv::Size(gw, gh), 0, 0, cv::INTER_AREA);
                const float blackF = static_cast<float>(black);
                const float eps = static_cast<float>(0.04 * std::max(0.05, white - black));
                cv::Mat ratioSmall(gh, gw, CV_32FC1);
                for (int gy = 0; gy < gh; ++gy)
                {
                    const float* mRow = mSmall.ptr<float>(gy);
                    const float* rRow = rSmall.ptr<float>(gy);
                    float* outRow = ratioSmall.ptr<float>(gy);
                    for (int gx = 0; gx < gw; ++gx)
                    {
                        const float rAbove = std::max(0.0f, rRow[gx] - blackF) + eps;
                        const float mAbove = std::max(0.0f, mRow[gx] - blackF) + eps;
                        outRow[gx] = std::clamp(rAbove / mAbove, 0.5f, 3.0f);
                    }
                }
                cv::Mat ratio;
                cv::resize(ratioSmall, ratio, m.size(), 0, 0, cv::INTER_LINEAR);
                const float spanF = static_cast<float>(std::max(1e-5, white - black));
                const float kneeF = 0.85f * spanF;
                const float shoulderF = 0.15f * spanF;
                auto applyHighlightSafeRatio = [&](cv::Mat& plane) {
                    for (int y = 0; y < plane.rows; ++y)
                    {
                        float* pRow = plane.ptr<float>(y);
                        const float* rRow = ratio.ptr<float>(y);
                        for (int x = 0; x < plane.cols; ++x)
                        {
                            const float above = std::max(0.0f, pRow[x] - blackF);
                            const float r = rRow[x];
                            float boosted = above * r;
                            // C1 soft shoulder above 85% of span when boosting locally,
                            // so a bright clear rim next to a sharp cloud edge never burns flat white.
                            if (r > 1.02f && boosted > kneeF)
                            {
                                const float excess = boosted - kneeF;
                                boosted = kneeF + shoulderF * (excess / (excess + shoulderF));
                            }
                            pRow[x] = blackF + boosted;
                        }
                    }
                };
                if (m.channels() == 1)
                {
                    applyHighlightSafeRatio(m);
                }
                else
                {
                    std::vector<cv::Mat> ch;
                    cv::split(m, ch);
                    for (cv::Mat& c : ch) applyHighlightSafeRatio(c);
                    cv::merge(ch, m);
                }
            }
        }
    }
    else if (std::abs(brightnessScale - 1.0) > 1e-6)
    {
        m = m * static_cast<float>(std::clamp(brightnessScale, 0.25, 4.0));
    }
    return m;
}

cv::Mat applyStretchAndGamma(const cv::Mat& m, double black, double white, double gamma)
{
    const double safeSpan = std::max(1e-5, white - black);
    const double invGamma = 1.0 / std::clamp(gamma, 0.2, 4.0);
    cv::Mat scaled = (m - cv::Scalar::all(black)) * static_cast<float>(1.0 / safeSpan);
    cv::min(scaled, 1.0f, scaled);
    cv::max(scaled, 0.0f, scaled);
    if (std::abs(invGamma - 1.0) > 1e-3)
        cv::pow(scaled, invGamma, scaled);
    return scaled;
}

} // namespace

QImage toneMapFitsToImage(const FitsImage& image,
    double black, double white, double gamma, double brightnessScale,
    const QRect& crop, int maxLongSide,
    const FitsImage* reference,
    BrightnessLevellingMode levelling,
    bool alignedPixels)
{
    if (!image.valid()) return {};
    cv::Mat m = prepareLevelledFitsMat(image, black, white, brightnessScale, crop, reference, levelling, alignedPixels);

    if (maxLongSide > 0 && std::max(m.cols, m.rows) > maxLongSide)
    {
        const double scale = static_cast<double>(maxLongSide) / std::max(m.cols, m.rows);
        const int nw = std::max(2, static_cast<int>(std::lround(m.cols * scale)));
        const int nh = std::max(2, static_cast<int>(std::lround(m.rows * scale)));
        cv::Mat resized;
        cv::resize(m, resized, cv::Size(nw, nh), 0, 0, cv::INTER_AREA);
        m = resized;
    }

    const cv::Mat scaled = applyStretchAndGamma(m, black, white, gamma);
    cv::Mat u8;
    scaled.convertTo(u8, image.channels == 3 ? CV_8UC3 : CV_8UC1, 255.0);
    const QImage::Format fmt = image.channels == 3 ? QImage::Format_RGB888 : QImage::Format_Grayscale8;
    QImage out(u8.cols, u8.rows, fmt);
    const int rowBytes = u8.cols * u8.channels();
    for (int y = 0; y < u8.rows; ++y)
        std::memcpy(out.scanLine(y), u8.ptr<uint8_t>(y), static_cast<size_t>(rowBytes));
    return out;
}

Frame fitsToSerFrame(const FitsImage& image,
    bool stretch, double black, double white, double gamma,
    const QRect& crop,
    const FitsImage* reference,
    BrightnessLevellingMode levelling,
    bool alignedPixels)
{
    Frame out;
    if (!image.valid()) return out;

    cv::Mat m = prepareLevelledFitsMat(image, black, white, 1.0, crop, reference, levelling, alignedPixels);
    cv::Mat norm;
    if (stretch)
    {
        norm = applyStretchAndGamma(m, black, white, gamma);
    }
    else
    {
        norm = m;
        cv::min(norm, 1.0f, norm);
        cv::max(norm, 0.0f, norm);
    }

    out.width = norm.cols;
    out.height = norm.rows;
    out.bayer = BayerPattern::None;
    const auto epoch = std::chrono::system_clock::time_point{};
    if (image.meta.dateObs != epoch) out.timestamp = image.meta.dateObs;
    else if (image.meta.dateAvg != epoch) out.timestamp = image.meta.dateAvg;
    else out.timestamp = std::chrono::system_clock::now();

    if (image.channels == 3)
    {
        out.format = PixelFormat::RGB888;
        out.significantBits = 8;
        out.alignment = BitAlignment::Low;
        cv::Mat u8;
        norm.convertTo(u8, CV_8UC3, 255.0);
        const size_t rowBytes = static_cast<size_t>(u8.cols) * 3;
        out.data.resize(rowBytes * static_cast<size_t>(u8.rows));
        for (int y = 0; y < u8.rows; ++y)
            std::memcpy(out.data.data() + static_cast<size_t>(y) * rowBytes, u8.ptr<uint8_t>(y), rowBytes);
    }
    else
    {
        out.format = PixelFormat::Mono16;
        out.significantBits = 16;
        out.alignment = BitAlignment::Low;
        cv::Mat u16;
        norm.convertTo(u16, CV_16UC1, 65535.0);
        const size_t rowBytes = static_cast<size_t>(u16.cols) * sizeof(uint16_t);
        out.data.resize(rowBytes * static_cast<size_t>(u16.rows));
        for (int y = 0; y < u16.rows; ++y)
            std::memcpy(out.data.data() + static_cast<size_t>(y) * rowBytes, u16.ptr<uint16_t>(y), rowBytes);
    }
    return out;
}
