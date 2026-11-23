#include "timelapse/fits_io.h"
#include "timelapse/sky_timelapse.h"
#include "timelapse/stack_processor.h"
#include "timelapse/timelapse_session.h"
#include "timelapse/timelapse_settings.h"
#include "timelapse/video_exporter.h"
#include "recording/ffmpeg_video.h"

#include "cameras/zwo_source.h"
#include "core/memory_budget.h"
#include "recording/ser_writer.h"
#include "session/session.h"
#include "sources/recording_source.h"
#include "sources/synthetic_frame_source.h"
#include "fake_zwo_api.h"

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
#include <QtCore/QRegularExpression>
#include <QtCore/QThread>
#include <QtGui/QColor>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimeZone>
#include <QtGui/QColor>
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
    void aTransitCanUseUpOneBurstGraceWithoutMovingTheCadence();
    void diskReserveStopsAndStillExportsVideo();
    void sessionStartTimeLapseAndResharpenRebuild();
    void unicodeFitsPathsAndHeaderOnlyMetadata();
    void stopMidWindowPreservesBurstAndConcurrentStop();
    void exposureUnitsAndFailedFitsWriteHandling();
    void rawSerRetainsOnlyKeptFramesAndWorkerDebayers();
    void rotatedMoonInscribedOverlapHasNoBlackCorners();
    void guidingRecoveryCloudBaselineAndMissedWindows();
    void fitsBitpixStrictValidationAndConsistentScaling();
    void bayerGrbgGbrgAnd12BitHighAlignedStacking();
    void multiStackMoonRotationAndPlanetDriftEndToEnd();
    void fakeZwoCameraTimeLapseAndNonBlockingStop();
    void resharpenPreviewMatchesExportedFrame();
    void cloudBrightnessEqualizationAndClearReferenceSelection();
    void realArchiveRecordingsIfPresent();
    void rawSerCopiesFramesWithinRamHeadroom();
    void stackCountTargetDiscardsQueuedBurst();
    void stackingErrorsSkipOrStopTheSessionOnly();
    void cancelledExportKeepsStacksAndSaysHowToRebuild();
    void eventPreviewGifStopsFfmpegAtTimeLimit();
    void skyTimelapseKeepsAFrameEveryIntervalAndItsVideo();
    void skyTimelapseStopsAtTheDiskReserve();
    void anUnclosedVideoStillPlays();
};

namespace {

// Feature-36: a 12-bit frame in 16-bit words whose samples are all different,
// at a second of source time per index.
Frame skyFrame(int index)
{
    Frame frame;
    frame.width = 64;
    frame.height = 48;
    frame.format = PixelFormat::Mono16;
    frame.significantBits = 12;
    frame.frameIndex = static_cast<uint64_t>(index);
    frame.timestamp = std::chrono::system_clock::time_point{} + std::chrono::seconds{1'790'000'000 + index};
    frame.data.resize(static_cast<size_t>(frame.width) * frame.height * 2);
    for (int i = 0; i < frame.width * frame.height; ++i)
    {
        const uint16_t value = static_cast<uint16_t>((200 + index * 3 + i) % 4096);
        frame.data[2 * static_cast<size_t>(i)] = static_cast<uint8_t>(value & 255);
        frame.data[2 * static_cast<size_t>(i) + 1] = static_cast<uint8_t>(value >> 8);
    }
    return frame;
}

} // namespace

// Feature-36: one frame every 30 s of source time, its samples unchanged in a
// 16-bit PNG, its time in frames.txt, and a video that decodes; a recording
// that loops starts the cadence again.
void TimelapseTests::skyTimelapseKeepsAFrameEveryIntervalAndItsVideo()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    std::optional<SkyTimelapse::Status> ended;
    SkyTimelapse timelapse([&ended](const SkyTimelapse::Status& status) { ended = status; });
    timelapse.start(tmp.path(), 30, 0);
    for (int i = 0; i < 200; ++i)
    {
        timelapse.offer(skyFrame(i));
        timelapse.waitIdle();
    }
    QCOMPARE(timelapse.status().frames, 7); // 0, 30, ... 180 s.
    timelapse.offer(skyFrame(0));           // The recording looped.
    timelapse.waitIdle();
    QCOMPARE(timelapse.status().frames, 8);
    timelapse.finish("monitoring stopped");
    timelapse.waitIdle();
    QVERIFY(ended && !ended->active);
    QCOMPARE(ended->frames, 8);
    QCOMPARE(ended->endReason, QString("monitoring stopped"));
    QVERIFY2(ended->problem.isEmpty(), qPrintable(ended->problem));

    const QDir folder(QDir(tmp.path()).filePath("timelapse"));
    // The third frame kept is the one at 60 s, its words unchanged.
    QImage third(folder.filePath("frame_00003.png"));
    QCOMPARE(third.format(), QImage::Format_Grayscale16);
    QCOMPARE(third.size(), QSize(64, 48));
    const Frame expected = skyFrame(60);
    for (int y = 0; y < 48; y += 7)
        for (int x = 0; x < 64; x += 5)
        {
            const size_t i = static_cast<size_t>(y) * 64 + x;
            const uint16_t word = static_cast<uint16_t>(expected.data[2 * i] | (expected.data[2 * i + 1] << 8));
            QCOMPARE(reinterpret_cast<const uint16_t*>(third.constScanLine(y))[x], word);
        }
    QFile frames(folder.filePath("frames.txt"));
    QVERIFY(frames.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines = QString::fromUtf8(frames.readAll()).split('\n', Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 10); // Two header lines, eight frames.
    QVERIFY2(lines[1].contains("format=Mono16 significant_bits=12"), qPrintable(lines[1]));
    QVERIFY2(lines[4].startsWith("frame_00003.png utc=") && lines[4].endsWith(" frame=60"), qPrintable(lines[4]));
    QVERIFY(decodeWithFfmpeg(folder.filePath("timelapse.mp4")));
    QFile record(folder.filePath("timelapse.txt"));
    QVERIFY(record.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString text = QString::fromUtf8(record.readAll());
    QVERIFY2(text.contains("frames: 8") && text.contains("end_reason: monitoring stopped") && text.contains("interval_s: 30"),
        qPrintable(text));
    // A second finish changes nothing; a new start begins a new time-lapse.
    timelapse.finish("again");
    timelapse.waitIdle();
    QCOMPARE(ended->endReason, QString("monitoring stopped"));
}

void TimelapseTests::skyTimelapseStopsAtTheDiskReserve()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    SkyTimelapse timelapse;
    timelapse.start(tmp.path(), 1, std::numeric_limits<uint64_t>::max() / 4);
    timelapse.offer(skyFrame(0));
    timelapse.waitIdle();
    timelapse.offer(skyFrame(5));
    timelapse.waitIdle();
    const SkyTimelapse::Status status = timelapse.status();
    QCOMPARE(status.frames, 0);
    QVERIFY2(status.problem.contains("disk reserve"), qPrintable(status.problem));
    timelapse.finish("monitoring stopped");
    timelapse.waitIdle();
    QVERIFY(QFileInfo::exists(QDir(tmp.path()).filePath("timelapse/timelapse.txt")));
}

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

void TimelapseTests::aTransitCanUseUpOneBurstGraceWithoutMovingTheCadence()
{
    QTemporaryDir tmp;
    QObject context;
    TimelapseSession::Config config;
    config.timelapsesRoot = tmp.path();
    config.stream.playback = "controlled test";
    config.settings.watchTransits = true;
    config.settings.intervalSeconds = 10;
    config.settings.captureWindowSeconds = 1;
    config.settings.video.writeMp4 = false;
    TimelapseSession session(&context, std::move(config));
    QVERIFY(session.valid());
    uint64_t index = 0;
    const auto field = makeLunarCraterField(48, 48);
    const auto offer = [&](int seconds, int milliseconds = 0, bool busy = false) {
        return session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(seconds, milliseconds)), busy);
    };
    QCOMPARE(offer(0), TimelapseWork::Capturing);
    offer(0, 100);
    offer(1);
    QTRY_COMPARE(session.status().stacksCompleted, 1);
    QCOMPARE(offer(10, 0, true), TimelapseWork::Idle);
    QCOMPARE(offer(14, 900, true), TimelapseWork::Idle);
    QCOMPARE(session.status().periodsSkipped, 0);
    QCOMPARE(offer(15, 0, true), TimelapseWork::Idle);
    QCOMPARE(session.status().periodsSkipped, 1);
    QCOMPARE(offer(16), TimelapseWork::Idle);
    QCOMPARE(offer(20), TimelapseWork::Capturing);
    offer(20, 100);
    offer(21);
    session.stop();
    session.waitFinished();
    QCOMPARE(session.status().stacksCompleted, 2);
    const auto log = readTextFile(QDir(session.status().sessionDir).filePath("timelapse.txt"));
    QVERIFY(log.contains("reason=\"transit clip occupied burst grace\""));
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

    // Fast Rebuild video only from already-sharpened folder: must not rewrite FITS
    // AND must write to a new non-destructive video_<UTC>/ folder without touching outDir1/timelapse.mp4.
    const QString reFitsPath = QDir(outDir1).filePath("sharpened/" + reFitsFiles.first());
    const QDateTime reFitsMtime = QFileInfo(reFitsPath).lastModified();
    const QDateTime reMp4Mtime = QFileInfo(QDir(outDir1).filePath("timelapse.mp4")).lastModified();
    QThread::msleep(20);
    VideoToneSettings toneGif = toneCustom;
    toneGif.gamma = 1.4;
    toneGif.writeMp4 = true;
    toneGif.writeGif = true;
    QString outDir2;
    const TimelapseExportResult r2 = resharpenTimelapseSession(
        sessionDir, QDir(outDir1).filePath("sharpened"), sharpCustom, toneGif, /*rebuildVideoOnly=*/true, outDir2);
    QVERIFY2(r2.ok, qPrintable(r2.error));
    QVERIFY(QDir::cleanPath(outDir2) != QDir::cleanPath(outDir1));
    QVERIFY(outDir2.contains("video_"));
    QCOMPARE(QFileInfo(reFitsPath).lastModified(), reFitsMtime);
    QCOMPARE(QFileInfo(QDir(outDir1).filePath("timelapse.mp4")).lastModified(), reMp4Mtime);
    QVERIFY(checkFileHasGifSignature(QDir(outDir2).filePath("timelapse.gif")));
    QVERIFY(decodeWithFfmpeg(QDir(outDir2).filePath("timelapse.mp4")));

    // Verify persisted commonOverlap in sharpening_profile.json actually crops re-sharpened video.
    {
        SharpeningSettings loadedS;
        VideoToneSettings loadedV;
        std::optional<QRect> loadedOverlap;
        QString pErr;
        QVERIFY2(loadProfileJson(sDir.filePath("sharpening_profile.json"), loadedS, loadedV, pErr, &loadedOverlap),
            qPrintable(pErr));
        QVERIFY(loadedOverlap.has_value() && loadedOverlap->isValid());

        // Persist a distinct smaller overlap rect and confirm rebuild honours it.
        const QRect testOverlap(20, 20, 120, 80);
        QVERIFY2(saveProfileJson(sDir.filePath("sharpening_profile.json"), loadedS, loadedV, pErr, testOverlap),
            qPrintable(pErr));
        VideoToneSettings cropTone = toneCustom;
        cropTone.cropToOverlap = true;
        cropTone.writeMp4 = true;
        cropTone.writeGif = false;
        QString outDirCrop;
        const TimelapseExportResult rCrop = resharpenTimelapseSession(
            sessionDir, {}, sharpCustom, cropTone, /*rebuildVideoOnly=*/true, outDirCrop);
        QVERIFY2(rCrop.ok, qPrintable(rCrop.error));
        // Probe video dimensions via ffmpeg stderr/stream check.
        QProcess probe;
        probe.setProgram(findFfmpegExecutable());
        probe.setArguments({"-hide_banner", "-i", QDir(outDirCrop).filePath("timelapse.mp4")});
        probe.start();
        QVERIFY(probe.waitForFinished(10000));
        const QString probeOut = QString::fromUtf8(probe.readAllStandardError());
        QVERIFY2(probeOut.contains("120x80"), qPrintable(probeOut));
    }

    session.stop();
    session.waitTimeLapseFinished();
}

