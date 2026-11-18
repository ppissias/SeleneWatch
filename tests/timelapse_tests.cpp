#include "timelapse/fits_io.h"
#include "timelapse/stack_processor.h"
#include "timelapse/timelapse_session.h"
#include "timelapse/timelapse_settings.h"
#include "timelapse/video_exporter.h"

#include "memory_budget.h"
#include "recording/ser_writer.h"
#include "session.h"
#include "sources/recording_source.h"
#include "sources/synthetic_frame_source.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <fitsio.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimeZone>
#include <QtTest/QTest>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::chrono::system_clock::time_point utcPoint(int secOffset, int msOffset = 0)
{
    return std::chrono::system_clock::time_point{}
        + std::chrono::seconds{1'790'000'000 + secOffset}
        + std::chrono::milliseconds{msOffset};
}

double sampleBilinear(const std::vector<float>& img, int w, int h, double x, double y)
{
    if (x < 0.0 || y < 0.0 || x >= w - 1 || y >= h - 1) return 0.0;
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0;
    const double fy = y - y0;
    const auto at = [&](int px, int py) -> double {
        return img[static_cast<size_t>(py) * w + px];
    };
    const double top = at(x0, y0) * (1.0 - fx) + at(x0 + 1, y0) * fx;
    const double bot = at(x0, y0 + 1) * (1.0 - fx) + at(x0 + 1, y0 + 1) * fx;
    return top * (1.0 - fy) + bot * fy;
}

std::vector<float> makeLunarCraterField(int w, int h)
{
    std::vector<float> out(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const double dx = (x - w * 0.5) / (w * 0.42);
            const double dy = (y - h * 0.5) / (h * 0.42);
            const double r2 = dx * dx + dy * dy;
            double v = 0.05 + 0.50 * std::exp(-r2 * 1.4);
            // Deterministic textured crater rims across both quadrants.
            const double c1 = std::exp(-(std::pow(x - w * 0.34, 2) + std::pow(y - h * 0.36, 2)) / 65.0);
            const double c2 = std::exp(-(std::pow(x - w * 0.65, 2) + std::pow(y - h * 0.32, 2)) / 90.0);
            const double c3 = std::exp(-(std::pow(x - w * 0.58, 2) + std::pow(y - h * 0.64, 2)) / 50.0);
            const double c4 = std::exp(-(std::pow(x - w * 0.32, 2) + std::pow(y - h * 0.68, 2)) / 75.0);
            const double ripples = 0.08 * std::sin(x * 0.27 + y * 0.13)
                                 + 0.06 * std::cos(x * 0.11 - y * 0.31)
                                 + 0.05 * std::sin((x - y) * 0.41);
            v += 0.18 * (c1 - 0.6 * c2 + 0.8 * c3 - 0.5 * c4) + ripples * std::exp(-r2 * 0.9);
            out[static_cast<size_t>(y) * w + x] = static_cast<float>(std::clamp(v, 0.0, 0.95));
        }
    }
    return out;
}

FramePtr makeMono16Frame(int w, int h, const std::vector<float>& normPixels, uint64_t index,
    std::chrono::system_clock::time_point utc, double scale = 1.0)
{
    auto f = std::make_shared<Frame>();
    f->width = w;
    f->height = h;
    f->format = PixelFormat::Mono16;
    f->significantBits = 16;
    f->alignment = BitAlignment::Low;
    f->frameIndex = index;
    f->timestamp = utc;
    f->testSource = true;
    f->data.resize(frameBytes(w, h, PixelFormat::Mono16));
    uint16_t* dst = reinterpret_cast<uint16_t*>(f->data.data());
    for (size_t i = 0; i < normPixels.size(); ++i)
    {
        const double v = std::clamp(normPixels[i] * scale, 0.0, 1.0);
        dst[i] = static_cast<uint16_t>(std::lround(v * 65535.0));
    }
    return f;
}

FramePtr makeMono8Frame(int w, int h, const std::vector<float>& normPixels, uint64_t index,
    std::chrono::system_clock::time_point utc, double scale = 1.0)
{
    auto f = std::make_shared<Frame>();
    f->width = w;
    f->height = h;
    f->format = PixelFormat::Mono8;
    f->significantBits = 8;
    f->alignment = BitAlignment::Low;
    f->frameIndex = index;
    f->timestamp = utc;
    f->testSource = true;
    f->data.resize(frameBytes(w, h, PixelFormat::Mono8));
    for (size_t i = 0; i < normPixels.size(); ++i)
    {
        const double v = std::clamp(normPixels[i] * scale, 0.0, 1.0);
        f->data[i] = static_cast<uint8_t>(std::lround(v * 255.0));
    }
    return f;
}

std::vector<float> warpFloatField(const std::vector<float>& src, int w, int h,
    double dxForward, double dyForward, double rotDegForward)
{
    // Output pixel (x, y) samples input at inverse transform.
    const double rad = rotDegForward * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    const double cx = w * 0.5;
    const double cy = h * 0.5;

    std::vector<float> dst(static_cast<size_t>(w) * h, 0.0f);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const double ux = (x - dxForward) - cx;
            const double uy = (y - dyForward) - cy;
            const double sx = c * ux + s * uy + cx;
            const double sy = -s * ux + c * uy + cy;
            dst[static_cast<size_t>(y) * w + x] = static_cast<float>(sampleBilinear(src, w, h, sx, sy));
        }
    }
    return dst;
}

