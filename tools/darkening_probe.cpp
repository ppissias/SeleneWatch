#include "tools/darkening_probe.h"

#include "detection/flash_detector.h"
#include "recording/recording_reader.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

// Candidates before this are the detector's start-up, not counted.
constexpr double kStartupSeconds = 3.0;
// By then the reference has learned for three of its time constants.
constexpr double kFirstCrossingSeconds = 6.0;
constexpr double kCrossingSpacingSeconds = 6.0;

// An elliptical silhouette, by its half-axes in pixels.
struct Shape
{
    const char* name;
    int halfWidth;
    int halfHeight;
};
constexpr Shape kShapes[] = {{"satellite", 30, 12}, {"ISS or distant plane", 80, 30}};
// The ISS crosses a 7-arcminute field in three or four frames; a plane in
// a few frames to about a second.
constexpr int kCrossingFrames[] = {3, 15};
// The share of the light the silhouette takes away.
constexpr double kDepths[] = {0.3, 0.6, 0.9};

// One crossing: its frames, its direction and the row its centre follows.
struct Crossing
{
    int first{0};
    int last{0};
    bool leftToRight{true};
    int row{0};
};

std::vector<Crossing> schedule(const std::vector<double>& seconds, int frames, int height)
{
    std::vector<Crossing> crossings;
    double next = kFirstCrossingSeconds;
    for (int i = 0; i < static_cast<int>(seconds.size()); ++i)
    {
        if (seconds[static_cast<size_t>(i)] < next) continue;
        if (i + frames > static_cast<int>(seconds.size())) break;
        const int n = static_cast<int>(crossings.size());
        crossings.push_back({i, i + frames - 1, n % 2 == 0, n % 3 == 0 ? height / 2 : n % 3 == 1 ? height / 3 : 2 * height / 3});
        next = seconds[static_cast<size_t>(i)] + kCrossingSpacingSeconds;
    }
    return crossings;
}

// Takes the share depth of the light inside an ellipse, in every channel.
void darken(Frame& frame, double centreX, double centreY, const Shape& shape, double depth)
{
    const int x0 = std::max(0, static_cast<int>(std::floor(centreX - shape.halfWidth)));
    const int x1 = std::min(frame.width - 1, static_cast<int>(std::ceil(centreX + shape.halfWidth)));
    const int y0 = std::max(0, static_cast<int>(std::floor(centreY - shape.halfHeight)));
    const int y1 = std::min(frame.height - 1, static_cast<int>(std::ceil(centreY + shape.halfHeight)));
    const double keep = 1 - depth;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const double dx = (x - centreX) / shape.halfWidth, dy = (y - centreY) / shape.halfHeight;
            if (dx * dx + dy * dy > 1) continue;
            const size_t i = static_cast<size_t>(y) * frame.width + x;
            switch (frame.format)
            {
            case PixelFormat::Mono8:
                frame.data[i] = static_cast<uint8_t>(std::lround(frame.data[i] * keep));
                break;
            case PixelFormat::Mono16:
            {
                const int value = frame.data[2 * i] | (frame.data[2 * i + 1] << 8);
                const auto scaled = static_cast<uint16_t>(std::lround(value * keep));
                frame.data[2 * i] = static_cast<uint8_t>(scaled & 255);
                frame.data[2 * i + 1] = static_cast<uint8_t>(scaled >> 8);
                break;
            }
            case PixelFormat::RGB888:
                for (int c = 0; c < 3; ++c) frame.data[3 * i + c] = static_cast<uint8_t>(std::lround(frame.data[3 * i + c] * keep));
                break;
            }
        }
}

// One way of changing the recording: none for the control, or one shape
// crossing in a number of frames at a depth. Each has a detector per K.
struct Variant
{
    int shape{-1};
    int frames{0};
    double depth{0};
    std::vector<Crossing> crossings;
    // Per K, per frame: new rising and falling candidates. Since Feature-45 a
    // detector with darkening tests no rise, so the control's rises are
    // counted by detectors of their own, without darkening.
    std::vector<std::unique_ptr<FlashDetector>> detectors;
    std::vector<std::unique_ptr<FlashDetector>> risingDetectors;
    std::vector<std::vector<std::pair<int, int>>> candidates;

