#include "tools/overview_probe.h"

#include "recording/clip_overview.h"
#include "recording/recording_reader.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>

#include <filesystem>
#include <stdexcept>

int runOverviewProbe(const QStringList& arguments)
{
    QTextStream out(stdout);
    if (arguments.size() < 4 || arguments.size() > 7)
        throw std::invalid_argument("usage: SeleneWatchProbe overview INPUT OUTPUT.png [FIRST [COUNT [K]]]");
    std::string error;
    auto reader = openRecordingReader(std::filesystem::path(arguments[2].toStdWString()), 30, error);
    if (!reader) throw std::runtime_error(error);
    const int first = arguments.size() > 4 ? arguments[4].toInt() : 0;
    const int count = std::min(arguments.size() > 5 ? arguments[5].toInt() : 300, reader->frameCount() - first);
    if (first < 0 || count < 2) throw std::invalid_argument("FIRST and COUNT must select at least two frames of the recording");

    std::vector<FramePtr> frames;
    for (int position = first; position < first + count; ++position)
    {
        auto frame = std::make_shared<Frame>();
        frame->data.resize(reader->frameBytes());
        if (!reader->readFrame(position, *frame, error)) throw std::runtime_error(error);
        frame->frameIndex = reader->frameNumber(position);
        frame->timestamp = reader->frameTime(position);
        frames.push_back(std::move(frame));
    }

    OverviewRequest request;
    if (arguments.size() > 6) request.sigma = arguments[6].toDouble();
    request.caption << QString("Recording %1, frames %2 to %3 (positions %4 to %5); no triggers. Probe output, not a clip.")
        .arg(QFileInfo(arguments[2]).fileName()).arg(frames.front()->frameIndex).arg(frames.back()->frameIndex)
        .arg(first).arg(first + count - 1);
    QElapsedTimer timer;
    timer.start();
    const ClipOverview overview = makeClipOverview(frames, request);
    const qint64 elapsed = timer.elapsed();
    if (!overview.image.save(arguments[3], "PNG")) throw std::runtime_error("could not write the picture");
    out << "frames=" << frames.size() << " width=" << frames.front()->width << " height=" << frames.front()->height
        << " overview_ms=" << elapsed << " main_frame=" << overview.mainFrameIndex
        << " active_frames=" << overview.activeFrames << " changed_pixels=" << overview.changedPixels << "\n"
        << overview.activity << "\n";
    return 0;
}