std::vector<float> gaussianBlur3x3(const std::vector<float>& src, int w, int h, int passes)
{
    std::vector<float> a = src;
    std::vector<float> b = src;
    for (int p = 0; p < passes; ++p)
    {
        b = a;
        for (int y = 1; y < h - 1; ++y)
        {
            for (int x = 1; x < w - 1; ++x)
            {
                const size_t i = static_cast<size_t>(y) * w + x;
                b[i] = (a[i - w - 1] + 2 * a[i - w] + a[i - w + 1]
                      + 2 * a[i - 1] + 4 * a[i] + 2 * a[i + 1]
                      + a[i + w - 1] + 2 * a[i + w] + a[i + w + 1]) / 16.0f;
            }
        }
        a.swap(b);
    }
    return a;
}

bool checkFileHasMp4Signature(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(32);
    return head.contains("ftyp");
}

bool checkFileHasGifSignature(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(6);
    return head == "GIF89a" || head == "GIF87a";
}

bool decodeWithFfmpeg(const QString& path)
{
    const QString exe = findFfmpegExecutable();
    QProcess proc;
    proc.setProgram(exe);
    proc.setArguments({"-v", "error", "-i", path, "-f", "null", "-"});
    proc.start();
    if (!proc.waitForFinished(15000))
    {
        proc.kill();
        return false;
    }
    return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

QString readTextFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace

class TimelapseTests : public QObject
{
    Q_OBJECT

private slots:
    void fitsRoundTripMonoAndRgbAndExternal16Bit();
    void debayerBayerPatternsAndNormalizedSharpness();
    void twoPassPhaseCorrelationAndSigmaClipping();
    void planetCentroidAndMoonAnchorAlignment();
    void linearSharpeningPreservesLinearity();
    void perPeriodCapAndParallelOverlapWithoutSkips();
    void overloadPreCaptureSkipGateAndMemoryBound();
    void diskReserveStopsAndStillExportsVideo();
    void sessionStartTimeLapseAndResharpenRebuild();
};

void TimelapseTests::fitsRoundTripMonoAndRgbAndExternal16Bit()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    // 1. 2D Mono BITPIX=-32 with complete metadata headers.
    FitsImage mono;
    mono.width = 48;
    mono.height = 36;
    mono.channels = 1;
    mono.pixels.resize(static_cast<size_t>(mono.width) * mono.height);
    for (size_t i = 0; i < mono.pixels.size(); ++i)
        mono.pixels[i] = static_cast<float>(i) / static_cast<float>(mono.pixels.size() - 1);

    mono.meta.dateObs = utcPoint(10, 125);
    mono.meta.dateAvg = utcPoint(17, 625);
    mono.meta.dateEnd = utcPoint(25, 125);
    mono.meta.exptimeSeconds = 0.015;
    mono.meta.gain = 140.0;
    mono.meta.hasGain = true;
    mono.meta.camera = "ASI178MM";
    mono.meta.telescope = "C11";
    mono.meta.observer = "Tester";
    mono.meta.siteName = "Athens";
    mono.meta.targetPreset = "moon";
    mono.meta.framesCaptured = 240;
    mono.meta.framesKept = 150;
    mono.meta.qualityScore = 0.382;
    mono.meta.shiftX = -1.25;
    mono.meta.shiftY = 2.75;
    mono.meta.rotationDeg = 0.625;
    mono.meta.sharpeningProfile = "Mild";
    mono.meta.softwareVersion = "2026.11-01";
    mono.meta.rawMaxAdu = 65535.0;

    const QString monoPath = tmp.filePath("mono.fits");
    QString err;
    QVERIFY2(writeFitsFloat(monoPath, mono, err), qPrintable(err));

    // Verify raw FITS header keys directly with CFITSIO.
    {
        int status = 0;
        fitsfile* fptr = nullptr;
        const QByteArray enc = QFile::encodeName(monoPath);
        QCOMPARE(fits_open_diskfile(&fptr, enc.constData(), READONLY, &status), 0);
        int bitpix = 0, naxis = 0;
        long naxes[3]{};
        QCOMPARE(fits_get_img_param(fptr, 3, &bitpix, &naxis, naxes, &status), 0);
        QCOMPARE(bitpix, FLOAT_IMG);
        QCOMPARE(naxis, 2);
        QCOMPARE(naxes[0], 48L);
        QCOMPARE(naxes[1], 36L);

        char strVal[FLEN_VALUE]{};
        for (const char* key : {"DATE-OBS", "DATE-AVG", "DATE-END", "SHARPPRO"})
        {
            status = 0;
            QCOMPARE(fits_read_key(fptr, TSTRING, key, strVal, nullptr, &status), 0);
            QVERIFY(std::strlen(strVal) > 0);
        }
        double dVal = 0;
        for (const char* key : {"EXPTIME", "GAIN", "QUALSCOR", "ALIGNDX", "ALIGNDY", "ALIGNROT"})
        {
            status = 0;
            QCOMPARE(fits_read_key(fptr, TDOUBLE, key, &dVal, nullptr, &status), 0);
        }
        long long lVal = 0;
        QCOMPARE(fits_read_key(fptr, TLONGLONG, "RAWCOUNT", &lVal, nullptr, &status), 0);
        QCOMPARE(lVal, 240LL);
        QCOMPARE(fits_read_key(fptr, TLONGLONG, "STACKCNT", &lVal, nullptr, &status), 0);
        QCOMPARE(lVal, 150LL);
        fits_close_file(fptr, &status);
    }

    FitsImage monoBack;
    QVERIFY2(readFitsFloat(monoPath, monoBack, err), qPrintable(err));
    QCOMPARE(monoBack.width, 48);
    QCOMPARE(monoBack.height, 36);
    QCOMPARE(monoBack.channels, 1);
    QCOMPARE(monoBack.meta.framesCaptured, 240ULL);
    QCOMPARE(monoBack.meta.framesKept, 150ULL);
    QCOMPARE(monoBack.meta.sharpeningProfile, std::string("Mild"));
    QCOMPARE(monoBack.meta.camera, std::string("ASI178MM"));
    QVERIFY(std::abs(monoBack.meta.exptimeSeconds - 0.015) < 1e-6);
    QVERIFY(monoBack.meta.hasGain);
    QVERIFY(std::abs(monoBack.meta.gain - 140.0) < 1e-6);
    QVERIFY(std::abs(monoBack.meta.shiftX - (-1.25)) < 1e-6);
    QVERIFY(std::abs(monoBack.meta.rotationDeg - 0.625) < 1e-6);
    QVERIFY(monoBack.meta.dateObs == mono.meta.dateObs);
    QVERIFY(monoBack.meta.dateAvg == mono.meta.dateAvg);
    QVERIFY(monoBack.meta.dateEnd == mono.meta.dateEnd);
    for (size_t i = 0; i < mono.pixels.size(); ++i)
        QVERIFY(std::abs(monoBack.pixels[i] - mono.pixels[i]) < 1e-6f);

    // 2. 3D plane-sequential RGB BITPIX=-32.
    FitsImage rgb;
    rgb.width = 40;
    rgb.height = 30;
    rgb.channels = 3;
    const size_t plane = static_cast<size_t>(rgb.width) * rgb.height;
    rgb.pixels.resize(plane * 3);
    std::fill(rgb.pixels.begin(), rgb.pixels.begin() + static_cast<ptrdiff_t>(plane), 0.25f);
    std::fill(rgb.pixels.begin() + static_cast<ptrdiff_t>(plane), rgb.pixels.begin() + static_cast<ptrdiff_t>(2 * plane), 0.50f);
    std::fill(rgb.pixels.begin() + static_cast<ptrdiff_t>(2 * plane), rgb.pixels.end(), 0.75f);

    const QString rgbPath = tmp.filePath("rgb.fits");
    QVERIFY2(writeFitsFloat(rgbPath, rgb, err), qPrintable(err));

    {
        int status = 0;
        fitsfile* fptr = nullptr;
        const QByteArray enc = QFile::encodeName(rgbPath);
        QCOMPARE(fits_open_diskfile(&fptr, enc.constData(), READONLY, &status), 0);
        int bitpix = 0, naxis = 0;
        long naxes[3]{};
        QCOMPARE(fits_get_img_param(fptr, 3, &bitpix, &naxis, naxes, &status), 0);
        QCOMPARE(bitpix, FLOAT_IMG);
        QCOMPARE(naxis, 3);
        QCOMPARE(naxes[2], 3L);
        fits_close_file(fptr, &status);
    }

    FitsImage rgbBack;
    QVERIFY2(readFitsFloat(rgbPath, rgbBack, err), qPrintable(err));
    QCOMPARE(rgbBack.channels, 3);
    QVERIFY(std::abs(rgbBack.pixels[0] - 0.25f) < 1e-6f);
    QVERIFY(std::abs(rgbBack.pixels[plane] - 0.50f) < 1e-6f);
    QVERIFY(std::abs(rgbBack.pixels[2 * plane] - 0.75f) < 1e-6f);

    // 3. External 16-bit unsigned integer FITS (Siril/SharpCap compatibility).
    const QString int16Path = tmp.filePath("ext16.fits");
    {
        int status = 0;
        fitsfile* fptr = nullptr;
        const QByteArray enc = QFile::encodeName(int16Path);
        QCOMPARE(fits_create_diskfile(&fptr, enc.constData(), &status), 0);
        long naxes[2]{16, 16};
        QCOMPARE(fits_create_img(fptr, USHORT_IMG, 2, naxes, &status), 0);
        std::vector<uint16_t> vals(256, 32768);
        vals[0] = 0;
        vals[255] = 65535;
        QCOMPARE(fits_write_img(fptr, TUSHORT, 1, 256, vals.data(), &status), 0);
        QCOMPARE(fits_close_file(fptr, &status), 0);
    }
    FitsImage intBack;
    QVERIFY2(readFitsFloat(int16Path, intBack, err), qPrintable(err));
    QCOMPARE(intBack.width, 16);
    QCOMPARE(intBack.height, 16);
    QVERIFY(std::abs(intBack.pixels[0] - 0.0f) < 1e-4f);
    QVERIFY(std::abs(intBack.pixels[10] - (32768.0f / 65535.0f)) < 1e-3f);
    QVERIFY(std::abs(intBack.pixels[255] - 1.0f) < 1e-4f);
}