void TimelapseTests::unicodeFitsPathsAndHeaderOnlyMetadata()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString unicodeDir = tmp.filePath(QString::fromUtf8("Πέτρος_Σελήνη"));
    QVERIFY(QDir().mkpath(unicodeDir));

    FitsImage img;
    img.width = 32;
    img.height = 24;
    img.channels = 3;
    img.pixels.resize(32 * 24 * 3, 0.42f);
    img.meta.exptimeSeconds = 0.0125;
    img.meta.qualityScore = 0.77;
    img.meta.camera = "ASI178MC";
    img.meta.sharpeningProfile = "Mild";

    const QString path = QDir(unicodeDir).filePath(QString::fromUtf8("στοίβα_0001.fits"));
    QString err;
    QVERIFY2(writeFitsFloat(path, img, err), qPrintable(err));
    QVERIFY(QFileInfo::exists(path));

    FitsMetadata meta;
    int w = 0, h = 0, ch = 0;
    QVERIFY2(readFitsMetadata(path, meta, w, h, ch, err), qPrintable(err));
    QCOMPARE(w, 32);
    QCOMPARE(h, 24);
    QCOMPARE(ch, 3);
    QVERIFY(std::abs(meta.exptimeSeconds - 0.0125) < 1e-6);
    QVERIFY(std::abs(meta.qualityScore - 0.77) < 1e-6);
    QCOMPARE(meta.camera, std::string("ASI178MC"));

    FitsImage back;
    QVERIFY2(readFitsFloat(path, back, err), qPrintable(err));
    QCOMPARE(back.width, 32);
    QCOMPARE(back.height, 24);
    QCOMPARE(back.channels, 3);
    QVERIFY(std::abs(back.pixels.front() - 0.42f) < 1e-6f);
}

void TimelapseTests::stopMidWindowPreservesBurstAndConcurrentStop()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto field = makeLunarCraterField(48, 48);

    // Part A: Manual stop mid-window must stack and keep the burst already collected.
    {
        TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
        s.intervalSeconds = 10.0;
        s.captureWindowSeconds = 5.0;
        s.keepPercent = 100.0;
        s.maxFramesPerPeriod = 50;
        s.video.writeMp4 = false;
        s.video.writeGif = false;

        QObject ctx;
        TimelapseSession::Config cfg;
        cfg.timelapsesRoot = tmp.filePath("midstop");
        cfg.settings = s;
        cfg.stream.playback = "mid.ser";
        cfg.memory = MemoryBudget(64ULL * 1024 * 1024);

        TimelapseSession session(&ctx, cfg);
        QVERIFY(session.valid());
        for (int i = 0; i < 8; ++i)
            session.onFrame(makeMono8Frame(48, 48, field, static_cast<uint64_t>(i), utcPoint(0, i * 100)));

        // Still inside [0, 5s) capture window when observer presses Stop.
        session.stop("stopped by the observer");
        session.waitFinished();

        const TimelapseStatus st = session.status();
        QCOMPARE(st.stacksCompleted, 1);
        QCOMPARE(cfg.memory.usage().used, size_t{0});
        QCOMPARE(QDir(QDir(st.sessionDir).filePath("stacks")).entryList({"*.fits"}, QDir::Files).size(), 1);
    }

    // Part B: Concurrent source-thread onFrame + GUI-thread status()/stop() race test.
    for (int iter = 0; iter < 5; ++iter)
    {
        TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
        s.intervalSeconds = 0.2;
        s.captureWindowSeconds = 0.15;
        s.keepPercent = 50.0;
        s.maxFramesPerPeriod = 30;
        s.video.writeMp4 = false;
        s.video.writeGif = false;

        QObject ctx;
        TimelapseSession::Config cfg;
        cfg.timelapsesRoot = tmp.filePath(QString("race_%1").arg(iter));
        cfg.settings = s;
        cfg.stream.playback = "race.ser";
        cfg.memory = MemoryBudget(64ULL * 1024 * 1024);

        TimelapseSession session(&ctx, cfg);
        std::atomic<bool> doneFeeding{false};
        std::jthread feeder([&] {
            for (int i = 0; i < 200 && !doneFeeding.load(); ++i)
            {
                session.onFrame(makeMono8Frame(48, 48, field, static_cast<uint64_t>(i), utcPoint(0, i * 5)));
            }
        });
        for (int p = 0; p < 10; ++p)
        {
            const TimelapseStatus snap = session.status();
            Q_UNUSED(snap);
            QThread::msleep(2);
        }
        session.stop("concurrent stop");
        doneFeeding.store(true);
        feeder.join();
        session.waitFinished();
        QCOMPARE(cfg.memory.usage().used, size_t{0});
    }
}

