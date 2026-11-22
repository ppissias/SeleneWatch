#include "timelapse/sky_timelapse.h"

#include "recording/metadata_text.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStorageInfo>
#include <QtCore/QTextStream>
#include <QtGui/QImage>

#include <algorithm>
#include <cstring>

namespace {

// The frame's samples as an image that owns them, unchanged.
QImage samplesImage(const Frame& frame)
{
    const QImage::Format format = frame.format == PixelFormat::Mono16 ? QImage::Format_Grayscale16
        : frame.format == PixelFormat::RGB888 ? QImage::Format_RGB888 : QImage::Format_Grayscale8;
    QImage image(frame.width, frame.height, format);
    if (image.isNull()) return {};
    const size_t rowBytes = static_cast<size_t>(frame.width) * static_cast<size_t>(bytesPerPixel(frame.format));
    if (frame.data.size() < rowBytes * static_cast<size_t>(frame.height)) return {};
    // Mono16 is little-endian in the frame and in QImage on this platform.
    for (int y = 0; y < frame.height; ++y)
        std::memcpy(image.scanLine(y), frame.data.data() + rowBytes * static_cast<size_t>(y), rowBytes);
    return image;
}

const char* alignmentName(BitAlignment alignment)
{
    return alignment == BitAlignment::High ? "high" : "low";
}

const char* bayerName(BayerPattern bayer)
{
    switch (bayer)
    {
    case BayerPattern::None: return "none";
    case BayerPattern::RGGB: return "RGGB";
    case BayerPattern::GRBG: return "GRBG";
    case BayerPattern::GBRG: return "GBRG";
    case BayerPattern::BGGR: return "BGGR";
    }
    return "none";
}

bool appendLine(const QString& path, const QString& line)
{
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) return false;
    return file.write(line.toUtf8()) == line.toUtf8().size();
}

} // namespace

SkyTimelapse::SkyTimelapse(Ended ended)
    : ended_(std::move(ended))
    , thread_([this](std::stop_token stop) { run(stop); })
{
}

SkyTimelapse::~SkyTimelapse()
{
    finish("the program closed");
    waitIdle();
    thread_.request_stop();
    wake_.notify_all();
}

void SkyTimelapse::start(const QString& watchFolder, double intervalSeconds, uint64_t diskReserveBytes)
{
    std::lock_guard lock(mutex_);
    if (status_.active || intervalSeconds <= 0) return;
    status_ = {};
    status_.active = true;
    status_.folder = QDir(watchFolder).filePath("timelapse");
    status_.intervalSeconds = intervalSeconds;
    keeping_ = QDir().mkpath(status_.folder);
    if (!keeping_) status_.problem = "Could not create " + status_.folder;
    finishQueued_ = false;
    nextDue_.reset();
    reserveBytes_ = diskReserveBytes;
    started_ = std::chrono::system_clock::now();
}

void SkyTimelapse::offer(const Frame& frame)
{
    std::lock_guard lock(mutex_);
    if (!keeping_) return;
    const auto interval = std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::duration<double>(status_.intervalSeconds));
    // Not yet due, unless the source time jumped back (a recording that loops).
    if (nextDue_ && frame.timestamp < *nextDue_ && frame.timestamp + interval >= *nextDue_) return;
    nextDue_ = frame.timestamp + interval;
    const auto waiting = std::count_if(queue_.begin(), queue_.end(), [](const Item& item) { return item.frame != nullptr; });
    if (waiting >= 2)
    {
        ++status_.skipped;
        return;
    }
    // A copy with its own memory: the camera's frame goes back to its pool.
    auto copy = std::make_shared<Frame>();
    copy->width = frame.width;
    copy->height = frame.height;
    copy->format = frame.format;
    copy->significantBits = frame.significantBits;
    copy->alignment = frame.alignment;
    copy->bayer = frame.bayer;
    copy->frameIndex = frame.frameIndex;
    copy->timestamp = frame.timestamp;
    copy->steadyTimestamp = frame.steadyTimestamp;
    copy->blip = frame.blip;
    copy->syntheticFlash = frame.syntheticFlash;
    copy->testSource = frame.testSource;
    copy->data.assign(frame.data.begin(), frame.data.end());
    queue_.push_back({std::move(copy), std::nullopt});
    wake_.notify_all();
}

void SkyTimelapse::finish(const QString& reason)
{
    std::lock_guard lock(mutex_);
    if (!status_.active || finishQueued_) return;
    keeping_ = false;
    finishQueued_ = true;
    queue_.push_back({nullptr, reason});
    wake_.notify_all();
}

SkyTimelapse::Status SkyTimelapse::status() const
{
    std::lock_guard lock(mutex_);
    return status_;
}

void SkyTimelapse::waitIdle()
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void SkyTimelapse::run(std::stop_token stop)
{
    for (;;)
    {
        Item item;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        if (item.frame) write(*item.frame);
        else if (item.finish) close(*item.finish);
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
        }
        idle_.notify_all();
    }
}