void TimelapseTests::debayerBayerPatternsAndNormalizedSharpness()
{
    auto makeBayerFrame = [](BayerPattern pat, uint8_t r, uint8_t g, uint8_t b) {
        auto f = std::make_shared<Frame>();
        f->width = 32;
        f->height = 32;
        f->format = PixelFormat::Mono8;
        f->bayer = pat;
        f->significantBits = 8;
        f->testSource = true;
        f->data.resize(32 * 32, g);
        for (int y = 0; y < 32; ++y)
        {
            for (int x = 0; x < 32; ++x)
            {
                const bool evenY = (y % 2) == 0;
                const bool evenX = (x % 2) == 0;
                uint8_t val = g;
                if (pat == BayerPattern::RGGB)
                    val = (evenY && evenX) ? r : ((!evenY && !evenX) ? b : g);
                else if (pat == BayerPattern::BGGR)
                    val = (evenY && evenX) ? b : ((!evenY && !evenX) ? r : g);
                f->data[static_cast<size_t>(y) * 32 + x] = val;
            }
        }
        return f;
    };

    const auto rggb = prepareBurstFrame(*makeBayerFrame(BayerPattern::RGGB, 220, 20, 45));
    QCOMPARE(rggb.channels, 3);
    const size_t plane = 32 * 32;
    const size_t mid = 16 * 32 + 16;
    QVERIFY(rggb.pixels[mid] > 0.7f);              // R
    QVERIFY(rggb.pixels[plane + mid] < 0.2f);      // G
    QVERIFY(rggb.pixels[2 * plane + mid] < 0.3f);  // B

    const auto bggr = prepareBurstFrame(*makeBayerFrame(BayerPattern::BGGR, 40, 20, 220));
    QCOMPARE(bggr.channels, 3);
    QVERIFY(bggr.pixels[mid] < 0.3f);              // R
    QVERIFY(bggr.pixels[plane + mid] < 0.2f);      // G
    QVERIFY(bggr.pixels[2 * plane + mid] > 0.7f);  // B

    // Brightness-normalized sharpness should stay consistent under thin cloud dimming
    // while cleanly rejecting genuinely blurred seeing.
    const auto field = makeLunarCraterField(128, 128);
    const auto blurred = gaussianBlur3x3(field, 128, 128, 8);
    const auto sharpRef = prepareBurstFrame(*makeMono16Frame(128, 128, field, 1, utcPoint(0), 1.0));
    const auto sharpDim = prepareBurstFrame(*makeMono16Frame(128, 128, field, 2, utcPoint(0), 0.5));
    const auto blurRef = prepareBurstFrame(*makeMono16Frame(128, 128, blurred, 3, utcPoint(0), 1.0));

    QVERIFY(sharpRef.sharpness > 0.0);
    const double dimRatio = sharpDim.sharpness / sharpRef.sharpness;
    const double blurRatio = blurRef.sharpness / sharpRef.sharpness;
    QVERIFY2(dimRatio > 0.85 && dimRatio < 1.15, qPrintable(QString("dimRatio=%1").arg(dimRatio)));
    QVERIFY2(blurRatio < 0.65, qPrintable(QString("blurRatio=%1").arg(blurRatio)));
}