void TimelapseTests::exposureUnitsAndFailedFitsWriteHandling()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto field = makeLunarCraterField(48, 48);

    // Part A: Live-camera microsecond exposure ("10000 us" -> 0.010 s) and non-numeric gain omitted.
    {
        TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
        s.intervalSeconds = 2.0;
        s.captureWindowSeconds = 1.0;
        s.keepPercent = 100.0;
        s.video.writeMp4 = false;
        s.video.writeGif = false;

        QObject ctx;
        TimelapseSession::Config cfg;
        cfg.timelapsesRoot = tmp.filePath("exptime");
        cfg.settings = s;
        cfg.stream.playback = "exp.ser";
        cfg.stream.exposure = "10000 us";
        cfg.stream.gain = "none (painted test image)";

        TimelapseSession session(&ctx, cfg);
        for (int i = 0; i < 5; ++i)
            session.onFrame(makeMono8Frame(48, 48, field, static_cast<uint64_t>(i), utcPoint(0, i * 50)));
        session.stop("done");
        session.waitFinished();

        const QDir stacksDir(QDir(session.status().sessionDir).filePath("stacks"));
        const QStringList files = stacksDir.entryList({"*.fits"}, QDir::Files);
        QCOMPARE(files.size(), 1);
        FitsMetadata meta;
        int w = 0, h = 0, ch = 0;
        QString err;
        QVERIFY2(readFitsMetadata(stacksDir.filePath(files.first()), meta, w, h, ch, err), qPrintable(err));
        QVERIFY2(std::abs(meta.exptimeSeconds - 0.010) < 1e-6,
            qPrintable(QString("exptime=%1").arg(meta.exptimeSeconds)));
        QVERIFY(!meta.hasGain);
    }

    // Part B: Failed FITS write must NOT increment stacksCompleted and must stop with error.
    {
        // Unwritable root (regular file blocking directory creation) marks session invalid.
        const QString blocker = tmp.filePath("blocker_file");
        {
            QFile f(blocker);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("block");
        }
        TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
        s.intervalSeconds = 2.0;
        s.captureWindowSeconds = 1.0;
        s.video.writeMp4 = false;
        s.video.writeGif = false;

        QObject ctx;
        TimelapseSession::Config badCfg;
        badCfg.timelapsesRoot = blocker;
        badCfg.settings = s;
        TimelapseSession badSession(&ctx, badCfg);
        QVERIFY(!badSession.valid());

        // Mid-session FITS write failure: replace stacks/ with a regular file after start.
        TimelapseSession::Config cfg;
        cfg.timelapsesRoot = tmp.filePath("failwrite");
        cfg.settings = s;
        cfg.stream.playback = "fail.ser";
        TimelapseSession session(&ctx, cfg);
        QVERIFY(session.valid());
        const QString sDir = session.status().sessionDir;
        QVERIFY(QDir(sDir).rmdir("stacks"));
        {
            QFile f(QDir(sDir).filePath("stacks"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("not a dir");
        }

        for (int i = 0; i < 5; ++i)
            session.onFrame(makeMono8Frame(48, 48, field, static_cast<uint64_t>(i), utcPoint(0, i * 50)));
        session.onFrame(makeMono8Frame(48, 48, field, 5, utcPoint(1, 50)));
        session.waitFinished();

        const TimelapseStatus st = session.status();
        QCOMPARE(st.stacksCompleted, 0);
        QVERIFY2(st.stopReason.contains("FITS write failed"), qPrintable(st.stopReason));
        const QString logText = readTextFile(QDir(sDir).filePath("timelapse.txt"));
        QVERIFY2(logText.contains("stacks_completed: 0"), qPrintable(logText));
    }
}

void TimelapseTests::rawSerRetainsOnlyKeptFramesAndWorkerDebayers()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const int w = 48, h = 48;
    const auto sharpField = makeLunarCraterField(w, h);
    const auto blurField = gaussianBlur3x3(sharpField, w, h, 8);

    auto makeRggbFrame = [&](const std::vector<float>& lum, uint64_t idx, std::chrono::system_clock::time_point utc) {
        auto f = std::make_shared<Frame>();
        f->width = w;
        f->height = h;
        f->format = PixelFormat::Mono8;
        f->bayer = BayerPattern::RGGB;
        f->significantBits = 8;
        f->frameIndex = idx;
        f->timestamp = utc;
        f->testSource = true;
        f->data.resize(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const float v = lum[static_cast<size_t>(y) * w + x];
                const bool evenY = (y % 2) == 0;
                const bool evenX = (x % 2) == 0;
                const float c = (evenY && evenX) ? v * 0.9f : ((!evenY && !evenX) ? v * 0.6f : v);
                f->data[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
            }
        }
        return f;
    };

    TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
    s.intervalSeconds = 2.0;
    s.captureWindowSeconds = 1.0;
    s.keepPercent = 100.0;
    s.maxFramesPerPeriod = 5;
    s.saveRawSer = true;
    s.video.writeMp4 = false;
    s.video.writeGif = false;

    QObject ctx;
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = tmp.filePath("rawser");
    cfg.settings = s;
    cfg.stream.playback = "bayer.ser";
    cfg.memory = MemoryBudget(64ULL * 1024 * 1024);

    TimelapseSession session(&ctx, cfg);
    // Feed 10 blurry frames first (filling the cap=5 heap), then 10 sharper frames
    // that evict them — saveRawSer must retain ONLY the 5 kept frames, not all 15+.
    for (int i = 0; i < 10; ++i)
        session.onFrame(makeRggbFrame(blurField, static_cast<uint64_t>(i), utcPoint(0, i * 20)));
    for (int i = 10; i < 20; ++i)
        session.onFrame(makeRggbFrame(sharpField, static_cast<uint64_t>(i), utcPoint(0, i * 20)));
    session.stop("done");
    session.waitFinished();

    const TimelapseStatus st = session.status();
    QCOMPARE(st.stacksCompleted, 1);
    QCOMPARE(cfg.memory.usage().used, size_t{0});

    // Verify FITS stack was debayered to 3-channel RGB on the worker thread.
    const QDir stacksDir(QDir(st.sessionDir).filePath("stacks"));
    const QStringList fitsFiles = stacksDir.entryList({"*.fits"}, QDir::Files);
    QCOMPARE(fitsFiles.size(), 1);
    FitsImage stackImg;
    QString err;
    QVERIFY2(readFitsFloat(stacksDir.filePath(fitsFiles.first()), stackImg, err), qPrintable(err));
    QCOMPARE(stackImg.channels, 3);

    // Verify raw SER contains strictly the 5 kept frames in chronological order.
    const QDir rawDir(QDir(st.sessionDir).filePath("raw"));
    const QStringList serFiles = rawDir.entryList({"*.ser"}, QDir::Files);
    QCOMPARE(serFiles.size(), 1);
    std::string serErr;
    auto reader = openRecordingReader(std::filesystem::path(rawDir.filePath(serFiles.first()).toStdWString()), 10.0, serErr);
    QVERIFY2(reader != nullptr, serErr.c_str());
    QCOMPARE(reader->frameCount(), 5);
    for (int i = 1; i < reader->frameCount(); ++i)
        QVERIFY(reader->frameTime(i) >= reader->frameTime(i - 1));
}

void TimelapseTests::rotatedMoonInscribedOverlapHasNoBlackCorners()
{
    const int w = 256, h = 256;
    const auto refField = makeLunarCraterField(w, h);
    FitsImage ref;
    ref.width = w; ref.height = h; ref.channels = 1;
    ref.pixels = refField;

    // Substantial 8-degree Alt-Az rotation + translation: outer boundingRect would
    // include large black corner triangles, while inscribed validOverlap must not.
    FitsImage mov;
    mov.width = w; mov.height = h; mov.channels = 1;
    mov.pixels = warpFloatField(refField, w, h, 8.0, -6.0, 8.0);

    const SessionAlignResult res = alignStackToReference(mov, ref, TimelapsePreset::Moon);
    QVERIFY(!res.usedFallback);
    QVERIFY(res.validOverlap.isValid());
    QVERIFY(res.validOverlap.width() < w - 20);
    QVERIFY(res.validOverlap.height() < h - 20);

    // Every single pixel inside validOverlap—including all four corners—must be non-black.
    const QRect r = res.validOverlap;
    for (int y = r.top(); y <= r.bottom(); ++y)
    {
        for (int x = r.left(); x <= r.right(); ++x)
        {
            const float px = res.aligned.pixels[static_cast<size_t>(y) * w + x];
            QVERIFY2(px > 0.01f, qPrintable(QString("black pixel %1 at (%2,%3) inside overlap (%4,%5 %6x%7)")
                .arg(px).arg(x).arg(y).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())));
        }
    }
}

void TimelapseTests::guidingRecoveryCloudBaselineAndMissedWindows()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const int w = 64, h = 64;
    const auto sharpField = makeLunarCraterField(w, h);
    const auto blurryField = gaussianBlur3x3(sharpField, w, h, 14);

    TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
    s.intervalSeconds = 1.0;
    s.captureWindowSeconds = 0.4;
    s.keepPercent = 100.0;
    s.maxFramesPerPeriod = 20;
    s.video.writeMp4 = false;
    s.video.writeGif = false;

    QObject ctx;
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = tmp.filePath("polish");
    cfg.settings = s;
    cfg.stream.playback = "polish.ser";

    TimelapseSession session(&ctx, cfg);

    // #13: Guiding lock lost sets warning, and recovering clears warningText immediately.
    GuiderStatus gsLost;
    gsLost.lock.state = LockState::Lost;
    session.setGuidingStatus(gsLost, true);
    QVERIFY(session.status().guidingLostWarning);
    QVERIFY(session.status().warningText.contains("Guiding lock lost"));

    GuiderStatus gsOk;
    gsOk.lock.state = LockState::Locked;
    session.setGuidingStatus(gsOk, true);
    QVERIFY(!session.status().guidingLostWarning);
    QVERIFY2(session.status().warningText.isEmpty(), qPrintable(session.status().warningText));

    // #8 + New I:
    // Period 0: blurry/cloudy opening stack.
    for (int i = 0; i < 4; ++i)
        session.onFrame(makeMono8Frame(w, h, blurryField, static_cast<uint64_t>(i), utcPoint(0, i * 60)));
    session.onFrame(makeMono8Frame(w, h, blurryField, 4, utcPoint(0, 450)));
    QTRY_VERIFY_WITH_TIMEOUT(session.status().stacksCompleted >= 1, 5000);

    // Jump straight past Period 1 ([1.0, 1.4)) to t = 2.05s (Period 2) -> must log Period 1 missed!
    for (int i = 0; i < 4; ++i)
        session.onFrame(makeMono8Frame(w, h, sharpField, static_cast<uint64_t>(20 + i), utcPoint(2, 50 + i * 60)));
    session.onFrame(makeMono8Frame(w, h, sharpField, 24, utcPoint(2, 450)));
    QTRY_VERIFY_WITH_TIMEOUT(session.status().stacksCompleted >= 2, 5000);

    // Period 3: another sharp stack -> running median baseline recovers to sharp level.
    for (int i = 0; i < 4; ++i)
        session.onFrame(makeMono8Frame(w, h, sharpField, static_cast<uint64_t>(30 + i), utcPoint(3, 50 + i * 60)));
    session.onFrame(makeMono8Frame(w, h, sharpField, 34, utcPoint(3, 450)));
    QTRY_VERIFY_WITH_TIMEOUT(session.status().stacksCompleted >= 3, 5000);

    // Period 4: cloudy stack -> must trigger cloudWarning against the recovered baseline!
    for (int i = 0; i < 4; ++i)
        session.onFrame(makeMono8Frame(w, h, blurryField, static_cast<uint64_t>(40 + i), utcPoint(4, 50 + i * 60)));
    session.onFrame(makeMono8Frame(w, h, blurryField, 44, utcPoint(4, 450)));
    QTRY_VERIFY_WITH_TIMEOUT(session.status().stacksCompleted >= 4, 5000);

    const TimelapseStatus mid = session.status();
    QVERIFY2(mid.cloudWarning, "Cloud warning should fire after running baseline recovers from blurry stack 1");
    QVERIFY(mid.periodsSkipped >= 1);

    session.stop("done");
    session.waitFinished();

    const QString logText = readTextFile(QDir(mid.sessionDir).filePath("timelapse.txt"));
    QVERIFY2(logText.contains("skip period=1") && logText.contains("window missed"), qPrintable(logText));
}

