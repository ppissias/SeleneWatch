#include "tools/detection_probe.h"
#include "tools/translation_estimator.h"

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
#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <filesystem>

namespace {

bool sameFile(const QString& first, const QString& second)
{
    std::error_code error;
    return std::filesystem::equivalent(std::filesystem::path(first.toStdWString()), std::filesystem::path(second.toStdWString()), error);
}

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
    if (sameFile(input, output) || !QDir().mkpath(QFileInfo(output).absolutePath())) return 1;
    QFile csv(output);
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
    QTextStream rows(&csv);
    rows << "position,source_index,utc_ser_ticks,noise_floor_adu,probe_noise_adu,peak_sigma,patch_pixels,passed,triggered,injected,actual_centre_addition_adu,processing_ms,peak_x,peak_y,box_x,box_y,box_width,box_height\n";
    const QString eventRoot = output + ".events";
    QObject context;
    std::vector<ClipWriteResult> written;
    std::unique_ptr<EventWriter> writer;
    if (config.value("saveEvents").toBool(false))
        writer = std::make_unique<EventWriter>(&context, eventRoot, size_t{2} * 1024 * 1024 * 1024, [&](const auto& result) { written.push_back(result); });
    StreamInfo info = cached.info;
    info.playback = "manual detection calibration over an explicitly selected prefix";
    if (writer) writer->logSessionStart(info);
    int current = 0, candidates = 0, background = 0, injectedFrames = 0;
    bool recovered = false;
    QJsonArray candidatePositions, candidatePatches;
    EventCapture capture(CaptureSettings{}, [&] {
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

int profileDisplay(const QString& input, const QString& output, const CachedRecording& cached)
{
    if (sameFile(input, output) || !QDir().mkpath(QFileInfo(output).absolutePath())) return 1;
    QJsonArray modes;
    const auto& frames = cached.frames;
    for (auto mode : {DisplayMode::Linear, DisplayMode::Window, DisplayMode::Auto})
    {
        DisplaySettings settings; settings.mode = mode;
        settings.black = 0; settings.white = 0.1;
        std::vector<double> times;
        QImage image;
        for (int repeat = 0; repeat < 3; ++repeat) for (const auto& frame : frames)
        {
            QElapsedTimer timer; timer.start();
            image = frameToPreviewImage(*frame, settings);
            if (image.isNull()) return 1;
            times.push_back(timer.nsecsElapsed() / 1e6);
        }
        const double mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        std::sort(times.begin(), times.end());
        modes.append(QJsonObject{{"mode", mode == DisplayMode::Linear ? "linear" : mode == DisplayMode::Window ? "window" : "auto"},
            {"meanMs", mean}, {"p95Ms", times[(times.size() - 1) * 95 / 100]}, {"maxMs", times.back()}});
        if (!image.save(output + QString(".%1.png").arg(static_cast<int>(mode)))) return 1;
    }
    const QJsonDocument report(QJsonObject{{"input", input}, {"frames", static_cast<int>(frames.size())},
        {"width", frames[0]->width}, {"height", frames[0]->height}, {"format", pixelFormatName(frames[0]->format)},
        {"significantBits", frames[0]->significantBits}, {"results", modes}});
    if (!writeJson(output, report)) return 1;
    QTextStream(stdout) << report.toJson(QJsonDocument::Compact) << '\n';
    return 0;
}

// Compare the same native samples and detector settings over a common
// interior. These derived frames are never passed to the evidence writer.
int registrationExperiment(const QString& input, const QString& output, const QJsonObject& config, const CachedRecording& cached)
{
    if (!QDir().mkpath(output)) return 1;
    const QDir directory(output);
    for (const QString name : {"registration.json","translations.csv","reference.png","unaligned.png","aligned.png"})
        if (sameFile(input,directory.filePath(name))) return 1;
    const auto& frames = cached.frames;
    TranslationEstimator estimator(*frames.front(), config.value("maxShift").toInt(48));
    const int margin = estimator.margin();
    const int x = config.value("x").toInt(frames[0]->width/2), y = config.value("y").toInt(frames[0]->height/2);
    const int at = config.value("atFrame").toInt(std::min(180,static_cast<int>(frames.size())*3/4));
    const int duration = config.value("duration").toInt(1);
    const double fwhm = config.value("fwhm").toDouble(5);
    if (at < 1 || duration < 1 || static_cast<int64_t>(at)+duration > static_cast<int64_t>(frames.size())
        || !std::isfinite(fwhm) || fwhm <= 0 || x < margin || y < margin
        || x >= frames[0]->width-margin || y >= frames[0]->height-margin) return 1;
    std::vector<FrameTranslation> shifts;
    std::vector<double> shiftTimes;
    int accepted = 0, boundary = 0;
    QFile trace(directory.filePath("translations.csv"));
    if (!trace.open(QIODevice::WriteOnly|QIODevice::Text)) return 1;
    QTextStream rows(&trace);
    rows << "position,source_index,dx,dy,correlation,peak_gap,accepted,processing_ms\n";
    for (size_t i = 0; i < frames.size(); ++i)
    {
        QElapsedTimer timer; timer.start();
        const auto shift = estimator.estimate(*frames[i]);
        const double ms = timer.nsecsElapsed()/1e6;
        shifts.push_back(shift); shiftTimes.push_back(ms);
        accepted += shift.valid;
        boundary += std::max(std::abs(shift.dx),std::abs(shift.dy)) >= config.value("maxShift").toInt(48);
        rows << i << ',' << frames[i]->frameIndex << ',' << shift.dx << ',' << shift.dy << ',' << shift.correlation << ','
            << shift.peakGap << ',' << shift.valid << ',' << ms << '\n';
    }
    rows.flush(); if (rows.status()!=QTextStream::Ok || !trace.flush()) return 1; trace.close();
    const auto inspection = [&](const FramePtr& frame, const QString& name, const QString& label) {
        QImage image=frameToPreviewImage(*frame);
        QPainter painter(&image);
        painter.fillRect(0,0,image.width(),24,Qt::black); painter.setPen(Qt::white);
        painter.drawText(8,17,"Derived TEST crop: " + label); painter.end();
        return image.save(directory.filePath(name));
    };
    const auto& selected=shifts[static_cast<size_t>(at)];
    if (!inspection(registrationInterior(frames[0],margin,0,0),"reference.png","first-frame reference")
        || !inspection(registrationInterior(frames[static_cast<size_t>(at)],margin,0,0),"unaligned.png","current, unaligned")
        || !inspection(registrationInterior(frames[static_cast<size_t>(at)],margin,selected.dx,selected.dy),"aligned.png","current, integer aligned")) return 1;
    const double shiftMean = std::accumulate(shiftTimes.begin(),shiftTimes.end(),0.0)/shiftTimes.size();
    std::sort(shiftTimes.begin(),shiftTimes.end());
    const double seconds = std::chrono::duration<double>(frames.back()->timestamp-frames.front()->timestamp).count();
    QJsonArray cases;
    const auto levels = config.value("sigmaLevels").toArray(QJsonArray{6,24});
    const auto amplitudes = config.value("amplitudes").toArray(QJsonArray{8,16,32,64,128,192});
    if (levels.isEmpty()) return 1;
    for (const auto level : levels)
    {
        const double k = level.toDouble();
        for (const QString mode : {QString("full"),QString("cropped"),QString("aligned")})
        {
            const int trials = mode=="full" ? 1 : static_cast<int>(amplitudes.size())+1;
            for (int trial = 0; trial < trials; ++trial)
            {
                const double amplitude = trial==0 ? 0 : amplitudes[trial-1].toDouble();
                if (!std::isfinite(amplitude) || amplitude < 0 || amplitude > 65535) return 1;
                DetectionSettings settings{k,config.value("minPixels").toInt(8),config.value("maxPixels").toInt(400),true};
                if (!settings.valid()) return 1;
                EventCapture capture(CaptureSettings{}, [] {}, [](EventClip) {}, settings);
                BlipInjector blip;
                QJsonArray positions;
                int background = 0, passingFrames = 0, firingFrames = 0, runFrames = 0, longestRun = 0;
                bool matchedTrigger = false, matchedPass = false, priorRun = false;
                double addition = 0, noise = 0, pipeline = 0, detector = 0;
                for (size_t i = 0; i < frames.size(); ++i)
                {
                    const auto& shift = shifts[i];
                    FramePtr current = frames[i];
                    if (amplitude > 0 && static_cast<int>(i)==at)
                    {
                        priorRun = capture.detectionSummary().firing;
                        const int bx=x+shift.dx, by=y+shift.dy;
                        if (bx<margin || by<margin || bx>=frames[0]->width-margin || by>=frames[0]->height-margin) return 1;
                        if (!blip.request({bx,by,fwhm,amplitude,duration})) return 1;
                    }
                    QElapsedTimer timer; timer.start();
                    if (blip.busy())
                    {
                        auto changed=std::make_shared<Frame>(*current);
                        blip.apply(*changed); current=std::move(changed);
                        const auto& point=current->blip->settings;
                        addition=std::max(addition,valueAt(*current,point.x,point.y)-valueAt(*frames[i],point.x,point.y));
                    }
                    const bool aligned=mode=="aligned";
                    const int sx=aligned?shift.dx:0, sy=aligned?shift.dy:0;
                    const int border=mode=="full"?0:margin;
                    const FramePtr derived=mode=="full"?current:registrationInterior(current,margin,sx,sy);
                    const int px=x+shift.dx-border-sx, py=y+shift.dy-border-sy;
                    capture.onFrame(derived,{DetectionOverlay::None,px,py,false});
                    const auto& measured=capture.detectionSummary();
                    passingFrames += measured.passed; firingFrames += measured.firing;
                    runFrames = measured.firing ? runFrames+1 : 0;
                    longestRun = std::max(longestRun,runFrames);
                    detector += measured.processingMs;
                    pipeline += timer.nsecsElapsed()/1e6;
                    bool atBlip=false;
                    if (current->blip)
                    {
                        noise=std::max(noise,measured.probeNoise);
                        const int ox=measured.peakX+border+sx, oy=measured.peakY+border+sy;
                        atBlip=valueAt(*current,ox,oy)>valueAt(*frames[i],ox,oy);
                        matchedPass=matchedPass||(measured.passed&&atBlip);
                        matchedTrigger=matchedTrigger||(measured.triggered&&atBlip);
                    }
                    if (measured.triggered)
                    {
                        positions.append(static_cast<int>(i));
                        if (!atBlip) ++background;
                    }
                }
                capture.finishPartial();
                cases.append(QJsonObject{{"mode",mode},{"sigma",k},{"amplitudeAdu",amplitude},{"actualCentreAdditionAdu",addition},
                    {"candidates",positions.size()},{"backgroundCandidates",background},{"candidatePositions",positions},
                    {"passingFrames",passingFrames},{"firingFrames",firingFrames},{"longestFiringRunFrames",longestRun},
                    {"candidatesPerMinute",seconds>0?positions.size()*60/seconds:0},{"matchedTrigger",matchedTrigger},
                    {"matchedPassingPatch",matchedPass},{"priorRunActive",priorRun},{"probeNoiseDuringAdu",noise},
                    {"meanDetectorMs",detector/frames.size()},{"meanPipelineMs",pipeline/frames.size()}});
            }
        }
    }
    QJsonDocument report(QJsonObject{{"input",input},{"frames",static_cast<int>(frames.size())},{"startFrame",cached.start},
        {"width",frames[0]->width},{"height",frames[0]->height},{"format",pixelFormatName(frames[0]->format)},
        {"frameTimeMeaning",QString::fromStdString(cached.info.frameTime)},{"streamSeconds",seconds},{"cropMargin",margin},
        {"referenceX",x},{"referenceY",y},{"injectionPosition",at},{"duration",duration},{"fwhm",fwhm},
        {"acceptedTranslations",accepted},{"boundaryTranslations",boundary},{"meanTranslationMs",shiftMean},
        {"p95TranslationMs",shiftTimes[(shiftTimes.size()-1)*95/100]},{"maxTranslationMs",shiftTimes.back()},
        {"method","integer translation, fixed first-frame template, 8x8 box coarse NCC and sparse native refinement; invalid estimates use zero shift"},
        {"evidence","no event/evidence output; crops are derived TEST data only"},{"cases",cases}});
    if (!writeJson(directory.filePath("registration.json"),report)) return 1;
    QTextStream(stdout) << "Compared " << cases.size() << " cases over " << frames.size() << " frames; accepted " << accepted
        << " translations; mean " << shiftMean << " ms; output " << directory.filePath("registration.json") << '\n';
    return 0;
}

} // namespace

int runDetectionProbe(const QStringList& args)
{
    if (args.size() == 4 && args[1] == "survey") return survey(args[2], args[3]);
    if (args.size() >= 4 && (args[1] == "measure" || args[1] == "measure-batch" || args[1] == "profile" || args[1] == "register"))
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
        if (args[1] == "measure" || args[1] == "profile" || args[1] == "register")
        {
            if (args[1] == "profile" && !config.contains("frameLimit")) config["frameLimit"] = 12;
            if (args[1] == "register" && !config.contains("frameLimit")) config["frameLimit"] = 240;
            auto cached = load(args[2], config);
            if (!cached) return 1;
            if (args[1] == "register") return registrationExperiment(args[2],args[3],config,*cached);
            return args[1] == "profile" ? profileDisplay(args[2], args[3], *cached) : measure(args[2], args[3], config, *cached);
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