void TimelapseTests::twoPassPhaseCorrelationAndSigmaClipping()
{
    const int w = 128, h = 128;
    const auto refField = makeLunarCraterField(w, h);
    std::vector<PreparedBurstFrame> kept;
    const std::vector<std::pair<double, double>> shifts{
        {0.0, 0.0}, {1.25, -0.75}, {-2.1, 1.4}, {0.8, 2.2}, {-1.6, -1.8},
        {2.4, 0.5}, {-0.9, 1.9}, {1.7, -2.1}, {-2.3, -0.4}, {0.4, -1.1}
    };

    const double refValAtCenter = refField[64 * w + 64];

    for (size_t i = 0; i < shifts.size(); ++i)
    {
        auto warped = warpFloatField(refField, w, h, shifts[i].first, shifts[i].second, 0.0);
        // Inject a strong transient outlier at aligned center coordinate in frame 4 ONLY.
        if (i == 4)
        {
            // Single-pixel cosmic-ray / hot-pixel transient at aligned center in frame 4 only.
            const int hx = static_cast<int>(std::lround(64.0 + shifts[i].first));
            const int hy = static_cast<int>(std::lround(64.0 + shifts[i].second));
            warped[static_cast<size_t>(hy) * w + hx] = 1.0f;
        }
        auto f = makeMono16Frame(w, h, warped, i, utcPoint(static_cast<int>(i)));
        kept.push_back(prepareBurstFrame(*f));
    }

    const BurstStackResult stacked = stackBurst(kept);
    QVERIFY2(stacked.ok, qPrintable(stacked.error));
    QVERIFY(stacked.stack.valid());
    QVERIFY2(stacked.validOverlap.width() <= w - 3,
        qPrintable(QString("overlap=%1,%2 %3x%4")
            .arg(stacked.validOverlap.x()).arg(stacked.validOverlap.y())
            .arg(stacked.validOverlap.width()).arg(stacked.validOverlap.height())));
    QVERIFY2(stacked.validOverlap.height() <= h - 3,
        qPrintable(QString("overlap=%1,%2 %3x%4").arg(stacked.validOverlap.x()).arg(stacked.validOverlap.y())
            .arg(stacked.validOverlap.width()).arg(stacked.validOverlap.height())));

    // Align stacked output back onto refField to verify alignment accuracy AND that
    // the single-frame hot pixel spike at center was clipped out (not +0.10 or +0.05).
    FitsImage refFits;
    refFits.width = w; refFits.height = h; refFits.channels = 1;
    refFits.pixels = refField;
    const SessionAlignResult alignedBack = alignStackToReference(stacked.stack, refFits, TimelapsePreset::Moon);
    const float stackedCenter = alignedBack.aligned.pixels[64 * w + 64];
    QVERIFY2(std::abs(stackedCenter - refValAtCenter) < 0.03,
        qPrintable(QString("alignedCenter=%1 ref=%2 shift=(%3,%4) rawCenter=%5")
            .arg(stackedCenter).arg(refValAtCenter)
            .arg(alignedBack.shiftX).arg(alignedBack.shiftY)
            .arg(stacked.stack.pixels[64 * w + 64])));
}