void SkyTimelapse::write(const Frame& frame)
{
    QString folder;
    int number = 0;
    uint64_t reserve = 0;
    double interval = 0;
    {
        std::lock_guard lock(mutex_);
        folder = status_.folder;
        number = status_.frames + 1;
        reserve = reserveBytes_;
        interval = status_.intervalSeconds;
    }
    const auto stopKeeping = [this](const QString& why) {
        std::lock_guard lock(mutex_);
        keeping_ = false;
        status_.problem = why;
    };

    // The disk reserve, as for clips and recordings.
    QStorageInfo storage(folder);
    storage.refresh();
    const uint64_t needed = reserve + 2 * frameBytes(frame.width, frame.height, frame.format);
    if (storage.isValid() && static_cast<uint64_t>(storage.bytesAvailable()) < needed)
    {
        stopKeeping("No more frames: the free space fell below the disk reserve.");
        return;
    }

    const QString name = QString("frame_%1.png").arg(number, 5, 10, QChar('0'));
    const QImage samples = samplesImage(frame);
    if (samples.isNull() || !samples.save(QDir(folder).filePath(name), "PNG"))
    {
        stopKeeping("No more frames: " + name + " could not be written.");
        return;
    }
    const QString log = QDir(folder).filePath("frames.txt");
    if (number == 1)
        appendLine(log, QString("# SeleneWatch sky time-lapse (Feature-36): one frame kept every %1 s of source time.\n"
            "# The PNG samples are the camera's, unchanged: format=%2 significant_bits=%3 alignment=%4 bayer=%5.\n")
            .arg(interval, 0, 'g', 6).arg(pixelFormatName(frame.format)).arg(frame.significantBits)
            .arg(alignmentName(frame.alignment)).arg(bayerName(frame.bayer)));
    const bool test = frame.blip.has_value() || frame.syntheticFlash || frame.testSource;
    appendLine(log, QString("%1 utc=%2 frame=%3%4\n").arg(name, utcText(frame.timestamp)).arg(frame.frameIndex)
        .arg(test ? " contains_test_signal=yes" : ""));

    // The video: the preview's auto-stretch, its levels eased from frame to frame.
    if (!videoFailed_)
    {
        QString error;
        if (!video_.isOpen())
        {
            const int longest = std::max(frame.width, frame.height);
            const double scale = longest > kVideoLongEdge ? static_cast<double>(kVideoLongEdge) / longest : 1.0;
            const int width = static_cast<int>(std::lround(frame.width * scale));
            const int height = static_cast<int>(std::lround(frame.height * scale));
            if (!video_.open(QDir(folder).filePath("timelapse.mp4"), width, height, kVideoFps, error)) videoFailed_ = true;
            else
            {
                std::lock_guard lock(mutex_);
                status_.videoPath = QDir(folder).filePath("timelapse.mp4");
            }
        }
        if (!videoFailed_)
        {
            const AutoStretchCurve measured = computeAutoStretchCurve(frame);
            if (!curve_) curve_ = measured;
            else
            {
                curve_->black = 0.7 * curve_->black + 0.3 * measured.black;
                curve_->white = 0.7 * curve_->white + 0.3 * measured.white;
                curve_->midtones = 0.7 * curve_->midtones + 0.3 * measured.midtones;
            }
            const QImage shown = renderAutoStretchedFrame(frame, *curve_);
            if (shown.isNull() || !video_.write(shown, error))
            {
                videoFailed_ = true;
                if (error.isEmpty()) error = "a frame could not be stretched for display";
            }
        }
        if (videoFailed_)
        {
            std::lock_guard lock(mutex_);
            status_.problem = "No video: " + error + ". The frames are kept as PNG.";
            status_.videoPath.clear();
        }
    }
    std::lock_guard lock(mutex_);
    status_.frames = number;
}

void SkyTimelapse::close(const QString& reason)
{
    QString error;
    const bool hadVideo = video_.isOpen();
    if (hadVideo && !video_.close(error))
    {
        std::lock_guard lock(mutex_);
        status_.problem = "The video may be incomplete: " + error + ".";
    }
    videoFailed_ = false;
    curve_.reset();

    Status ended;
    std::chrono::system_clock::time_point started;
    {
        std::lock_guard lock(mutex_);
        status_.active = false;
        status_.endReason = reason;
        ended = status_;
        started = started_;
    }
    QFile record(QDir(ended.folder).filePath("timelapse.txt"));
    if (record.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    {
        QTextStream out(&record);
        out << "SeleneWatch sky time-lapse (Feature-36)\n"
            << "started: " << utcText(started) << "\n"
            << "ended: " << utcText(std::chrono::system_clock::now()) << "\n"
            << "end_reason: " << reason << "\n"
            << "interval_s: " << QString::number(ended.intervalSeconds, 'g', 6) << "\n"
            << "frames: " << ended.frames << "\n"
            << "skipped: " << ended.skipped << " (due while two frames still waited to be written)\n"
            << "frames_folder: PNG, the camera's samples unchanged; times in frames.txt\n"
            << "video: " << (ended.videoPath.isEmpty() ? QString("none")
                   : QString("timelapse.mp4, %1 fps, stretched for display like the preview's auto-stretch").arg(kVideoFps))
            << "\n";
        if (!ended.problem.isEmpty()) out << "problem: " << ended.problem << "\n";
    }
    if (ended_) ended_(ended);
}