void TimelapseTests::fitsBitpixStrictValidationAndConsistentScaling()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QString err;

    auto writeRaw16Fits = [&](const QString& path, uint16_t fillVal) {
        int status = 0;
        fitsfile* fptr = nullptr;
        const QByteArray enc = QFile::encodeName(path);
        QCOMPARE(fits_create_diskfile(&fptr, enc.constData(), &status), 0);
        long naxes[2]{16, 16};
        QCOMPARE(fits_create_img(fptr, USHORT_IMG, 2, naxes, &status), 0);
        std::vector<uint16_t> vals(256, fillVal);
        QCOMPARE(fits_write_img(fptr, TUSHORT, 1, 256, vals.data(), &status), 0);
        QCOMPARE(fits_close_file(fptr, &status), 0);
    };

    // 16-bit FITS below and above 4095 ADU must use the exact same /65535 scale (no 16x jump!).
    const QString low16 = tmp.filePath("low16.fits");
    const QString high16 = tmp.filePath("high16.fits");
    writeRaw16Fits(low16, 3000);
    writeRaw16Fits(high16, 5000);

    FitsImage lowImg, highImg;
    QVERIFY2(readFitsFloat(low16, lowImg, err), qPrintable(err));
    QVERIFY2(readFitsFloat(high16, highImg, err), qPrintable(err));
    QVERIFY2(std::abs(lowImg.pixels[0] - (3000.0f / 65535.0f)) < 1e-4f,
        qPrintable(QString("low=%1").arg(lowImg.pixels[0])));
    QVERIFY2(std::abs(highImg.pixels[0] - (5000.0f / 65535.0f)) < 1e-4f,
        qPrintable(QString("high=%1").arg(highImg.pixels[0])));
    QVERIFY(highImg.pixels[0] > lowImg.pixels[0]);

    // Unsupported BITPIX (-64 double and 32 int) must be strictly rejected.
    for (int badBitpix : {DOUBLE_IMG, LONG_IMG})
    {
        const QString badPath = tmp.filePath(QString("bad_%1.fits").arg(badBitpix));
        int status = 0;
        fitsfile* fptr = nullptr;
        const QByteArray enc = QFile::encodeName(badPath);
        QCOMPARE(fits_create_diskfile(&fptr, enc.constData(), &status), 0);
        long naxes[2]{8, 8};
        QCOMPARE(fits_create_img(fptr, badBitpix, 2, naxes, &status), 0);
        std::vector<double> vals(64, 0.5);
        QCOMPARE(fits_write_img(fptr, TDOUBLE, 1, 64, vals.data(), &status), 0);
        QCOMPARE(fits_close_file(fptr, &status), 0);

        FitsImage badOut;
        QString badErr;
        QVERIFY(!readFitsFloat(badPath, badOut, badErr));
        QVERIFY2(badErr.contains("unsupported FITS BITPIX"), qPrintable(badErr));

        FitsMetadata meta;
        int w = 0, h = 0, ch = 0;
        QVERIFY(!readFitsMetadata(badPath, meta, w, h, ch, badErr));
        QVERIFY2(badErr.contains("unsupported FITS BITPIX"), qPrintable(badErr));
    }

    // 32-bit float with out-of-range (<0, >1) or NaN pixels clamps safely into [0, 1].
    const QString oorPath = tmp.filePath("oor_float.fits");
    {
        int status = 0;
        fitsfile* fptr = nullptr;
        const QByteArray enc = QFile::encodeName(oorPath);
        QCOMPARE(fits_create_diskfile(&fptr, enc.constData(), &status), 0);
        long naxes[2]{8, 8};
        QCOMPARE(fits_create_img(fptr, FLOAT_IMG, 2, naxes, &status), 0);
        std::vector<float> vals(64, 0.5f);
        vals[0] = -0.25f;
        vals[1] = 1.75f;
        vals[2] = std::numeric_limits<float>::quiet_NaN();
        QCOMPARE(fits_write_img(fptr, TFLOAT, 1, 64, vals.data(), &status), 0);
        QCOMPARE(fits_close_file(fptr, &status), 0);
    }
    FitsImage oorImg;
    QVERIFY2(readFitsFloat(oorPath, oorImg, err), qPrintable(err));
    QCOMPARE(oorImg.pixels[0], 0.0f);
    QCOMPARE(oorImg.pixels[1], 1.0f);
    QCOMPARE(oorImg.pixels[2], 0.0f);
    QVERIFY(std::abs(oorImg.pixels[10] - 0.5f) < 1e-5f);
}

void TimelapseTests::bayerGrbgGbrgAnd12BitHighAlignedStacking()
{
    auto makeBayer = [](BayerPattern pat, uint8_t r, uint8_t g, uint8_t b) {
        auto f = std::make_shared<Frame>();
        f->width = 40;
        f->height = 40;
        f->format = PixelFormat::Mono8;
        f->bayer = pat;
        f->significantBits = 8;
        f->testSource = true;
        f->data.resize(40 * 40, g);
        for (int y = 0; y < 40; ++y)
        {
            for (int x = 0; x < 40; ++x)
            {
                const bool evenY = (y % 2) == 0;
                const bool evenX = (x % 2) == 0;
                uint8_t val = g;
                if (pat == BayerPattern::GRBG)
                    val = (evenY && !evenX) ? r : ((!evenY && evenX) ? b : g);
                else if (pat == BayerPattern::GBRG)
                    val = (evenY && !evenX) ? b : ((!evenY && evenX) ? r : g);
                f->data[static_cast<size_t>(y) * 40 + x] = val;
            }
        }
        return f;
    };

    const size_t plane = 40 * 40;
    const size_t mid = 20 * 40 + 20;

    // Verify GRBG through captureRawBurstFrame -> stackBurst (worker debayer path).
    {
        std::vector<RawBurstFrame> burst;
        for (int i = 0; i < 4; ++i)
            burst.push_back(captureRawBurstFrame(*makeBayer(BayerPattern::GRBG, 230, 30, 50)));
        const BurstStackResult st = stackBurst(burst);
        QVERIFY2(st.ok, qPrintable(st.error));
        QCOMPARE(st.stack.channels, 3);
        QVERIFY2(st.stack.pixels[mid] > 0.75f, qPrintable(QString("GRBG R=%1").arg(st.stack.pixels[mid])));
        QVERIFY2(st.stack.pixels[plane + mid] < 0.25f, qPrintable(QString("GRBG G=%1").arg(st.stack.pixels[plane + mid])));
        QVERIFY2(st.stack.pixels[2 * plane + mid] < 0.30f, qPrintable(QString("GRBG B=%1").arg(st.stack.pixels[2 * plane + mid])));
    }

    // Verify GBRG through captureRawBurstFrame -> stackBurst (worker debayer path).
    {
        std::vector<RawBurstFrame> burst;
        for (int i = 0; i < 4; ++i)
            burst.push_back(captureRawBurstFrame(*makeBayer(BayerPattern::GBRG, 40, 30, 230)));
        const BurstStackResult st = stackBurst(burst);
        QVERIFY2(st.ok, qPrintable(st.error));
        QCOMPARE(st.stack.channels, 3);
        QVERIFY2(st.stack.pixels[mid] < 0.30f, qPrintable(QString("GBRG R=%1").arg(st.stack.pixels[mid])));
        QVERIFY2(st.stack.pixels[plane + mid] < 0.25f, qPrintable(QString("GBRG G=%1").arg(st.stack.pixels[plane + mid])));
        QVERIFY2(st.stack.pixels[2 * plane + mid] > 0.75f, qPrintable(QString("GBRG B=%1").arg(st.stack.pixels[2 * plane + mid])));
    }

    // Verify 12-bit high-aligned Mono16 (values shifted left by 4 bits) through captureRawBurstFrame -> stackBurst.
    {
        std::vector<RawBurstFrame> burst;
        for (int i = 0; i < 4; ++i)
        {
            auto f = std::make_shared<Frame>();
            f->width = 32;
            f->height = 32;
            f->format = PixelFormat::Mono16;
            f->significantBits = 12;
            f->alignment = BitAlignment::High;
            f->testSource = true;
            f->data.resize(frameBytes(32, 32, PixelFormat::Mono16));
            auto* dst = reinterpret_cast<uint16_t*>(f->data.data());
            // 2048 out of 4095 (~0.5001), placed in the upper 12 bits (2048 << 4 = 32768).
            const uint16_t highWord = static_cast<uint16_t>(2048u << 4);
            std::fill(dst, dst + 32 * 32, highWord);
            burst.push_back(captureRawBurstFrame(*f));
        }
        const BurstStackResult st = stackBurst(burst);
        QVERIFY2(st.ok, qPrintable(st.error));
        QCOMPARE(st.stack.channels, 1);
        QCOMPARE(st.stack.meta.rawMaxAdu, 4095.0);
        const float expected = 2048.0f / 4095.0f;
        QVERIFY2(std::abs(st.stack.pixels[16 * 32 + 16] - expected) < 1e-3f,
            qPrintable(QString("got=%1 expected=%2").arg(st.stack.pixels[16 * 32 + 16]).arg(expected)));
    }
}