    // The silhouette's centre on frame i, or none.
    [[nodiscard]] bool centreAt(int i, int width, double& x, double& y) const
    {
        for (const Crossing& crossing : crossings)
        {
            if (i < crossing.first || i > crossing.last) continue;
            const Shape& s = kShapes[shape];
            // The middle of each frame's stretch of the path, so every frame of
            // even a three-frame crossing shows some of the silhouette.
            const double t = (i - crossing.first + 0.5) / frames;
            const double travel = width + 2.0 * s.halfWidth;
            x = crossing.leftToRight ? -s.halfWidth + t * travel : width + s.halfWidth - t * travel;
            y = crossing.row;
            return true;
        }
        return false;
    }
};

std::vector<double> sigmaList(const QString& text)
{
    std::vector<double> sigmas;
    for (const QString& part : text.split(','))
    {
        bool ok = false;
        const double sigma = part.toDouble(&ok);
        DetectionSettings settings;
        settings.sigma = sigma;
        if (!ok || !settings.valid()) throw std::invalid_argument("not a K value: " + part.toStdString());
        sigmas.push_back(sigma);
    }
    return sigmas;
}

} // namespace

int runDarkeningProbe(const QStringList& args)
{
    if (args.size() != 5 && args.size() != 6)
        throw std::invalid_argument("usage: SeleneWatchProbe darkening INPUT OUTPUT.json K[,K...] [FRAME_LIMIT]");
    const std::vector<double> sigmas = sigmaList(args[4]);
    std::string error;
    auto reader = openRecordingReader(std::filesystem::path(args[2].toStdWString()), 30, error);
    if (!reader) throw std::runtime_error(error);
    const int count = args.size() == 6 ? std::min(reader->frameCount(), args[5].toInt()) : reader->frameCount();
    if (count < 2) throw std::runtime_error("the recording needs at least two frames");
    QFile report(args[3]);
    if (!report.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("cannot create a new output file: " + args[3].toStdString());

    const auto start = reader->frameTime(0);
    std::vector<double> seconds;
    for (int i = 0; i < count; ++i) seconds.push_back(std::chrono::duration<double>(reader->frameTime(i) - start).count());

    // The first frame gives the layout for the schedule.
    auto probe = std::make_shared<Frame>();
    probe->data.resize(reader->frameBytes());
    if (!reader->readFrame(0, *probe, error)) throw std::runtime_error(error);
    const int width = probe->width, height = probe->height;

    std::vector<Variant> variants;
    variants.push_back({});
    for (int shape = 0; shape < static_cast<int>(std::size(kShapes)); ++shape)
        for (const int frames : kCrossingFrames)
            for (const double depth : kDepths)
                variants.push_back({shape, frames, depth, schedule(seconds, frames, height), {}, {}, {}});
    for (Variant& variant : variants)
        for (const double sigma : sigmas)
        {
            DetectionSettings settings;
            settings.sigma = sigma;
            if (variant.shape < 0) variant.risingDetectors.push_back(std::make_unique<FlashDetector>(settings));
            settings.darkening = true;
            variant.detectors.push_back(std::make_unique<FlashDetector>(settings));
            variant.candidates.emplace_back();
        }

    QTextStream console(stdout);
    for (int i = 0; i < count; ++i)
    {
        auto original = std::make_shared<Frame>();
        original->data.resize(reader->frameBytes());
        if (!reader->readFrame(i, *original, error)) throw std::runtime_error(error);
        original->frameIndex = reader->frameNumber(i);
        original->timestamp = reader->frameTime(i);
        for (Variant& variant : variants)
        {
            std::shared_ptr<Frame> frame = original;
            double x = 0, y = 0;
            if (variant.shape >= 0 && variant.centreAt(i, width, x, y))
            {
                frame = std::make_shared<Frame>(*original);
                darken(*frame, x, y, kShapes[variant.shape], variant.depth);
            }
            for (size_t k = 0; k < sigmas.size(); ++k)
            {
                // Each detector counts its own candidates: the darkening one
                // falls, the rising one (the control's only) rises.
                const auto count = [&frame](FlashDetector& detector) {
                    detector.analyze(*frame);
                    const int found = static_cast<int>(detector.candidates().size());
                    detector.updateReference(*frame);
                    return found;
                };
                const int falls = count(*variant.detectors[k]);
                const int rises = variant.risingDetectors.empty() ? 0 : count(*variant.risingDetectors[k]);
                variant.candidates[k].push_back({rises, falls});
            }
        }
        if (i % 100 == 0) console << "frame " << i << " of " << count << "\n" << Qt::flush;
    }

    console << "\nfile=" << args[2] << "\nframes=" << count << " seconds=" << seconds.back()
            << " size=" << width << "x" << height << " " << pixelFormatName(probe->format) << "\n";
    QJsonArray levels;
    for (size_t k = 0; k < sigmas.size(); ++k)
    {
        // The unchanged recording after start-up.
        int controlRises = 0, controlFalls = 0;
        for (int i = 0; i < count; ++i)
        {
            if (seconds[static_cast<size_t>(i)] < kStartupSeconds) continue;
            controlRises += variants[0].candidates[k][static_cast<size_t>(i)].first;
            controlFalls += variants[0].candidates[k][static_cast<size_t>(i)].second;
        }
        console << QString("K=%1 control: rising %2, falling %3 after %4 s\n")
            .arg(sigmas[k]).arg(controlRises).arg(controlFalls).arg(kStartupSeconds);
        QJsonArray injections;
        for (size_t v = 1; v < variants.size(); ++v)
        {
            const Variant& variant = variants[v];
            int fired = 0, controlInWindows = 0, fallsInWindows = 0, fallsOutside = 0;
            std::vector<bool> inWindow(static_cast<size_t>(count), false);
            for (const Crossing& crossing : variant.crossings)
            {
                bool hit = false;
                bool controlHit = false;
                // A window is the crossing and the frame after it.
                for (int i = crossing.first; i <= std::min(count - 1, crossing.last + 1); ++i)
                {
                    inWindow[static_cast<size_t>(i)] = true;
                    const int falls = variant.candidates[k][static_cast<size_t>(i)].second;
                    fallsInWindows += falls;
                    hit = hit || falls > 0;
                    controlHit = controlHit || variants[0].candidates[k][static_cast<size_t>(i)].second > 0;
                }
                fired += hit ? 1 : 0;
                controlInWindows += controlHit ? 1 : 0;
            }
            for (int i = 0; i < count; ++i)
                if (!inWindow[static_cast<size_t>(i)] && seconds[static_cast<size_t>(i)] >= kStartupSeconds)
                    fallsOutside += variant.candidates[k][static_cast<size_t>(i)].second;
            const Shape& shape = kShapes[variant.shape];
            console << QString("K=%1 %2 (%3x%4 px) in %5 frames, depth %6: %7 of %8 crossings fired (control windows %9); "
                               "falls in windows %10, outside %11\n")
                .arg(sigmas[k]).arg(shape.name).arg(2 * shape.halfWidth).arg(2 * shape.halfHeight).arg(variant.frames)
                .arg(variant.depth).arg(fired).arg(variant.crossings.size()).arg(controlInWindows).arg(fallsInWindows).arg(fallsOutside);
            injections.append(QJsonObject{{"shape", shape.name}, {"widthPixels", 2 * shape.halfWidth},
                {"heightPixels", 2 * shape.halfHeight}, {"crossingFrames", variant.frames}, {"depth", variant.depth},
                {"crossings", static_cast<int>(variant.crossings.size())}, {"crossingsFired", fired},
                {"controlWindowsWithFall", controlInWindows}, {"fallCandidatesInWindows", fallsInWindows},
                {"fallCandidatesOutsideWindows", fallsOutside}});
        }
        levels.append(QJsonObject{{"sigma", sigmas[k]}, {"controlRisingCandidates", controlRises},
            {"controlFallingCandidates", controlFalls}, {"injections", injections}});
    }

    const QJsonDocument document(QJsonObject{{"input", args[2]}, {"frames", count}, {"streamSeconds", seconds.back()},
        {"width", width}, {"height", height}, {"format", pixelFormatName(probe->format)},
        {"frameTimeMeaning", QString::fromStdString(reader->describe().frameTime)},
        {"frameRate", QString::fromStdString(reader->describe().frameRate)},
        {"startupSeconds", kStartupSeconds}, {"firstCrossingSeconds", kFirstCrossingSeconds},
        {"crossingSpacingSeconds", kCrossingSpacingSeconds},
        {"crossing", "an ellipse whose light is scaled by 1 - depth in every channel; horizontal, alternating direction, "
                     "on the middle, upper-third and lower-third rows in turn; its centre at the middle of each frame's stretch "
                     "of the path; a window is the crossing and the frame after"},
        {"scope", "the application's default detector settings except K, with darkening on and no large darkening "
                  "(the control's rising candidates from a detector without darkening); "
                  "a fresh detector per variant and K from the first frame; no evidence written"},
        {"levels", levels}});
    const QByteArray bytes = document.toJson();
    if (report.write(bytes) != bytes.size() || !report.flush()) throw std::runtime_error("cannot write the report");
    return 0;
}
