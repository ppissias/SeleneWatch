#include "recording/event_writer.h"
#include "recording/ser_reader.h"
#include "recording/ser_writer.h"
#include "recording/recording_reader.h"
#include "session.h"
#include "sources/recording_source.h"
#include "tools/detection_probe.h"

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QFontDatabase>

#include <exception>
#include <filesystem>
#include <memory>

// Manual checks against external recordings. No archive path is compiled
// into the tests. Output always goes to the explicitly supplied directory.
int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const int font = QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR", "C:/Windows") + "/Fonts/segoeui.ttf");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 9));
    QTextStream out(stdout), err(stderr);
    const QStringList args = app.arguments();
    if (args.size() < 3)
    {
        err << "Usage: SeleneWatchProbe capture INPUT OUTPUT | sample INPUT OUTPUT | write-benchmark INPUT OUTPUT | fixtures OUTPUT | crash OUTPUT | survey LIST OUTPUT | profile INPUT OUTPUT.json [CONFIG.json] | measure INPUT CSV [CONFIG.json] | measure-batch INPUT OUTPUT CONFIG.json | register INPUT OUTPUT [CONFIG.json]\n";
        return 2;
    }
    const QString mode = args[1];
    if (mode == "survey" || mode == "measure" || mode == "measure-batch" || mode == "profile" || mode == "register")
    {
        try { return runDetectionProbe(args); }
        catch (const std::exception& ex) { err << ex.what() << '\n'; return 1; }
    }
    QObject context;
    if (mode == "capture" && args.size() == 4)
    {
        QElapsedTimer timer;
        timer.start();
        std::vector<ClipWriteResult> results;
        uint64_t frames = 0;
        {
            Session::Notifications notices;
            notices.ended = [&] { app.quit(); };
            notices.failed = [&](const QString& reason) { err << reason << "\n"; app.exit(1); };
            notices.clipWritten = [&](const ClipWriteResult& result) { results.push_back(result); };
            Session session(&context, args[3], std::move(notices));
            auto source = std::make_unique<RecordingSource>(std::filesystem::path(args[2].toStdWString()));
            source->setSpeed(RecordingSource::Speed::Fast);
            session.setSource(std::move(source));
            QString error;
            if (!session.start(error)) { err << error << "\n"; return 1; }
            if (app.exec() != 0) return 1;
            frames = session.acquiredFrames();
            session.stop();
            out << "source_frames=" << frames << " source_and_collection_ms=" << timer.elapsed() << "\n";
        }
        QCoreApplication::processEvents();
        out << "total_including_writer_ms=" << timer.elapsed() << " clips=" << results.size() << "\n";
        for (const auto& result : results)
        {
            out << "clip=" << result.folder << " error=" << result.notSaved << " record_failed=" << result.recordFailed << "\n";
            if (!result.notSaved.isEmpty() || result.recordFailed) return 1;
        }
        return 0;
    }

    const QString output = args.back();
    std::vector<EventClip> clips;
    if ((mode == "sample" || mode == "write-benchmark") && args.size() == 4)
    {
        std::string error;
        auto reader = openRecordingReader(std::filesystem::path(args[2].toStdWString()), 30, error);
        if (!reader)
        { err << QString::fromStdString(error) << "\n"; return 1; }
        EventClip clip;
        clip.stream = reader->describe();
        clip.stream.source = args[2].toStdString();
        clip.stream.playback = "TEST: six nonconsecutive format-check samples, not a detected event";
        const bool benchmark = mode == "write-benchmark";
        if (benchmark) clip.stream.playback = "TEST: repeat one native frame 128 times for buffered writer throughput; indices/times repeat, not an observation";
        for (int i = 0; i < (benchmark ? 128 : 6); ++i)
        {
            if (benchmark && i > 0) { clip.frames.push_back(clip.frames.front()); continue; }
            const int position = i * (reader->frameCount() - 1) / 5;
            auto frame = std::make_shared<Frame>();
            frame->data.resize(reader->frameBytes());
            if (!reader->readFrame(position, *frame, error)) return 1;
            frame->frameIndex = reader->frameNumber(position);
            frame->timestamp = reader->frameTime(position);
            frame->testSource = true;
            clip.frames.push_back(frame);
        }
        clip.triggers.push_back({clip.frames[1]->frameIndex, clip.frames[2]->frameIndex,
            clip.frames[2]->timestamp, 0, false});
        clips.push_back(std::move(clip));
    }
    else if (mode == "fixtures")
    {
        for (int kind = 0; kind < 5; ++kind)
        {
            EventClip clip;
            clip.stream.source = "constructed format check";
            clip.stream.camera = "none";
            clip.stream.playback = "TEST: constructed format fixture, not a detected event";
            for (int n = 0; n < 3; ++n)
            {
                auto frame = std::make_shared<Frame>();
                frame->width = 32; frame->height = 24;
                frame->format = kind == 0 ? PixelFormat::Mono8 : (kind == 4 ? PixelFormat::RGB888 : PixelFormat::Mono16);
                frame->significantBits = kind == 1 ? 16 : (kind == 2 || kind == 3 ? 12 : 8);
                frame->alignment = kind == 3 ? BitAlignment::High : BitAlignment::Low;
                frame->bayer = kind == 3 ? BayerPattern::RGGB : BayerPattern::None;
                frame->frameIndex = static_cast<uint64_t>(n);
                frame->testSource = true;
                frame->timestamp = std::chrono::system_clock::time_point{} + std::chrono::seconds{1'700'000'000 + kind}
                    + std::chrono::milliseconds{n * 33};
                for (int p = 0; p < 32 * 24; ++p)
                {
                    if (kind >= 1 && kind <= 3)
                    {
                        const uint16_t value = static_cast<uint16_t>((kind == 1 ? (p * 73 + n) % 65536 : (p * 13 + n) % 4096)
                            << (kind == 3 ? 4 : 0));
                        frame->data.push_back(static_cast<uint8_t>(value & 255));
                        frame->data.push_back(static_cast<uint8_t>(value >> 8));
                    }
                    else
                        for (int c = 0; c < (kind == 4 ? 3 : 1); ++c)
                            frame->data.push_back(static_cast<uint8_t>((p * 7 + n + c * 19) % 256));
                }
                clip.frames.push_back(frame);
            }
            clip.triggers.push_back({1, 2, clip.frames[2]->timestamp, 0, false});
            clips.push_back(std::move(clip));
        }
    }
    else if (mode == "crash")
    {
        auto frame = std::make_shared<Frame>();
        frame->width = frame->height = 4096;
        frame->format = PixelFormat::Mono16;
        frame->significantBits = 16;
        frame->testSource = true;
        frame->timestamp = std::chrono::system_clock::now();
        frame->data.resize(frameBytes(frame->width, frame->height, frame->format), 1);
        EventClip clip;
        clip.stream.source = "TEST: constructed interrupted-write check";
        clip.frames.assign(64, frame); // 32 MB in RAM; long disk write for the kill check.
        clip.triggers.push_back({0, 0, frame->timestamp, 0, false});
        clips.push_back(std::move(clip));
    }
    else { err << "Unknown mode or invalid arguments\n"; return 2; }

    std::vector<ClipWriteResult> results;
    QElapsedTimer timer;
    timer.start();
    {
        EventWriter writer(&context, output, size_t{2} * 1024 * 1024 * 1024, [&](const ClipWriteResult& result) { results.push_back(result); });
        for (auto& clip : clips) writer.submit(std::move(clip));
        writer.shutdown();
        const auto usage = writer.usage();
        out << "ser_bytes=" << usage.writtenBytes << " ser_seconds=" << usage.writeSeconds
            << " ser_MB_per_second=" << (usage.writeSeconds > 0 ? usage.writtenBytes / usage.writeSeconds / 1e6 : 0) << "\n";
    }
    QCoreApplication::processEvents();
    out << "write_and_shutdown_ms=" << timer.elapsed() << "\n";
    for (const auto& result : results)
    {
        out << "clip=" << result.folder << " error=" << result.notSaved << "\n";
        if (!result.notSaved.isEmpty() || result.recordFailed) return 1;
    }
    return 0;
}