void TimelapseTests::multiStackMoonRotationAndPlanetDriftEndToEnd()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    // 1. Multi-stack Moon session: progressive Alt-Az rotation (0, 3.5, 7 deg) + drift
    //    + evolving shadow outside sunlit lunarAnchor + cropToLock.
    {
        const int w = 256, h = 256;
        const auto baseField = makeLunarCraterField(w, h);

        TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
        s.intervalSeconds = 1.0;
        s.captureWindowSeconds = 0.4;
        s.keepPercent = 100.0;
        s.maxFramesPerPeriod = 20;
        s.cropToLock = true;
        s.lunarAnchor = QRect(128, 48, 80, 80);
        s.video.cropToOverlap = true;
        s.video.writeMp4 = true;
        s.video.writeGif = false;
        s.video.fps = 5;

        QObject ctx;
        TimelapseSession::Config cfg;
        cfg.timelapsesRoot = tmp.filePath("moon_rot");
        cfg.settings = s;
        cfg.stream.playback = "moon_rot.ser";
        cfg.lockRegion = LockRegion{64, 64, 128, 128}; // -> effectiveCropRoi = (0,0,256,256)

        TimelapseSession session(&ctx, cfg);
        const std::vector<std::tuple<double, double, double, float>> periods{
            {0.0, 0.0, 0.0, 1.0f},
            {4.0, -3.0, 3.5, 0.55f},
            {8.0, -6.0, 7.0, 0.25f},
        };
        uint64_t idx = 0;
        for (int p = 0; p < static_cast<int>(periods.size()); ++p)
        {
            auto [dx, dy, rot, shadow] = periods[static_cast<size_t>(p)];
            auto field = warpFloatField(baseField, w, h, dx, dy, rot);
            for (int y = 150; y < h; ++y)
                for (int x = 0; x < 110; ++x)
                    field[static_cast<size_t>(y) * w + x] *= shadow;
            for (int f = 0; f < 4; ++f)
                session.onFrame(makeMono8Frame(w, h, field, idx++, utcPoint(p, f * 70)));
            session.onFrame(makeMono8Frame(w, h, field, idx++, utcPoint(p, 450)));
            QTRY_VERIFY_WITH_TIMEOUT(session.status().stacksCompleted >= p + 1, 10000);
        }
        session.stop("moon multi-stack done");
        session.waitFinished();

        const TimelapseStatus st = session.status();
        QCOMPARE(st.stacksCompleted, 3);
        const QString mp4 = QDir(st.sessionDir).filePath("timelapse.mp4");
        QVERIFY(QFileInfo::exists(mp4) && decodeWithFfmpeg(mp4));

        SharpeningSettings loadedS;
        VideoToneSettings loadedV;
        std::optional<QRect> overlap;
        QString pErr;
        QVERIFY2(loadProfileJson(QDir(st.sessionDir).filePath("sharpening_profile.json"),
            loadedS, loadedV, pErr, &overlap), qPrintable(pErr));
        QVERIFY(overlap.has_value() && overlap->isValid());
        // Overlap must have cropped inward to exclude rotated borders.
        QVERIFY(overlap->width() < w - 16 && overlap->height() < h - 16);

        // Verify exported MP4 dimensions match the even-rounded persisted commonOverlap.
        const int expW = std::max(2, overlap->width() & ~1);
        const int expH = std::max(2, overlap->height() & ~1);
        QProcess probe;
        probe.setProgram(findFfmpegExecutable());
        probe.setArguments({"-hide_banner", "-i", mp4});
        probe.start();
        QVERIFY(probe.waitForFinished(10000));
        const QString probeOut = QString::fromUtf8(probe.readAllStandardError());
        QVERIFY2(probeOut.contains(QString("%1x%2").arg(expW).arg(expH)), qPrintable(probeOut));
    }

    // 2. Multi-stack Planet session: drifting disc with rotating surface spot.
    {
        const int w = 160, h = 160;
        auto makePlanetField = [&](double cx, double cy, double phaseDeg) {
            std::vector<float> pix(static_cast<size_t>(w) * h, 0.0f);
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
                    pix[static_cast<size_t>(y) * w + x] = static_cast<float>(std::clamp(disc * bands, 0.0, 1.0));
                }
            }
            return pix;
        };

        TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Planet);
        s.intervalSeconds = 1.0;
        s.captureWindowSeconds = 0.4;
        s.keepPercent = 100.0;
        s.maxFramesPerPeriod = 20;
        s.video.writeMp4 = true;
        s.video.writeGif = false;
        s.video.fps = 5;

        QObject ctx;
        TimelapseSession::Config cfg;
        cfg.timelapsesRoot = tmp.filePath("planet_drift");
        cfg.settings = s;
        cfg.stream.playback = "planet.ser";

        TimelapseSession session(&ctx, cfg);
        const std::vector<std::tuple<double, double, double>> states{
            {80.0, 80.0, 0.0},
            {92.0, 73.0, 25.0},
            {69.0, 89.0, 50.0},
        };
        uint64_t idx = 0;
        for (int p = 0; p < static_cast<int>(states.size()); ++p)
        {
            auto [cx, cy, phase] = states[static_cast<size_t>(p)];
            const auto field = makePlanetField(cx, cy, phase);
            for (int f = 0; f < 4; ++f)
                session.onFrame(makeMono8Frame(w, h, field, idx++, utcPoint(p, f * 70)));
            session.onFrame(makeMono8Frame(w, h, field, idx++, utcPoint(p, 450)));
            QTRY_VERIFY_WITH_TIMEOUT(session.status().stacksCompleted >= p + 1, 10000);
        }
        session.stop("planet done");
        session.waitFinished();

        const TimelapseStatus st = session.status();
        QCOMPARE(st.stacksCompleted, 3);
        QVERIFY(decodeWithFfmpeg(QDir(st.sessionDir).filePath("timelapse.mp4")));

        // Verify the third aligned stack moved the drifted planet back near (80, 80).
        const QDir stacksDir(QDir(st.sessionDir).filePath("stacks"));
        const QStringList files = stacksDir.entryList({"*.fits"}, QDir::Files, QDir::Name);
        QCOMPARE(files.size(), 3);
        FitsImage lastStack;
        QString err;
        QVERIFY2(readFitsFloat(stacksDir.filePath(files.last()), lastStack, err), qPrintable(err));
        QVERIFY2(std::abs(lastStack.meta.shiftX - 11.0) < 1.5 && std::abs(lastStack.meta.shiftY - (-9.0)) < 1.5,
            qPrintable(QString("planet3 shift=(%1,%2)").arg(lastStack.meta.shiftX).arg(lastStack.meta.shiftY)));
    }
}

void TimelapseTests::fakeZwoCameraTimeLapseAndNonBlockingStop()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    auto api = std::make_shared<FakeZwoApi>();
    for (auto& c : api->caps)
        if (c.kind == ZwoControl::Exposure) c.value = 10000; // 10,000 us = 10 ms -> EXPTIME must be 0.010 s!
    const auto profile = ZwoSource::inspect(api, 0);

    QObject ctx;
    bool finishedCalled = false;
    TimelapseFinishResult finishRes;
    Session::Notifications notices;
    notices.timeLapseFinished = [&](const TimelapseFinishResult& r) {
        finishedCalled = true;
        finishRes = r;
    };

    Session session(&ctx, tmp.filePath("zwo_out"), std::move(notices));
    session.setSource(std::make_unique<ZwoSource>(api, profile, profile.current));

    QString err;
    QVERIFY2(session.start(err, Session::Mode::Preview), qPrintable(err));
    QTRY_VERIFY_WITH_TIMEOUT(session.acquiredFrames() >= 4, 3000);

    TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
    s.intervalSeconds = 0.4;
    s.captureWindowSeconds = 0.2;
    s.keepPercent = 100.0;
    s.maxFramesPerPeriod = 30;
    s.video.writeMp4 = true;
    s.video.writeGif = false;
    s.video.fps = 5;

    // Inject a 600 ms worker delay so we can prove Session::stop() returns promptly
    // instead of freezing the GUI thread while stacking/FFmpeg finishes!
    QVERIFY2(session.startTimeLapse(s, err, {}, std::chrono::milliseconds{600}), qPrintable(err));
    QTRY_VERIFY_WITH_TIMEOUT(session.timeLapseStatus().stacksCompleted >= 1, 5000);
    // Let a second window open so stopping triggers another 600 ms delayed stack + MP4 encode.
    QThread::msleep(250);

    const auto stopStart = std::chrono::steady_clock::now();
    session.stop("user stopped stream");
    const double stopMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - stopStart).count();
    QVERIFY2(stopMs < 400.0, qPrintable(QString("Session::stop blocked for %1 ms").arg(stopMs)));

    session.waitTimeLapseFinished();
    QTRY_VERIFY_WITH_TIMEOUT(finishedCalled, 5000);
    QVERIFY(finishRes.ok);
    QVERIFY(finishRes.stacksCompleted >= 1);
    QCOMPARE(session.droppedFrames().value_or(99), uint64_t{0});

    // Verify FITS EXPTIME came from ZwoSource's "10000 us" as 0.010 s (not 10.0 s!).
    const QDir stacksDir(QDir(finishRes.sessionDir).filePath("stacks"));
    const QStringList files = stacksDir.entryList({"*.fits"}, QDir::Files, QDir::Name);
    QVERIFY(!files.isEmpty());
    FitsMetadata meta;
    int w = 0, h = 0, ch = 0;
    QVERIFY2(readFitsMetadata(stacksDir.filePath(files.first()), meta, w, h, ch, err), qPrintable(err));
    QVERIFY2(std::abs(meta.exptimeSeconds - 0.010) < 1e-6,
        qPrintable(QString("ZWO EXPTIME=%1 (expected 0.010)").arg(meta.exptimeSeconds)));
}