void TimelapseTests::planetCentroidAndMoonAnchorAlignment()
{
    // 1. Planet centroid alignment: rotating surface bands shifted by (+14.5, -9.25).
    {
        const int w = 160, h = 160;
        auto makePlanet = [&](double cx, double cy, double phaseDeg) {
            FitsImage img;
            img.width = w; img.height = h; img.channels = 1;
            img.pixels.resize(static_cast<size_t>(w) * h, 0.0f);
            const double rad = phaseDeg * 3.14159265358979323846 / 180.0;
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    const double dx = x - cx;
                    const double dy = y - cy;
                    const double r = std::hypot(dx, dy);
                    const double disc = 1.0 / (1.0 + std::exp((r - 22.0) / 1.8));
                    const double rx = dx * std::cos(rad) - dy * std::sin(rad);
                    const double spot = 0.25 * std::exp(-((rx - 8.0) * (rx - 8.0) + dy * dy) / 28.0);
                    const double bands = 0.72 + 0.14 * std::sin(dy * 0.32) - spot;
                    img.pixels[static_cast<size_t>(y) * w + x] = static_cast<float>(std::clamp(disc * bands, 0.0, 1.0));
                }
            }
            return img;
        };

        const FitsImage ref = makePlanet(80.0, 80.0, 0.0);
        const FitsImage mov = makePlanet(94.5, 70.75, 42.0);
        const SessionAlignResult res = alignStackToReference(mov, ref, TimelapsePreset::Planet);
        QVERIFY(res.aligned.valid());
        QVERIFY2(std::abs(res.shiftX - (-14.5)) < 0.6 && std::abs(res.shiftY - 9.25) < 0.6,
            qPrintable(QString("planet shift=(%1, %2)").arg(res.shiftX).arg(res.shiftY)));
    }

    // 2. Moon translation + rigid rotation alignment with steady sunlit lunarAnchor
    // while unanchored quadrant undergoes severe simulated terminator shadow drift.
    {
        const int w = 256, h = 256;
        const auto refField = makeLunarCraterField(w, h);
        FitsImage ref;
        ref.width = w; ref.height = h; ref.channels = 1;
        ref.pixels = refField;

        auto movField = warpFloatField(refField, w, h, 8.0, -6.0, 3.5);
        // Deepen simulated shadow across the bottom-left area outside the anchor.
        for (int y = 135; y < h; ++y)
            for (int x = 0; x < 120; ++x)
                movField[static_cast<size_t>(y) * w + x] *= 0.18f;

        FitsImage mov;
        mov.width = w; mov.height = h; mov.channels = 1;
        mov.pixels = std::move(movField);

        const QRect sunlitAnchor(128, 32, 96, 96);
        const SessionAlignResult res = alignStackToReference(mov, ref, TimelapsePreset::Moon, sunlitAnchor);
    QVERIFY(!res.usedFallback);
    QVERIFY2(std::abs(res.rotationDeg - (-3.5)) < 0.6,
        qPrintable(QString("recovered rotDeg=%1 shift=(%2,%3)")
            .arg(res.rotationDeg).arg(res.shiftX).arg(res.shiftY)));

    double sqErr = 0.0;
    int n = 0;
    for (int y = sunlitAnchor.top() + 8; y < sunlitAnchor.bottom() - 8; ++y)
    {
        for (int x = sunlitAnchor.left() + 8; x < sunlitAnchor.right() - 8; ++x)
        {
            const double d = res.aligned.pixels[static_cast<size_t>(y) * w + x]
                           - ref.pixels[static_cast<size_t>(y) * w + x];
            sqErr += d * d;
            ++n;
        }
    }
    const double rms = std::sqrt(sqErr / std::max(1, n));
    QVERIFY2(rms < 0.025, qPrintable(QString("anchored moon rms=%1 rot=%2 shift=(%3,%4)")
        .arg(rms).arg(res.rotationDeg).arg(res.shiftX).arg(res.shiftY)));
    }
}

