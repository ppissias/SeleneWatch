#include "tools/detection_probe.h"

#include "blip_injector.h"
#include "event_capture.h"
#include "preview_image.h"
#include "recording/event_writer.h"
#include "recording/recording_reader.h"
#include "recording/ser_writer.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTextStream>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>

namespace {

double valueAt(const Frame& frame, int x, int y)
{
    const size_t i = static_cast<size_t>(y) * frame.width + x;
    if (frame.format == PixelFormat::Mono8) return frame.data[i];
    if (frame.format == PixelFormat::RGB888)
        return (frame.data[3 * i] + frame.data[3 * i + 1] + frame.data[3 * i + 2]) / 3.0;
    const int shift = frame.alignment == BitAlignment::High ? 16 - frame.significantBits : 0;
    return (frame.data[2 * i] | (frame.data[2 * i + 1] << 8)) >> shift;
}

bool writeJson(const QString& path, const QJsonDocument& document)
{
    QFile file(path);
    const QByteArray bytes = document.toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

int survey(const QString& listPath, const QString& output)
{
    QFile list(listPath);
    if (!list.open(QIODevice::ReadOnly | QIODevice::Text) || !QDir().mkpath(output)) return 1;
    const QStringList paths = QString::fromUtf8(list.readAll()).split('\n', Qt::SkipEmptyParts);
    QJsonArray records;
    int id = 0;
    for (const auto& raw : paths)
    {
        const QString path = raw.trimmed();
        if (path.isEmpty()) continue;
        QJsonObject row{{"id", id++}, {"path", path}};
        std::string error;
        auto reader = openRecordingReader(std::filesystem::path(path.toStdWString()), 30, error);
        if (!reader) { row["error"] = QString::fromStdString(error); records.append(row); continue; }
        Frame frame;
        frame.data.resize(reader->frameBytes());
        if (!reader->readFrame(0, frame, error)) { row["error"] = QString::fromStdString(error); records.append(row); continue; }
        row["width"] = frame.width; row["height"] = frame.height;
        row["format"] = pixelFormatName(frame.format); row["bits"] = frame.significantBits;
        row["alignment"] = frame.alignment == BitAlignment::High ? "high" : "low";
        row["frames"] = reader->frameCount();
        row["bytesPerFrame"] = static_cast<double>(reader->frameBytes());
        row["frameTime"] = QString::fromStdString(reader->describe().frameTime);
        row["rate"] = QString::fromStdString(reader->describe().frameRate);
        const QString thumbnail = QString("%1.png").arg(row["id"].toInt(), 3, 10, QChar('0'));
        QImage image = frameToPreviewImage(frame).scaled(320, 240, Qt::KeepAspectRatio);
        if (!image.save(QDir(output).filePath(thumbnail))) return 1;
        row["thumbnail"] = thumbnail;
        records.append(row);
    }
    if (!writeJson(QDir(output).filePath("manifest.json"), QJsonDocument(records))) return 1;
    QTextStream(stdout) << "Surveyed " << records.size() << " recordings into " << output << "\n";
    return 0;
}

struct CachedRecording
{
    std::vector<FramePtr> frames;
    StreamInfo info;
    size_t frameBytes{0};
    int start{0};
};

std::optional<CachedRecording> load(const QString& input, const QJsonObject& config)
{
    QTextStream err(stderr);
    std::string reason;
    auto reader = openRecordingReader(std::filesystem::path(input.toStdWString()), 30, reason);
    if (!reader) { err << QString::fromStdString(reason) << "\n"; return {}; }
    const int start = std::max(0, config.value("startFrame").toInt(0));
    const int budgetFrames = static_cast<int>(std::min(size_t{5000}, (size_t{512} * 1024 * 1024) / reader->frameBytes()));
    const int count = std::min({config.value("frameLimit").toInt(300), reader->frameCount() - start, budgetFrames});
    if (count <= 0) { err << "No frames in requested range / memory budget\n"; return {}; }
    CachedRecording cached;
    cached.info = reader->describe();
    cached.info.source = input.toStdString();
    cached.frameBytes = reader->frameBytes();
    cached.start = start;
    for (int i = 0; i < count; ++i)
    {
        auto frame = std::make_shared<Frame>();
        frame->data.resize(reader->frameBytes());
        if (!reader->readFrame(start + i, *frame, reason)) { err << QString::fromStdString(reason); return {}; }
        frame->frameIndex = reader->frameNumber(start + i);
        frame->timestamp = reader->frameTime(start + i);
        cached.frames.push_back(std::move(frame));
    }
    return cached;
}

int measure(const QString& input, const QString& output, const QJsonObject& config, const CachedRecording& cached)
{
    QTextStream err(stderr);
    const auto& frames = cached.frames;
    const int count = static_cast<int>(frames.size());
    const int start = cached.start;
    const DetectionSettings settings{config.value("sigma").toDouble(6), config.value("minPixels").toInt(8),
        config.value("maxPixels").toInt(400), config.value("oneFrame").toBool(true)};
    if (!settings.valid()) { err << "Invalid trigger settings\n"; return 1; }
    const int x = config.value("x").toInt(frames[0]->width / 2);
    const int y = config.value("y").toInt(frames[0]->height / 2);
    if (x < 0 || y < 0 || x >= frames[0]->width || y >= frames[0]->height) return 1;
    const int injectionAt = config.value("atFrame").toInt(180);
    const int duration = config.value("duration").toInt(1);
    const double amplitude = config.value("amplitude").toDouble(0);
    const double amplitudeSigma = config.value("amplitudeSigma").toDouble(0);
    const bool inject = amplitude > 0 || amplitudeSigma > 0;
    if (inject && (injectionAt < 1 || injectionAt + duration > count || duration <= 0))
    { err << "Injection is outside the cached prefix\n"; return 1; }
    if (!QDir().mkpath(QFileInfo(output).absolutePath()) || QFileInfo(input).absoluteFilePath() == QFileInfo(output).absoluteFilePath()) return 1;
    QFile csv(output);
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
    QTextStream rows(&csv);
    rows << "position,source_index,utc_ser_ticks,noise_floor_adu,probe_noise_adu,peak_sigma,patch_pixels,passed,triggered,injected,actual_centre_addition_adu,processing_ms,peak_x,peak_y,box_x,box_y,box_width,box_height\n";
    const QString eventRoot = output + ".events";
    QObject context;
    std::vector<ClipWriteResult> written;
    std::unique_ptr<EventWriter> writer;
    if (config.value("saveEvents").toBool(false))
        writer = std::make_unique<EventWriter>(&context, eventRoot, 2000, [&](const auto& result) { written.push_back(result); });
    StreamInfo info = cached.info;
    info.playback = "manual detection calibration over an explicitly selected prefix";
    if (writer) writer->logSessionStart(info);
    int current = 0, candidates = 0, background = 0, injectedFrames = 0;
    bool recovered = false;
    QJsonArray candidatePositions, candidatePatches;
    EventCapture capture(150, [&] {
        ++candidates; candidatePositions.append(current);
    }, [&](EventClip clip) { if (writer) { clip.stream = info; writer->submit(std::move(clip)); } }, settings);
    BlipInjector blip;
    std::vector<double> timings;
    double beforeNoise = 0, duringNoise = 0, requestedAmplitude = 0, peakDuring = 0, actualAddition = 0;
    double captureMs = 0, originalCentre = 0;
    const QString mode = config.value("overlay").toString("none");
    const DetectionOverlay overlay = mode == "sigma" ? DetectionOverlay::Sigma : (mode == "noise" ? DetectionOverlay::Noise : DetectionOverlay::None);
    const int mapEvery = std::max(1, config.value("mapEvery").toInt(3));
    for (int i = 0; i < count; ++i)
    {
        current = i;
        FramePtr frame = frames[static_cast<size_t>(i)];
        if (inject && i == injectionAt)
        {
            beforeNoise = capture.detectionSummary().probeNoise;
            originalCentre = valueAt(*frame, x, y);
            requestedAmplitude = amplitudeSigma > 0 ? amplitudeSigma * beforeNoise : amplitude;
            if (!blip.request({x, y, config.value("fwhm").toDouble(3), requestedAmplitude, duration})) return 1;
        }
        double addition = 0;
        if (blip.busy())
        {
            auto changed = std::make_shared<Frame>(*frame);
            const double oldValue = valueAt(*frame, x, y);
            blip.apply(*changed);
            addition = valueAt(*changed, x, y) - oldValue;
            frame = std::move(changed);
        }
        QElapsedTimer timer;
        timer.start();
        capture.onFrame(frame, {overlay, x, y, overlay != DetectionOverlay::None && i % mapEvery == 0});
        captureMs += timer.nsecsElapsed() / 1.0e6;
        const auto& summary = capture.detectionSummary();
        if (summary.triggered)
        {
            // Match to a peak pixel actually brightened by the injection.
            // At a saturated centre the passing patch may lie to one side.
            const bool atBlip = frame->blip
                && valueAt(*frame, summary.peakX, summary.peakY)
                    > valueAt(*frames[static_cast<size_t>(i)], summary.peakX, summary.peakY);
            recovered = recovered || atBlip;
            if (!atBlip) ++background;
            candidatePatches.append(QJsonObject{{"position", i}, {"peakX", summary.peakX}, {"peakY", summary.peakY},
                {"peakSigma", summary.peakSigma}, {"pixels", summary.brightPixels}, {"atBlip", atBlip}});
        }
        timings.push_back(summary.processingMs);
        if (frame->blip)
        {
            ++injectedFrames;
            duringNoise = std::max(duringNoise, summary.probeNoise);
            peakDuring = std::max(peakDuring, summary.peakSigma);
            actualAddition = std::max(actualAddition, addition);
        }
        rows << i << ',' << frame->frameIndex << ',' << timeToSerTicks(frame->timestamp) << ',' << summary.noiseFloor << ','
            << summary.probeNoise << ',' << summary.peakSigma << ',' << summary.brightPixels << ','
            << summary.passed << ',' << summary.triggered << ',' << frame->blip.has_value() << ',' << addition << ',' << summary.processingMs
            << ',' << summary.peakX << ',' << summary.peakY << ',' << summary.boxX << ',' << summary.boxY
            << ',' << summary.boxWidth << ',' << summary.boxHeight << '\n';
    }
    capture.finishPartial();
    if (writer) { writer->shutdown(); QCoreApplication::processEvents(); }
    rows.flush();
    const bool csvWritten = rows.status() == QTextStream::Ok && csv.flush();
    csv.close();
    if (!csvWritten) return 1;
    for (const auto& result : written) if (result.recordFailed || !result.notSaved.isEmpty()) return 1;
    const double mean = std::accumulate(timings.begin(), timings.end(), 0.0) / count;
    std::sort(timings.begin(), timings.end());
    const double seconds = std::chrono::duration<double>(frames.back()->timestamp - frames.front()->timestamp).count();
    QJsonObject report{{"input", input}, {"frames", count}, {"startFrame", start}, {"width", frames[0]->width},
        {"height", frames[0]->height}, {"format", pixelFormatName(frames[0]->format)}, {"significantBits", frames[0]->significantBits},
        {"frameTimeMeaning", QString::fromStdString(info.frameTime)}, {"settings", QJsonObject{{"sigma", settings.sigma},
        {"minPixels", settings.minPixels}, {"maxPixels", settings.maxPixels}, {"oneFrame", settings.oneFrame}}},
        {"x", x}, {"y", y}, {"candidates", candidates}, {"backgroundCandidates", background}, {"candidatePositions", candidatePositions},
        {"candidatePatches", candidatePatches}, {"originalCentreAdu", originalCentre},
        {"streamSeconds", seconds}, {"backgroundCandidatesPerMinute", seconds > 0 ? background * 60 / seconds : 0},
        {"requestedAmplitudeAdu", requestedAmplitude}, {"actualCentreAdditionAdu", actualAddition},
        {"probeNoiseBeforeAdu", beforeNoise}, {"probeNoiseDuringAdu", duringNoise}, {"injectedFrames", injectedFrames},
        {"recovered", recovered}, {"selectedPeakSigmaDuringInjection", peakDuring},
        {"meanDetectorMs", mean}, {"p95DetectorMs", timings[static_cast<size_t>(count - 1) * 95 / 100]},
        {"maxDetectorMs", timings.back()}, {"meanCaptureMs", captureMs / count}, {"overlay", mode},
        {"savedClips", static_cast<int>(written.size())}, {"caseName", config.value("name").toString()},
        {"cacheBytes", static_cast<double>(cached.frameBytes) * count}};
    if (!writeJson(output + ".json", QJsonDocument(report))) return 1;
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}

} // namespace

int runDetectionProbe(const QStringList& args)
{
    if (args.size() == 4 && args[1] == "survey") return survey(args[2], args[3]);
    if (args.size() >= 4 && (args[1] == "measure" || args[1] == "measure-batch"))
    {
        QJsonObject config;
        if (args.size() >= 5)
        {
            QFile file(args[4]);
            if (!file.open(QIODevice::ReadOnly)) return 1;
            const auto document = QJsonDocument::fromJson(file.readAll());
            if (!document.isObject()) return 1;
            config = document.object();
        }
        if (args[1] == "measure")
        {
            auto cached = load(args[2], config);
            return cached ? measure(args[2], args[3], config, *cached) : 1;
        }
        const auto common = config.value("sourceConfig").toObject();
        auto cached = load(args[2], common);
        if (!cached || !QDir().mkpath(args[3])) return 1;
        const auto cases = config.value("cases").toArray();
        if (cases.isEmpty()) return 2;
        int index = 0;
        for (const auto& value : cases)
        {
            if (!value.isObject()) return 2;
            QJsonObject settings = common;
            const auto item = value.toObject();
            for (auto at = item.begin(); at != item.end(); ++at) settings[at.key()] = at.value();
            if (settings.value("startFrame").toInt() != common.value("startFrame").toInt()
                || settings.value("frameLimit").toInt(300) != common.value("frameLimit").toInt(300)) return 2;
            QString name = settings.value("name").toString(QString("case-%1").arg(index));
            for (int i = 0; i < name.size(); ++i)
                if (!name[i].isLetterOrNumber() && name[i] != '_' && name[i] != '-') name[i] = '_';
            const QString output = QDir(args[3]).filePath(QString("%1-%2.csv").arg(index++, 3, 10, QChar('0')).arg(name));
            if (measure(args[2], output, settings, *cached) != 0) return 1;
        }
        return 0;
    }
    return 2;
}