void TimelapseTests::resharpenPreviewMatchesExportedFrame()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const int w = 96, h = 96;
    const auto field = makeLunarCraterField(w, h);
    const QString sessionDir = tmp.filePath("parity_session");
    const QString stacksDir = QDir(sessionDir).filePath("stacks");
    QVERIFY(QDir().mkpath(stacksDir));

    // Create two stacks with different brightnesses (stack 1 = bright/high-Q, stack 2 = dimmer/lower-Q).
    FitsImage s1;
    s1.width = w; s1.height = h; s1.channels = 1;
    s1.pixels = field;
    s1.meta.qualityScore = 0.80;

    FitsImage s2 = s1;
    for (float& px : s2.pixels) px *= 0.65f;
    s2.meta.qualityScore = 0.50;

    QString err;
    QVERIFY2(writeFitsFloat(QDir(stacksDir).filePath("0001_a.fits"), s1, err), qPrintable(err));
    QVERIFY2(writeFitsFloat(QDir(stacksDir).filePath("0002_b.fits"), s2, err), qPrintable(err));

    const SharpeningSettings sharp = sharpeningForPreset(SharpeningPreset::Medium);
    VideoToneSettings tone;
    tone.autoStretch = true;
    tone.gamma = 1.15;
    tone.brightnessMode = BrightnessLevellingMode::Global;
    tone.cropToOverlap = true;
    tone.writeMp4 = true;
    tone.writeGif = false;
    tone.fps = 5;
    const QRect overlap(8, 8, 80, 80);
    QVERIFY2(saveProfileJson(QDir(sessionDir).filePath("sharpening_profile.json"), sharp, tone, err, overlap),
        qPrintable(err));

    // Run full re-sharpen + video export.
    QString outDir;
    const TimelapseExportResult res = resharpenTimelapseSession(sessionDir, {}, sharp, tone, false, outDir);
    QVERIFY2(res.ok, qPrintable(res.error));

    // Compute what ResharpenDialog's preview pipeline produces for stack 2 (non-best stack, levelled to best).
    const FitsImage sharp1 = applyLinearSharpening(s1, sharp);
    const FitsImage sharp2 = applyLinearSharpening(s2, sharp);
    double black = 0, white = 1;
    computeAutoStretchLevels(sharp1, black, white, overlap);
    const QImage previewFrame2 = toneMapFitsToImage(sharp2, black, white, tone.gamma, 1.0, overlap, 0,
        &sharp1, tone.brightnessMode, true);
    QCOMPARE(previewFrame2.width(), 80);
    QCOMPARE(previewFrame2.height(), 80);

    // Decode the actual MP4 frames back to raw rgb24 and compare frame 2 against previewFrame2.
    QProcess dec;
    dec.setProgram(findFfmpegExecutable());
    dec.setArguments({
        "-v", "error",
        "-i", QDir(outDir).filePath("timelapse.mp4"),
        "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"
    });
    dec.start();
    QVERIFY(dec.waitForFinished(10000));
    QCOMPARE(dec.exitCode(), 0);
    const QByteArray raw = dec.readAllStandardOutput();
    const int frameSize = 80 * 80 * 3;
    QCOMPARE(raw.size(), frameSize * 2);

    const QImage previewRgb = previewFrame2.convertToFormat(QImage::Format_RGB888);
    const auto* mp4Frame2 = reinterpret_cast<const uint8_t*>(raw.constData() + frameSize);
    double sumAbsDiff = 0.0;
    for (int y = 0; y < 80; ++y)
    {
        const uint8_t* pRow = previewRgb.constScanLine(y);
        const uint8_t* mRow = mp4Frame2 + y * 80 * 3;
        for (int x = 0; x < 80 * 3; ++x)
            sumAbsDiff += std::abs(static_cast<int>(pRow[x]) - static_cast<int>(mRow[x]));
    }
    const double mae = sumAbsDiff / static_cast<double>(frameSize);
    QVERIFY2(mae < 8.0, qPrintable(QString("Preview vs MP4 frame 2 MAE=%1 (expected < 8)").arg(mae)));
}

void TimelapseTests::cloudBrightnessEqualizationAndClearReferenceSelection()
{
    const int w = 160, h = 128;
    const QRect overlap(12, 12, 136, 104);
    const auto base = makeLunarCraterField(w, h);

    // 1. Clear reference selection must ignore a thick-cloud stack whose qualityScore was inflated.
    const std::vector<double> quals{0.0015, 0.00164, 0.0014, 0.00255};
    const std::vector<double> spans{0.40,   0.50,    0.25,   0.045};
    QCOMPARE(selectClearReferenceStack(quals, spans), 1);

    // 2. Build a clear reference stack with a 0.0 rotated border outside overlap, plus:
    //    - heavy uniform cloud (10x dim + 0.020 additive haze)
    //    - patchy cloud (left half 0.35x dim + haze, right half clear 1.0x)
    //    - advancing lunar terminator shadow under 3x cloud + haze
    auto makeMoonFits = [&](const std::vector<float>& pix, double q) {
        FitsImage img;
        img.width = w; img.height = h; img.channels = 1;
        img.pixels = pix;
        img.meta.qualityScore = q;
        img.meta.targetPreset = "moon";
        return img;
    };

    std::vector<float> clearPix(static_cast<size_t>(w) * h, 0.0f);
    for (int y = overlap.top(); y <= overlap.bottom(); ++y)
        for (int x = overlap.left(); x <= overlap.right(); ++x)
            clearPix[static_cast<size_t>(y) * w + x] = 0.02f + 0.70f * base[static_cast<size_t>(y) * w + x];
    const FitsImage clearRef = makeMoonFits(clearPix, 0.00164);

    // Auto-stretch inside overlap must not be dragged down to 0.0 by borders outside overlap.
    double bUncropped = 0, wUncropped = 1, bCropped = 0, wCropped = 1;
    computeAutoStretchLevels(clearRef, bUncropped, wUncropped);
    computeAutoStretchLevels(clearRef, bCropped, wCropped, overlap);
    QVERIFY(bUncropped < 0.005);
    QVERIFY2(bCropped > 0.05, qPrintable(QString("bCropped=%1").arg(bCropped)));

    // Heavy 10x cloud + 0.020 scattered haze
    std::vector<float> heavyPix = clearPix;
    for (float& px : heavyPix) px = 0.020f + 0.10f * px;
    const FitsImage heavyCloud = makeMoonFits(heavyPix, 0.00255);

    const QImage refImg8 = toneMapFitsToImage(clearRef, bCropped, wCropped, 1.0, 1.0, overlap, 0);
    const QImage eqHeavy8 = toneMapFitsToImage(heavyCloud, bCropped, wCropped, 1.0, 1.0, overlap, 0,
        &clearRef, BrightnessLevellingMode::Global, true);
    double maeHeavy = 0.0;
    int burnedHeavy = 0;
    for (int y = 0; y < refImg8.height(); ++y)
    {
        const uint8_t* r = refImg8.constScanLine(y);
        const uint8_t* e = eqHeavy8.constScanLine(y);
        for (int x = 0; x < refImg8.width(); ++x)
        {
            maeHeavy += std::abs(static_cast<int>(e[x]) - static_cast<int>(r[x]));
            if (e[x] >= 254) ++burnedHeavy;
        }
    }
    maeHeavy /= static_cast<double>(refImg8.width() * refImg8.height());
    const double burnedHeavyPct = 100.0 * burnedHeavy / static_cast<double>(refImg8.width() * refImg8.height());
    QVERIFY2(maeHeavy < 4.0, qPrintable(QString("10x cloud Global MAE=%1").arg(maeHeavy)));
    QVERIFY2(burnedHeavyPct < 1.0, qPrintable(QString("10x cloud burned=%1%").arg(burnedHeavyPct)));

    // Patchy cloud: left half 0.35x + haze, right half clear 1.0x.
    // Global highlight guard must prevent burning the clear right half; GlobalAndPatchy must even both halves!
    std::vector<float> patchyPix = clearPix;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w / 2; ++x)
            patchyPix[static_cast<size_t>(y) * w + x] = 0.015f + 0.35f * clearPix[static_cast<size_t>(y) * w + x];
    const FitsImage patchyCloud = makeMoonFits(patchyPix, 0.0014);

    const QImage eqPatchyGlobal = toneMapFitsToImage(patchyCloud, bCropped, wCropped, 1.0, 1.0, overlap, 0,
        &clearRef, BrightnessLevellingMode::Global, true);
    const QImage eqPatchyLocal = toneMapFitsToImage(patchyCloud, bCropped, wCropped, 1.0, 1.0, overlap, 0,
        &clearRef, BrightnessLevellingMode::GlobalAndPatchy, true);
    int burnedRightGlobal = 0, burnedRightLocal = 0;
    double leftDiffGlobal = 0, rightDiffGlobal = 0, leftDiffLocal = 0, rightDiffLocal = 0;
    const int halfW = refImg8.width() / 2;
    const int halfCount = halfW * refImg8.height();
    for (int y = 0; y < refImg8.height(); ++y)
    {
        const uint8_t* r = refImg8.constScanLine(y);
        const uint8_t* g = eqPatchyGlobal.constScanLine(y);
        const uint8_t* l = eqPatchyLocal.constScanLine(y);
        for (int x = 0; x < refImg8.width(); ++x)
        {
            if (x < halfW)
            {
                leftDiffGlobal += std::abs(static_cast<int>(g[x]) - static_cast<int>(r[x]));
                leftDiffLocal += std::abs(static_cast<int>(l[x]) - static_cast<int>(r[x]));
            }
            else
            {
                if (g[x] >= 254) ++burnedRightGlobal;
                if (l[x] >= 254) ++burnedRightLocal;
                rightDiffGlobal += std::abs(static_cast<int>(g[x]) - static_cast<int>(r[x]));
                rightDiffLocal += std::abs(static_cast<int>(l[x]) - static_cast<int>(r[x]));
            }
        }
    }
    QVERIFY2(100.0 * burnedRightGlobal / halfCount < 2.0,
        qPrintable(QString("Global highlight guard failed: burnedRight=%1%").arg(100.0 * burnedRightGlobal / halfCount)));
    QVERIFY2(100.0 * burnedRightLocal / halfCount < 2.0,
        qPrintable(QString("Local patchy burnedRight=%1%").arg(100.0 * burnedRightLocal / halfCount)));
    QVERIFY2(leftDiffLocal < leftDiffGlobal * 0.65,
        qPrintable(QString("Local patchy should reduce left cloud error: local=%1 global=%2")
            .arg(leftDiffLocal / halfCount).arg(leftDiffGlobal / halfCount)));

    // 3. Planet (rotating small disc on dark sky, alignedPixels=false) under 5x cloud + haze.
    auto makePlanetFits = [&](double spotOffsetX, double cloudScale, double haze) {
        FitsImage img;
        img.width = 160; img.height = 160; img.channels = 1;
        img.pixels.resize(160 * 160);
        img.meta.targetPreset = "planet";
        for (int y = 0; y < 160; ++y)
            for (int x = 0; x < 160; ++x)
            {
                const double r = std::hypot(x - 80.0, y - 80.0);
                const double disc = 1.0 / (1.0 + std::exp((r - 20.0) / 1.4));
                const double spot = 0.25 * std::exp(-((x - (80.0 + spotOffsetX)) * (x - (80.0 + spotOffsetX))
                    + (y - 78.0) * (y - 78.0)) / 24.0);
                const double v = 0.015 + haze + cloudScale * disc * (0.70 - spot);
                img.pixels[static_cast<size_t>(y) * 160 + x] = static_cast<float>(std::clamp(v, 0.0, 1.0));
            }
        return img;
    };
    const FitsImage planetRef = makePlanetFits(-5.0, 1.0, 0.0);
    const FitsImage planetCloud = makePlanetFits(6.0, 0.20, 0.025);
    double pb = 0, pw = 1;
    computeAutoStretchLevels(planetRef, pb, pw);
    const QImage pRef8 = toneMapFitsToImage(planetRef, pb, pw, 1.0, 1.0);
    const QImage pEq8 = toneMapFitsToImage(planetCloud, pb, pw, 1.0, 1.0, {}, 0,
        &planetRef, BrightnessLevellingMode::Global, false);
    double refDisc = 0, eqDisc = 0, eqSky = 0;
    int nDisc = 0, nSky = 0;
    for (int y = 0; y < 160; ++y)
    {
        const uint8_t* r = pRef8.constScanLine(y);
        const uint8_t* e = pEq8.constScanLine(y);
        for (int x = 0; x < 160; ++x)
        {
            const double d = std::hypot(x - 80.0, y - 80.0);
            if (d < 15.0) { refDisc += r[x]; eqDisc += e[x]; ++nDisc; }
            else if (d > 40.0) { eqSky += e[x]; ++nSky; }
        }
    }
    QVERIFY2(std::abs(eqDisc / nDisc - refDisc / nDisc) < 12.0,
        qPrintable(QString("Planet disc mean eq=%1 ref=%2").arg(eqDisc / nDisc).arg(refDisc / nDisc)));
    QVERIFY2(eqSky / nSky < 8.0, qPrintable(QString("Planet hazy sky not removed: %1").arg(eqSky / nSky)));

    // 4. Legacy sharpening_profile.json without "brightnessMode" must still parse "brightnessLevelling" bool.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString legacyPath = tmp.filePath("legacy.json");
    {
        QFile f(legacyPath);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(R"({"version":1,"video":{"brightnessLevelling":false,"fps":12}})");
    }
    SharpeningSettings ls;
    VideoToneSettings lv;
    QString lErr;
    QVERIFY2(loadProfileJson(legacyPath, ls, lv, lErr), qPrintable(lErr));
    QCOMPARE(lv.brightnessMode, BrightnessLevellingMode::Off);

    // 5. Full exportTimelapseFromFits picks clearRef (not inflated-Q heavyCloud) and writes an unburned MP4.
    const QString fitsDir = tmp.filePath("fits");
    const QString outDir = tmp.filePath("out");
    QVERIFY(QDir().mkpath(fitsDir));
    QVERIFY2(writeFitsFloat(QDir(fitsDir).filePath("0001_clear.fits"), clearRef, lErr), qPrintable(lErr));
    QVERIFY2(writeFitsFloat(QDir(fitsDir).filePath("0002_heavy.fits"), heavyCloud, lErr), qPrintable(lErr));
    VideoToneSettings expTone;
    expTone.autoStretch = true;
    expTone.brightnessMode = BrightnessLevellingMode::Global;
    expTone.cropToOverlap = true;
    expTone.writeMp4 = true;
    expTone.writeGif = false;
    expTone.fps = 5;
    const TimelapseExportResult expRes = exportTimelapseFromFits(fitsDir, outDir, expTone, overlap);
    QVERIFY2(expRes.ok, qPrintable(expRes.error));
    QVERIFY(decodeWithFfmpeg(expRes.mp4Path));
}