void TimelapseTests::linearSharpeningPreservesLinearity()
{
    FitsImage img;
    img.width = 96;
    img.height = 96;
    img.channels = 1;
    img.pixels.resize(96 * 96, 0.35f);
    // Crisp high-contrast crater circle in right half while left half stays flat 0.35.
    for (int y = 24; y < 72; ++y)
        for (int x = 56; x < 84; ++x)
            if ((x - 70) * (x - 70) + (y - 48) * (y - 48) < 100)
                img.pixels[static_cast<size_t>(y) * 96 + x] = 0.75f;

    const FitsImage sharp = applyLinearSharpening(img, sharpeningForPreset(SharpeningPreset::Medium));
    QCOMPARE(sharp.meta.sharpeningProfile, std::string("Medium"));
    // Uniform linear patch stays strictly at 0.35 (no gamma/stretch applied!).
    for (int y = 20; y < 76; ++y)
        for (int x = 10; x < 35; ++x)
            QVERIFY(std::abs(sharp.pixels[static_cast<size_t>(y) * 96 + x] - 0.35f) < 1e-4f);

    // Edge gradient around crater boundary increases.
    double gradOrig = 0, gradSharp = 0;
    for (int y = 36; y <= 60; ++y)
    {
        for (int x = 56; x <= 84; ++x)
        {
            gradOrig += std::abs(img.pixels[static_cast<size_t>(y) * 96 + x + 1] - img.pixels[static_cast<size_t>(y) * 96 + x]);
            gradSharp += std::abs(sharp.pixels[static_cast<size_t>(y) * 96 + x + 1] - sharp.pixels[static_cast<size_t>(y) * 96 + x]);
        }
    }
    QVERIFY(gradSharp > gradOrig * 1.25);
}

void TimelapseTests::perPeriodCapAndParallelOverlapWithoutSkips()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Planet);
    s.intervalSeconds = 1.0;
    s.captureWindowSeconds = 0.6;
    s.keepPercent = 100.0;
    s.maxFramesPerPeriod = 250; // Must fail valid() until clamped to <= 200.
    QVERIFY(!s.valid());
    s.maxFramesPerPeriod = TimelapseSettings::kMaxFramesCap; // 200
    s.video.writeMp4 = false;
    s.video.writeGif = false;
    QVERIFY(s.valid());

    QObject ctx;
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = tmp.filePath("timelapses");
    cfg.settings = s;
    cfg.stream.playback = "synthetic_cap.ser";
    cfg.stream.camera = "TestCam";
    cfg.memory = MemoryBudget(128ULL * 1024 * 1024);

    TimelapseSession session(&ctx, cfg);
    // Worker takes 300ms on job 0 while period 1 enters the depth-1 pending slot in parallel.
    // Once job 0 finishes and job 1 pops into worker, period 2 enters cleanly with zero skips.
    session.setStackDelay(std::chrono::milliseconds{300});

    const auto field = makeLunarCraterField(48, 48);
    // Period 0: 240 frames (cap must bound kept frames to 200!).
    for (int i = 0; i < 240; ++i)
    {
        const int ms = (i * 580) / 240;
        session.onFrame(makeMono8Frame(48, 48, field, static_cast<uint64_t>(i), utcPoint(0, ms)));
    }
    session.onFrame(makeMono8Frame(48, 48, field, 240, utcPoint(0, 620)));
    QThread::msleep(80); // Let worker pop job 0 and sleep in processBurst.

    // Period 1: enters pendingJob_ while job 0 runs in parallel.
    for (int i = 0; i < 20; ++i)
        session.onFrame(makeMono8Frame(48, 48, field, static_cast<uint64_t>(300 + i), utcPoint(1, i * 25)));
    session.onFrame(makeMono8Frame(48, 48, field, 320, utcPoint(1, 620)));

    // Wait deterministically until job 0 completes and worker pops job 1.
    session.setStackDelay(std::chrono::milliseconds{0});
    QTRY_VERIFY_WITH_TIMEOUT(session.status().stacksCompleted >= 1, 5000);
    QThread::msleep(20);

    // Period 2:
    for (int i = 0; i < 20; ++i)
        session.onFrame(makeMono8Frame(48, 48, field, static_cast<uint64_t>(400 + i), utcPoint(2, i * 25)));
    session.onFrame(makeMono8Frame(48, 48, field, 420, utcPoint(2, 620)));

    session.stop("test done");
    session.waitFinished();

    const TimelapseStatus st = session.status();
    QCOMPARE(st.periodsSkipped, 0);
    QCOMPARE(st.stacksCompleted, 3);
    QCOMPARE(cfg.memory.usage().used, size_t{0});

    FitsImage firstStack;
    QString err;
    const QDir stacksDir(QDir(st.sessionDir).filePath("stacks"));
    const QStringList fitsFiles = stacksDir.entryList({"*.fits"}, QDir::Files, QDir::Name);
    QCOMPARE(fitsFiles.size(), 3);
    QVERIFY2(readFitsFloat(stacksDir.filePath(fitsFiles.first()), firstStack, err), qPrintable(err));
    QCOMPARE(firstStack.meta.framesCaptured, 240ULL);
    QCOMPARE(firstStack.meta.framesKept, 200ULL);
}

