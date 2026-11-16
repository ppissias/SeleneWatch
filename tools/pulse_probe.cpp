#include "tools/pulse_probe.h"

#include "flash_detector.h"
#include "guiding/guide_controller.h"
#include "recording/recording_reader.h"
#include "tools/translation_estimator.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

// Room for the largest trial: 1 px per frame for 1.5 s at 18 fps is 27 px.
constexpr int kMargin = 32;

// By then the detector's reference has learned for three of its time constants.
constexpr double kFirstPulseSeconds = 6.0;

// About the controller's shortest cycle: a pulse, the settle time, and a
// fresh estimate over 3 s.
constexpr double kPulseSpacingSeconds = 6.0;

// Guide speeds in image pixels per frame. The controller refuses a guide
// rate above 0.25; the two faster ones show what that limit prevents.
constexpr double kSpeeds[] = {0.05, 0.10, 0.25, 0.5, 1.0};

// A short pulse, and the longest pulse the controller sends.
constexpr double kDurations[] = {0.3, 1.5};

// One correction, in seconds from the first frame, and the way it moves the image.
struct Pulse
{
    double start{0};
    double end{0};
    int dx{0};
    int dy{0};
};

// The pulses of one duration that fit, with their settle time, into the stream.
std::vector<Pulse> schedule(double streamSeconds, double duration, double settleSeconds)
{
    static constexpr int kDirections[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    std::vector<Pulse> pulses;
    for (int p = 0;; ++p)
    {
        const double start = kFirstPulseSeconds + p * kPulseSpacingSeconds;
        if (start + duration + settleSeconds > streamSeconds) break;
        pulses.push_back({start, start + duration, kDirections[p % 4][0], kDirections[p % 4][1]});
    }
    return pulses;
}

// What the detector said about one frame of one trial, and where the crop was.
struct FrameRecord
{
    double seconds{0};
    int newCandidates{0};
    bool firing{false};
    double peakSigma{0};
    int dx{0};
    int dy{0};
};

// One detector following one crop. A speed of zero is the control.
struct Trial
{
    double sigma{0};
    double speed{0};
    double duration{0};
    std::vector<Pulse> pulses;
    std::unique_ptr<FlashDetector> detector;
    double offsetX{0};
    double offsetY{0};
    std::vector<FrameRecord> records;

    // Moves the crop for a frame at this time, if a pulse is under way.
    void advance(double seconds)
    {
        for (const Pulse& pulse : pulses)
        {
            if (seconds >= pulse.start && seconds < pulse.end)
            {
                offsetX += speed * pulse.dx;
                offsetY += speed * pulse.dy;
            }
        }
    }
};

// One trial's frames counted against a schedule of pulses. A window is a
// pulse and its settle time: what the writer marks "during mount
// correction". "Between" runs from a window's end to the next pulse.
struct Counts
{
    int pulses{0};
    int shiftPixels{0};
    int candidates{0};
    int beforePulses{0};
    int inWindows{0};
    int betweenWindows{0};
    int pulsesWithCandidate{0};
    int pulsesWithCandidateAfter{0};
    double latestCandidateAfterPulseEnd{0};
    int windowFrames{0};
    int windowFiringFrames{0};
    int betweenFrames{0};
    int betweenFiringFrames{0};
    double windowPeakSigma{0};
    double betweenPeakSigma{0};
};

Counts countAgainst(const std::vector<FrameRecord>& records, const std::vector<Pulse>& pulses, double settleSeconds)
{
    Counts c;
    c.pulses = static_cast<int>(pulses.size());
    std::vector<bool> fired(pulses.size()), firedAfter(pulses.size());
    for (const FrameRecord& r : records)
    {
        c.candidates += r.newCandidates;
        c.shiftPixels = std::max({c.shiftPixels, std::abs(r.dx), std::abs(r.dy)});

        // The latest pulse that has started by this frame.
        int p = -1;
        for (size_t k = 0; k < pulses.size() && pulses[k].start <= r.seconds; ++k) p = static_cast<int>(k);
        if (p < 0)
        {
            c.beforePulses += r.newCandidates;
        }
        else if (r.seconds <= pulses[p].end + settleSeconds)
        {
            c.inWindows += r.newCandidates;
            if (r.newCandidates > 0) fired[p] = true;
            ++c.windowFrames;
            c.windowFiringFrames += r.firing ? 1 : 0;
            c.windowPeakSigma = std::max(c.windowPeakSigma, r.peakSigma);
        }
        else
        {
            c.betweenWindows += r.newCandidates;
            if (r.newCandidates > 0)
            {
                firedAfter[p] = true;
                c.latestCandidateAfterPulseEnd = std::max(c.latestCandidateAfterPulseEnd, r.seconds - pulses[p].end);
            }
            ++c.betweenFrames;
            c.betweenFiringFrames += r.firing ? 1 : 0;
            c.betweenPeakSigma = std::max(c.betweenPeakSigma, r.peakSigma);
        }
    }
    c.pulsesWithCandidate = static_cast<int>(std::count(fired.begin(), fired.end(), true));
    c.pulsesWithCandidateAfter = static_cast<int>(std::count(firedAfter.begin(), firedAfter.end(), true));
    return c;
}

QJsonObject toJson(const Counts& c)
{
    return QJsonObject{{"pulses", c.pulses},
        {"shiftPixelsPerPulse", c.shiftPixels},
        {"candidates", c.candidates},
        {"candidatesBeforeFirstPulse", c.beforePulses},
        {"candidatesInWindows", c.inWindows},
        {"candidatesBetweenWindows", c.betweenWindows},
        {"pulsesWithCandidateInWindow", c.pulsesWithCandidate},
        {"pulsesWithCandidateAfterWindow", c.pulsesWithCandidateAfter},
        {"latestCandidateAfterPulseEndSeconds", c.latestCandidateAfterPulseEnd},
        {"windowFrames", c.windowFrames},
        {"windowFiringFrames", c.windowFiringFrames},
        {"betweenFrames", c.betweenFrames},
        {"betweenFiringFrames", c.betweenFiringFrames},
        {"windowPeakSigma", c.windowPeakSigma},
        {"betweenPeakSigma", c.betweenPeakSigma}};
}

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

int runGuidePulseProbe(const QStringList& args)
{
    if (args.size() != 5 && args.size() != 6)
        throw std::invalid_argument("usage: SeleneWatchProbe guide-pulses INPUT OUTPUT.json K[,K...] [FRAME_LIMIT]");
    const std::vector<double> sigmas = sigmaList(args[4]);

    std::string error;
    auto reader = openRecordingReader(std::filesystem::path(args[2].toStdWString()), 30, error);
    if (!reader) throw std::runtime_error(error);
    const int count = args.size() == 6 ? std::min(reader->frameCount(), args[5].toInt()) : reader->frameCount();
    if (count < 2) throw std::runtime_error("the recording needs at least two frames");

    // A new file only, so the recording can never be the output.
    QFile report(args[3]);
    if (!report.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        throw std::runtime_error("cannot create a new output file: " + args[3].toStdString());

    const auto first = reader->frameTime(0);
    const double streamSeconds = std::chrono::duration<double>(reader->frameTime(count - 1) - first).count();
    const double settleSeconds = GuideController::Settings{}.settleSeconds;

    std::vector<Trial> trials;
    for (const double sigma : sigmas)
    {
        DetectionSettings settings;
        settings.sigma = sigma;
        trials.push_back({sigma, 0, 0, {}, std::make_unique<FlashDetector>(settings)});
        for (const double duration : kDurations)
        {
            for (const double speed : kSpeeds)
            {
                trials.push_back({sigma, speed, duration, schedule(streamSeconds, duration, settleSeconds),
                    std::make_unique<FlashDetector>(settings)});
            }
        }
    }

    QTextStream console(stdout);
    int width = 0;
    int height = 0;
    QString format;
    for (int i = 0; i < count; ++i)
    {
        auto original = std::make_shared<Frame>();
        original->data.resize(reader->frameBytes());
        if (!reader->readFrame(i, *original, error)) throw std::runtime_error(error);
        original->frameIndex = reader->frameNumber(i);
        original->timestamp = reader->frameTime(i);
        const double seconds = std::chrono::duration<double>(original->timestamp - first).count();

        // Trials at the same offset share one crop of this frame.
        std::map<std::pair<int, int>, FramePtr> crops;
        for (Trial& trial : trials)
        {
            trial.advance(seconds);
            const int dx = static_cast<int>(std::lround(trial.offsetX));
            const int dy = static_cast<int>(std::lround(trial.offsetY));
            FramePtr& crop = crops[{dx, dy}];
            if (!crop) crop = registrationInterior(original, kMargin, dx, dy);
            trial.detector->analyze(*crop);
            trial.records.push_back({seconds, static_cast<int>(trial.detector->candidates().size()),
                trial.detector->firing(), trial.detector->summary().peakSigma, dx, dy});
            trial.detector->updateReference(*crop);
            width = crop->width;
            height = crop->height;
            format = pixelFormatName(crop->format);
        }
        if (i % 100 == 0) console << "frame " << i << " of " << count << "\n" << Qt::flush;
    }

    console << "\nfile=" << args[2] << "\nframes=" << count << " seconds=" << streamSeconds
            << " crop=" << width << "x" << height << " " << format << "\n"
            << "Columns: K, speed px/frame, pulse s, shift px per pulse, pulses, then for the trial (and the control in\n"
            << "the same windows): candidates in windows, between windows, before the first pulse; pulses with a\n"
            << "candidate in the window; pulses with one after it; peak sigma in windows and between.\n";

    QJsonArray levels;
    for (const double sigma : sigmas)
    {
        const auto control = std::find_if(trials.begin(), trials.end(),
            [sigma](const Trial& t) { return t.sigma == sigma && t.speed == 0; });
        QJsonArray rows;
        for (const Trial& trial : trials)
        {
            if (trial.sigma != sigma || trial.speed == 0) continue;
            const Counts t = countAgainst(trial.records, trial.pulses, settleSeconds);
            const Counts c = countAgainst(control->records, trial.pulses, settleSeconds);
            rows.append(QJsonObject{{"speedPixelsPerFrame", trial.speed}, {"pulseSeconds", trial.duration},
                {"trial", toJson(t)}, {"controlInSameWindows", toJson(c)}});
            console << QString("K=%1 speed=%2 pulse=%3 shift=%4 pulses=%5 | in %6 (%7) between %8 (%9) before %10 (%11) "
                               "| fired %12 (%13) after %14 (%15) | sigma win %16 (%17) between %18 (%19)\n")
                           .arg(sigma).arg(trial.speed, 0, 'f', 2).arg(trial.duration, 0, 'f', 1).arg(t.shiftPixels)
                           .arg(t.pulses).arg(t.inWindows).arg(c.inWindows).arg(t.betweenWindows).arg(c.betweenWindows)
                           .arg(t.beforePulses).arg(c.beforePulses).arg(t.pulsesWithCandidate).arg(c.pulsesWithCandidate)
                           .arg(t.pulsesWithCandidateAfter).arg(c.pulsesWithCandidateAfter)
                           .arg(t.windowPeakSigma, 0, 'f', 1).arg(c.windowPeakSigma, 0, 'f', 1)
                           .arg(t.betweenPeakSigma, 0, 'f', 1).arg(c.betweenPeakSigma, 0, 'f', 1);
        }
        levels.append(QJsonObject{{"sigma", sigma}, {"controlCandidates", countAgainst(control->records, {}, settleSeconds).candidates},
            {"trials", rows}});
    }

    const QJsonDocument document(QJsonObject{{"input", args[2]}, {"frames", count}, {"streamSeconds", streamSeconds},
        {"cropWidth", width}, {"cropHeight", height}, {"format", format}, {"marginPixels", kMargin},
        {"frameTimeMeaning", QString::fromStdString(reader->describe().frameTime)},
        {"frameRate", QString::fromStdString(reader->describe().frameRate)},
        {"firstPulseSeconds", kFirstPulseSeconds}, {"pulseSpacingSeconds", kPulseSpacingSeconds},
        {"settleSeconds", settleSeconds}, {"directions", "+x, +y, -x, -y in turn"},
        {"scope", "whole-pixel crop offsets, no resampling; the application's default detector settings except K; "
                  "a fresh detector per trial from the first frame; no evidence written"},
        {"levels", levels}});
    const QByteArray bytes = document.toJson();
    if (report.write(bytes) != bytes.size() || !report.flush()) throw std::runtime_error("cannot write the report");
    return 0;
}