void TimelapseTests::realArchiveRecordingsIfPresent()
{
    const QDir testDataDir(QStringLiteral(SELENEWATCH_SOURCE_DIR) + "/test_data");
    if (!testDataDir.exists())
        QSKIP("Local gitignored test_data/ folder not present; skipping real-recording integration test.");

    auto runClip = [&](const QString& fileName, TimelapsePreset preset, double intervalSec, double windowSec) {
        const QString clipPath = testDataDir.filePath(fileName);
        if (!QFileInfo::exists(clipPath)) return;

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QObject ctx;
        bool finished = false;
        TimelapseFinishResult finishRes;
        Session::Notifications notices;
        notices.timeLapseFinished = [&](const TimelapseFinishResult& r) {
            finished = true;
            finishRes = r;
        };
        Session session(&ctx, tmp.filePath("out"), std::move(notices));

        auto src = std::make_unique<RecordingSource>(std::filesystem::path(clipPath.toStdWString()));
        src->setSpeed(RecordingSource::Speed::Fast);
        src->setNominalFps(30.0);
        session.setSource(std::move(src));

        QString err;
        QVERIFY2(session.start(err, Session::Mode::Preview), qPrintable(fileName + ": " + err));

        TimelapseSettings s = defaultTimelapseSettings(preset);
        s.intervalSeconds = intervalSec;
        s.captureWindowSeconds = windowSec;
        s.keepPercent = 50.0;
        s.maxFramesPerPeriod = 25;
        s.stopKind = StopConditionKind::StackCount;
        s.targetStacks = 2;
        s.video.writeMp4 = true;
        s.video.writeGif = false;
        s.video.fps = 5;

        QVERIFY2(session.startTimeLapse(s, err), qPrintable(fileName + ": " + err));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 45000);
        QVERIFY2(finishRes.ok, qPrintable(fileName + ": " + finishRes.exportResult.error));
        QVERIFY2(finishRes.stacksCompleted >= 1, qPrintable(fileName));

        const QDir stacksDir(QDir(finishRes.sessionDir).filePath("stacks"));
        const QStringList fitsFiles = stacksDir.entryList({"*.fits"}, QDir::Files, QDir::Name);
        QVERIFY2(!fitsFiles.isEmpty(), qPrintable(fileName));
        FitsMetadata meta;
        int w = 0, h = 0, ch = 0;
        QVERIFY2(readFitsMetadata(stacksDir.filePath(fitsFiles.first()), meta, w, h, ch, err), qPrintable(err));
        QVERIFY(w > 0 && h > 0);
        // FireCapture .txt companion logs supply exposure in ms -> EXPTIME must be positive & < 1 s.
        if (QFileInfo::exists(QFileInfo(clipPath).absolutePath() + "/" + QFileInfo(clipPath).completeBaseName() + ".txt"))
        {
            QVERIFY2(meta.exptimeSeconds > 0.0 && meta.exptimeSeconds < 1.0,
                qPrintable(QString("%1 EXPTIME=%2").arg(fileName).arg(meta.exptimeSeconds)));
        }

        const QString mp4 = QDir(finishRes.sessionDir).filePath("timelapse.mp4");
        QVERIFY2(QFileInfo::exists(mp4) && decodeWithFfmpeg(mp4), qPrintable(fileName));
        session.stop();
        session.waitTimeLapseFinished();
    };

    runClip("Moon_221449.avi", TimelapsePreset::Moon, 1.5, 0.6);
    runClip("Mars_222831.avi", TimelapsePreset::Planet, 1.5, 0.6);
    runClip("Moon_171311.ser", TimelapsePreset::Moon, 2.0, 0.6);
}

namespace {

TimelapseSettings quickSettings()
{
    TimelapseSettings s = defaultTimelapseSettings(TimelapsePreset::Moon);
    s.intervalSeconds = 2.0;
    s.captureWindowSeconds = 1.0;
    s.keepPercent = 100.0;
    s.maxFramesPerPeriod = 200;
    s.video.writeMp4 = false;
    s.video.writeGif = false;
    return s;
}

} // namespace

// Issue 1: raw SER keeps its own budgeted copies (the camera's frame goes back
// at once), and a burst stops admitting frames at the RAM headroom instead of
// using the last of the ceiling. The SER holds exactly the stacked frames.
void TimelapseTests::rawSerCopiesFramesWithinRamHeadroom()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const int w = 48, h = 48;
    const auto field = makeLunarCraterField(w, h);

    TimelapseSettings s = quickSettings();
    s.saveRawSer = true;

    QObject ctx;
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = tmp.filePath("headroom");
    cfg.settings = s;
    cfg.stream.playback = "headroom.ser";
    // Each admitted frame costs a 2304-byte raw copy plus a 2304-byte cropped
    // copy. 90 % of 25000 bytes admits four frames; the fifth's cropped copy
    // would pass the headroom, so it and every later frame are refused.
    cfg.memory = MemoryBudget(25000);

    TimelapseSession session(&ctx, cfg);
    std::vector<FramePtr> fed;
    for (int i = 0; i < 8; ++i)
    {
        fed.push_back(makeMono8Frame(w, h, field, static_cast<uint64_t>(i), utcPoint(0, i * 100)));
        session.onFrame(fed.back());
        QCOMPARE(fed.back().use_count(), 1L); // Never holds the source's frame
    }
    QVERIFY(cfg.memory.usage().used <= 22500);
    session.stop("done");
    session.waitFinished();

    const TimelapseStatus st = session.status();
    QCOMPARE(st.stacksCompleted, 1);
    QCOMPARE(cfg.memory.usage().used, size_t{0});

    const QDir stacksDir(QDir(st.sessionDir).filePath("stacks"));
    const QStringList fitsFiles = stacksDir.entryList({"*.fits"}, QDir::Files);
    QCOMPARE(fitsFiles.size(), 1);
    FitsMetadata meta;
    int mw = 0, mh = 0, mc = 0;
    QString err;
    QVERIFY2(readFitsMetadata(stacksDir.filePath(fitsFiles.first()), meta, mw, mh, mc, err), qPrintable(err));
    QCOMPARE(meta.framesKept, uint64_t{4});
    QCOMPARE(meta.framesCaptured, uint64_t{8});

    const QDir rawDir(QDir(st.sessionDir).filePath("raw"));
    const QStringList serFiles = rawDir.entryList({"*.ser"}, QDir::Files);
    QCOMPARE(serFiles.size(), 1);
    std::string serErr;
    auto reader = openRecordingReader(std::filesystem::path(rawDir.filePath(serFiles.first()).toStdWString()), 10.0, serErr);
    QVERIFY2(reader != nullptr, serErr.c_str());
    QCOMPARE(reader->frameCount(), 4);

    const QString logText = readTextFile(QDir(st.sessionDir).filePath("timelapse.txt"));
    QVERIFY2(logText.contains("ram_refused=4"), qPrintable(logText));
}