void TimelapseTests::overloadPreCaptureSkipGateAndMemoryBound()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
    s.intervalSeconds = 1.0;
    s.captureWindowSeconds = 0.5;
    s.keepPercent = 50.0;
    s.maxFramesPerPeriod = 100;
    s.video.writeMp4 = false;
    s.video.writeGif = false;

    const size_t budgetLimit = 40ULL * 1024 * 1024;
    QObject ctx;
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = tmp.filePath("timelapses");
    cfg.settings = s;
    cfg.stream.playback = "overload.ser";
    cfg.stream.camera = "TestCam";
    cfg.memory = MemoryBudget(budgetLimit);

    TimelapseSession session(&ctx, cfg);
    session.setStackDelay(std::chrono::milliseconds{400});

    const auto field = makeLunarCraterField(64, 64);

    // Period 0 -> worker pops it and sleeps 400ms.
    for (int i = 0; i < 8; ++i)
        session.onFrame(makeMono8Frame(64, 64, field, static_cast<uint64_t>(i), utcPoint(0, i * 50)));
    session.onFrame(makeMono8Frame(64, 64, field, 8, utcPoint(0, 520)));
    QThread::msleep(40);

    // Period 1 -> enters pendingJob_ (depth-1 queue full while worker still busy on job 0).
    for (int i = 0; i < 8; ++i)
        session.onFrame(makeMono8Frame(64, 64, field, static_cast<uint64_t>(20 + i), utcPoint(1, i * 50)));
    session.onFrame(makeMono8Frame(64, 64, field, 28, utcPoint(1, 520)));

    // Period 2 starts immediately -> pre-capture skip gate must skip period 2 cleanly!
    session.onFrame(makeMono8Frame(64, 64, field, 40, utcPoint(2, 10)));
    QVERIFY(cfg.memory.usage().used <= budgetLimit);

    session.stop("overload test complete");
    session.waitFinished();

    const TimelapseStatus st = session.status();
    QVERIFY(st.periodsSkipped >= 1);
    QVERIFY(st.overloadWarning);
    QCOMPARE(st.stacksCompleted, 2);
    QCOMPARE(cfg.memory.usage().used, size_t{0});

    const QString logText = readTextFile(QDir(st.sessionDir).filePath("timelapse.txt"));
    QVERIFY(logText.contains("skip period=2"));
}

void TimelapseTests::diskReserveStopsAndStillExportsVideo()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    std::atomic<int> spaceQueries{0};
    TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Planet);
    s.intervalSeconds = 1.0;
    s.captureWindowSeconds = 0.5;
    s.keepPercent = 100.0;
    s.maxFramesPerPeriod = 100;
    s.video.writeMp4 = true;
    s.video.writeGif = false;
    s.video.fps = 5;

    QObject ctx;
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = tmp.filePath("timelapses");
    cfg.settings = s;
    cfg.stream.playback = "disk.ser";
    cfg.stream.camera = "TestCam";
    cfg.diskReserveBytes = 2ULL * 1024 * 1024 * 1024;
    cfg.freeSpace = [&](const QString&) -> std::optional<uint64_t> {
        const int c = ++spaceQueries;
        return c == 1 ? 10ULL * 1024 * 1024 * 1024 : 200ULL * 1024 * 1024;
    };

    TimelapseSession session(&ctx, cfg);
    const auto field = makeLunarCraterField(64, 64);

    // Period 0 (space check #1 = 10 GiB -> written).
    for (int i = 0; i < 6; ++i)
        session.onFrame(makeMono8Frame(64, 64, field, static_cast<uint64_t>(i), utcPoint(0, i * 60)));
    session.onFrame(makeMono8Frame(64, 64, field, 6, utcPoint(0, 550)));
    QThread::msleep(60);

    // Period 1 (space check #2 = 200 MiB < 2 GiB reserve -> stops and assembles MP4!).
    for (int i = 0; i < 6; ++i)
        session.onFrame(makeMono8Frame(64, 64, field, static_cast<uint64_t>(20 + i), utcPoint(1, i * 60)));
    session.onFrame(makeMono8Frame(64, 64, field, 26, utcPoint(1, 550)));

    session.waitFinished();
    const TimelapseStatus st = session.status();
    QVERIFY(st.diskLowWarning);
    QCOMPARE(st.stacksCompleted, 1);
    QVERIFY(st.stopReason.contains("disk reserve"));

    const QString mp4 = QDir(st.sessionDir).filePath("timelapse.mp4");
    QVERIFY(QFileInfo::exists(mp4));
    QVERIFY(QFileInfo(mp4).size() > 500);
    QVERIFY(checkFileHasMp4Signature(mp4));
    QVERIFY(decodeWithFfmpeg(mp4));
}