// Issue 4: a burst queued behind the stack that reaches the target is not stacked.
void TimelapseTests::stackCountTargetDiscardsQueuedBurst()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto field = makeLunarCraterField(48, 48);

    TimelapseSettings s = quickSettings();
    s.stopKind = StopConditionKind::StackCount;
    s.targetStacks = 1;

    QObject ctx;
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = tmp.filePath("target");
    cfg.settings = s;
    cfg.stream.playback = "target.ser";
    cfg.memory = MemoryBudget(64ULL * 1024 * 1024);

    TimelapseSession session(&ctx, cfg);
    session.setStackDelay(std::chrono::milliseconds(1500));
    uint64_t index = 0;
    for (int i = 0; i < 5; ++i)
        session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(0, i * 100)));
    // After the window, before period 1: hands period 0 to the (slow) worker.
    session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(1, 500)));
    QTest::qWait(200); // The worker takes it, so period 1 is not skipped for overload
    for (int i = 0; i < 5; ++i)
        session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(2, i * 100)));
    // Closes period 1 while period 0 is still being stacked: it waits in the queue.
    session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(3, 500)));
    session.waitFinished();

    const TimelapseStatus st = session.status();
    QCOMPARE(st.stacksCompleted, 1);
    QCOMPARE(st.periodsSkipped, 0);
    QCOMPARE(st.stopReason, QString("target stack count reached"));
    QCOMPARE(QDir(QDir(st.sessionDir).filePath("stacks")).entryList({"*.fits"}, QDir::Files).size(), 1);
    QCOMPARE(cfg.memory.usage().used, size_t{0});
    const QString logText = readTextFile(QDir(st.sessionDir).filePath("timelapse.txt"));
    QVERIFY2(logText.contains("discarded period=1"), qPrintable(logText));
}

// Issue 2: the worker thread's exception boundary. A RAM refusal costs one
// period; anything else stops the time-lapse (not the program) with a record.
void TimelapseTests::stackingErrorsSkipOrStopTheSessionOnly()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto field = makeLunarCraterField(48, 48);

    // A: RAM refused while stacking period 0 -> skipped; period 1 is stacked.
    {
        QObject ctx;
        TimelapseSession::Config cfg;
        cfg.timelapsesRoot = tmp.filePath("memory");
        cfg.settings = quickSettings();
        cfg.stream.playback = "memory.ser";
        cfg.memory = MemoryBudget(64ULL * 1024 * 1024);
        TimelapseSession session(&ctx, cfg);
        session.setBeforeStackForTests([](int period) {
            if (period == 0) throw MemoryLimitError();
        });
        uint64_t index = 0;
        for (int i = 0; i < 5; ++i)
            session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(0, i * 100)));
        session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(1, 500)));
        QTRY_COMPARE_WITH_TIMEOUT(session.status().periodsSkipped, 1, 5000);
        for (int i = 0; i < 5; ++i)
            session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(2, i * 100)));
        session.stop("done");
        session.waitFinished();

        const TimelapseStatus st = session.status();
        QCOMPARE(st.stacksCompleted, 1);
        QCOMPARE(st.periodsSkipped, 1);
        QCOMPARE(st.stopReason, QString("done"));
        QCOMPARE(cfg.memory.usage().used, size_t{0});
        const QString logText = readTextFile(QDir(st.sessionDir).filePath("timelapse.txt"));
        QVERIFY2(logText.contains("skip period=0") && logText.contains("RAM limit while stacking"), qPrintable(logText));
    }

    // B: an unexpected exception stops the session, records it, and still finishes.
    {
        QObject ctx;
        TimelapseSession::Config cfg;
        cfg.timelapsesRoot = tmp.filePath("fault");
        cfg.settings = quickSettings();
        cfg.stream.playback = "fault.ser";
        cfg.memory = MemoryBudget(64ULL * 1024 * 1024);
        TimelapseSession session(&ctx, cfg);
        session.setBeforeStackForTests([](int) { throw std::runtime_error("injected fault"); });
        uint64_t index = 0;
        for (int i = 0; i < 5; ++i)
            session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(0, i * 100)));
        session.onFrame(makeMono8Frame(48, 48, field, index++, utcPoint(1, 500)));
        session.waitFinished(); // Ends on its own: no stop() from the test

        const TimelapseStatus st = session.status();
        QVERIFY(!session.active());
        QCOMPARE(st.stacksCompleted, 0);
        QVERIFY2(st.stopReason.contains("internal error: injected fault"), qPrintable(st.stopReason));
        QVERIFY2(st.warningText.contains("Internal error"), qPrintable(st.warningText));
        QCOMPARE(cfg.memory.usage().used, size_t{0});
        const QString logText = readTextFile(QDir(st.sessionDir).filePath("timelapse.txt"));
        QVERIFY2(logText.contains("error period=0") && logText.contains("what=\"injected fault\""), qPrintable(logText));
        QVERIFY2(logText.contains("stop_reason: internal error: injected fault"), qPrintable(logText));
    }
}

// Issue 5: closing the program can abandon the video; the stacks stay and the
// log says how to make the video later.
void TimelapseTests::cancelledExportKeepsStacksAndSaysHowToRebuild()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const auto field = makeLunarCraterField(48, 48);

    TimelapseSettings s = quickSettings();
    s.video.writeMp4 = true;

    QObject ctx;
    TimelapseSession::Config cfg;
    cfg.timelapsesRoot = tmp.filePath("cancel");
    cfg.settings = s;
    cfg.stream.playback = "cancel.ser";
    TimelapseSession session(&ctx, cfg);
    for (int i = 0; i < 5; ++i)
        session.onFrame(makeMono8Frame(48, 48, field, static_cast<uint64_t>(i), utcPoint(0, i * 100)));
    session.cancelExport();
    session.stop("the program closed");
    session.waitFinished();

    const TimelapseStatus st = session.status();
    QCOMPARE(st.stacksCompleted, 1);
    QVERIFY(!QFileInfo::exists(QDir(st.sessionDir).filePath("timelapse.mp4")));
    QCOMPARE(QDir(QDir(st.sessionDir).filePath("sharpened")).entryList({"*.fits"}, QDir::Files).size(), 1);
    const QString logText = readTextFile(QDir(st.sessionDir).filePath("timelapse.txt"));
    QVERIFY2(logText.contains("export_error: video not built (cancelled when the program closed)"), qPrintable(logText));
}

// Issue 3: the event preview GIF runs on the evidence writer's thread, so
// FFmpeg is stopped once its time is up.
void TimelapseTests::eventPreviewGifStopsFfmpegAtTimeLimit()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    std::vector<QImage> frames;
    for (int i = 0; i < 10; ++i)
    {
        QImage img(256, 256, QImage::Format_RGB888);
        img.fill(QColor(20 * i, 40, 200 - 10 * i));
        frames.push_back(img);
    }
    const QString gifPath = tmp.filePath("clip/04_event_preview.gif");

    EventPreviewGifOptions options;
    options.timeoutMs = 1; // Already over by the time FFmpeg has started
    QString error;
    QVERIFY(!encodeEventPreviewGif(frames, gifPath, options, error));
    QVERIFY2(error.contains("FFmpeg did not finish within"), qPrintable(error));
    QVERIFY(!QFileInfo::exists(gifPath));
    QVERIFY(QDir(tmp.filePath("clip")).entryList({"*.tmp.gif"}, QDir::Files | QDir::Hidden).isEmpty());

    // The normal limit leaves room for the real encode.
    options.timeoutMs = 30000;
    QVERIFY2(encodeEventPreviewGif(frames, gifPath, options, error), qPrintable(error));
    QVERIFY(checkFileHasGifSignature(gifPath));
}

// Feature-36: the time-lapse's video is fragmented and flushed as it goes, so
// a night cut short by a crash or a power cut still plays up to its last
// fragment (a keyframe every 10 frames).
void TimelapseTests::anUnclosedVideoStillPlays()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    Mp4Stream stream;
    QString error;
    const QString path = QDir(tmp.path()).filePath("open.mp4");
    QVERIFY2(stream.open(path, 320, 240, 25, error), qPrintable(error));
    for (int i = 0; i < 35; ++i)
    {
        QImage image(320, 240, QImage::Format_RGB888);
        image.fill(QColor(i * 7, 100, 200 - i * 5));
        QVERIFY2(stream.write(image, error), qPrintable(error));
    }
    // A copy of the file as it is on disk while FFmpeg still runs.
    const QString copy = QDir(tmp.path()).filePath("copy.mp4");
    int decoded = 0;
    for (int attempt = 0; attempt < 20 && decoded < 30; ++attempt)
    {
        QThread::msleep(250);
        QFile::remove(copy);
        QVERIFY(QFile::copy(path, copy));
        QProcess probe;
        probe.setProgram(findFfmpegExecutable());
        probe.setArguments({"-v", "info", "-i", copy, "-map", "0:v", "-f", "null", "-"});
        probe.start();
        QVERIFY(probe.waitForFinished(10000));
        const QByteArray out = probe.readAllStandardError();
        QRegularExpressionMatchIterator frames = QRegularExpression(R"(frame=\s*(\d+))").globalMatch(QString::fromUtf8(out));
        while (probe.exitCode() == 0 && frames.hasNext()) decoded = frames.next().captured(1).toInt();
    }
    QVERIFY2(decoded >= 30, qPrintable(QString::number(decoded)));
    QVERIFY(stream.close(error));
    QVERIFY(decodeWithFfmpeg(path));
}

QTEST_MAIN(TimelapseTests)
#include "timelapse_tests.moc"