void TimelapseTests::sessionStartTimeLapseAndResharpenRebuild()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString timelapsesRoot = tmp.filePath("timelapses");
    QObject ctx;
    QString failedMsg;
    bool endedFired = false;
    Session::Notifications notices;
    notices.failed = [&](const QString& r) { failedMsg = r; };
    notices.ended = [&] { endedFired = true; };
    Session session(&ctx, tmp.filePath("output"), std::move(notices));

    auto source = std::make_unique<SyntheticFrameSource>();
    session.setSource(std::move(source));

    QString err;
    QVERIFY2(session.start(err, Session::Mode::Preview), qPrintable(err));

    TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
    s.intervalSeconds = 0.45;
    s.captureWindowSeconds = 0.25;
    s.keepPercent = 100.0;
    s.maxFramesPerPeriod = 150;
    s.stopKind = StopConditionKind::StackCount;
    s.targetStacks = 2;
    s.saveRawSer = false;
    s.video.writeMp4 = true;
    s.video.writeGif = true;
    s.video.fps = 4;

    QVERIFY2(session.startTimeLapse(s, err), qPrintable(err));
    QCOMPARE(session.mode(), Session::Mode::TimeLapse);

    QString observedSessionDir;
    QTRY_VERIFY_WITH_TIMEOUT([&] {
        const auto cur = session.timeLapseStatus();
        if (!cur.sessionDir.isEmpty()) observedSessionDir = cur.sessionDir;
        return !cur.active && session.mode() != Session::Mode::TimeLapse;
    }(), 20000);
    QVERIFY(!endedFired);

    const QString tlRoot = tmp.filePath("output/timelapses");
    const QStringList tlEntries = QDir(tlRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QVERIFY2(tlEntries.size() == 1, qPrintable(QString("tl=[%1] fail='%2'").arg(tlEntries.join(","), failedMsg)));
    const QString sessionDir = !observedSessionDir.isEmpty() ? observedSessionDir : QDir(tlRoot).filePath(tlEntries.first());

    const QDir sDir(sessionDir);
    QVERIFY(QFileInfo::exists(sDir.filePath("timelapse.txt")));
    const QString tlLog = readTextFile(sDir.filePath("timelapse.txt"));
    QVERIFY2(tlLog.contains("stacks_completed: 2"), qPrintable(tlLog));
    QVERIFY2(tlLog.contains("periods_skipped: 0"), qPrintable(tlLog));
    QVERIFY(QFileInfo::exists(sDir.filePath("sharpening_profile.json")));
    QCOMPARE(QDir(sDir.filePath("stacks")).entryList({"*.fits"}, QDir::Files).size(), 2);
    QCOMPARE(QDir(sDir.filePath("sharpened")).entryList({"*.fits"}, QDir::Files).size(), 2);

    const QString mp4 = sDir.filePath("timelapse.mp4");
    const QString gif = sDir.filePath("timelapse.gif");
    QVERIFY(QFileInfo::exists(mp4) && QFileInfo(mp4).size() > 500);
    QVERIFY(QFileInfo::exists(gif) && QFileInfo(gif).size() > 500);
    QVERIFY(checkFileHasMp4Signature(mp4));
    QVERIFY(checkFileHasGifSignature(gif));
    QVERIFY(decodeWithFfmpeg(mp4));

    const QDateTime origMp4Mtime = QFileInfo(mp4).lastModified();

    // Non-destructive Re-sharpen & Rebuild into sharpened_<profile>_<UTC>/.
    SharpeningSettings sharpCustom = sharpeningForPreset(SharpeningPreset::Medium);
    sharpCustom.profileName = "Lunar Crisp";
    sharpCustom.amount = 1.6;
    VideoToneSettings toneCustom;
    toneCustom.fps = 6;
    toneCustom.gamma = 1.2;
    toneCustom.writeMp4 = true;
    toneCustom.writeGif = false;

    QString outDir1;
    const TimelapseExportResult r1 = resharpenTimelapseSession(
        sessionDir, {}, sharpCustom, toneCustom, /*rebuildVideoOnly=*/false, outDir1);
    QVERIFY2(r1.ok, qPrintable(r1.error));
    QVERIFY(outDir1.contains("sharpened_Lunar_Crisp_"));
    QVERIFY(QFileInfo::exists(QDir(outDir1).filePath("timelapse.mp4")));
    QVERIFY(!QFileInfo::exists(QDir(outDir1).filePath("timelapse.gif")));
    QCOMPARE(QDir(QDir(outDir1).filePath("sharpened")).entryList({"*.fits"}, QDir::Files).size(), 2);
    // Original session video remains untouched.
    QCOMPARE(QFileInfo(mp4).lastModified(), origMp4Mtime);

    FitsImage reSharpFits;
    const QStringList reFitsFiles = QDir(QDir(outDir1).filePath("sharpened")).entryList({"*.fits"}, QDir::Files, QDir::Name);
    QVERIFY2(readFitsFloat(QDir(outDir1).filePath("sharpened/" + reFitsFiles.first()), reSharpFits, err), qPrintable(err));
    QCOMPARE(reSharpFits.meta.sharpeningProfile, std::string("Lunar Crisp"));

    // Fast Rebuild video only from already-sharpened folder: must not rewrite FITS.
    const QString reFitsPath = QDir(outDir1).filePath("sharpened/" + reFitsFiles.first());
    const QDateTime reFitsMtime = QFileInfo(reFitsPath).lastModified();
    QThread::msleep(20);
    VideoToneSettings toneGif = toneCustom;
    toneGif.gamma = 1.4;
    toneGif.writeMp4 = true;
    toneGif.writeGif = true;
    QString outDir2;
    const TimelapseExportResult r2 = resharpenTimelapseSession(
        sessionDir, QDir(outDir1).filePath("sharpened"), sharpCustom, toneGif, /*rebuildVideoOnly=*/true, outDir2);
    QVERIFY2(r2.ok, qPrintable(r2.error));
    QCOMPARE(QDir::cleanPath(outDir2), QDir::cleanPath(outDir1));
    QCOMPARE(QFileInfo(reFitsPath).lastModified(), reFitsMtime);
    QVERIFY(checkFileHasGifSignature(QDir(outDir1).filePath("timelapse.gif")));
    QVERIFY(decodeWithFfmpeg(QDir(outDir1).filePath("timelapse.mp4")));

    session.stop();
}

QTEST_MAIN(TimelapseTests)
#include "timelapse_tests.moc"
