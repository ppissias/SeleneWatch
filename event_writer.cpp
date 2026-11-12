#include "event_writer.h"

#include "frame_image.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMetaObject>
#include <QtCore/QTextStream>
#include <QtCore/QTimeZone>

#include <exception>

EventWriter::EventWriter(QObject* context, Finished finished)
    : context_(context)
    , finished_(std::move(finished))
{
    thread_ = std::jthread([this](std::stop_token stopToken) {
        run(stopToken);
    });
}

EventWriter::~EventWriter()
{
    shutdown();
}

void EventWriter::submit(EventClip clip)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(clip));
    }
    ready_.notify_one();
}

void EventWriter::shutdown()
{
    if (!thread_.joinable())
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        thread_.request_stop();
    }
    ready_.notify_all();
    thread_.join();
}

void EventWriter::deliver(const QString& path, const QString& error)
{
    if (context_ == nullptr || !finished_)
    {
        return;
    }

    QMetaObject::invokeMethod(context_, [done = finished_, path, error] {
        done(path, error);
    }, Qt::QueuedConnection);
}

void EventWriter::run(std::stop_token stopToken)
{
    try
    {
        for (;;)
        {
            EventClip clip;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [&] {
                    return stopToken.stop_requested() || !queue_.empty();
                });
                if (queue_.empty())
                {
                    return;
                }
                clip = std::move(queue_.front());
                queue_.pop_front();
            }

            QString error;
            const QString path = writeClip(clip, error);
            deliver(path, error);
        }
    }
    catch (const std::exception& ex)
    {
        try
        {
            deliver({}, QString::fromUtf8(ex.what()));
        }
        catch (...)
        {
        }
    }
    catch (...)
    {
        try
        {
            deliver({}, QString("Event writer failed"));
        }
        catch (...)
        {
        }
    }
}

namespace {

QString utcStamp(qint64 epochMs)
{
    return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC).toString("yyyyMMdd-HHmmss-zzz");
}

QString utcText(std::chrono::system_clock::time_point time)
{
    const qint64 epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

} // namespace

QString EventWriter::writeClip(const EventClip& clip, QString& error)
{
    const qint64 triggerMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        clip.triggerTime.time_since_epoch()).count();
    const QString dirPath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QString("events/%1").arg(utcStamp(triggerMs)));

    if (!QDir().mkpath(dirPath))
    {
        error = QString("Could not create %1").arg(dirPath);
        return {};
    }

    const QDir dir(dirPath);
    QFile note(dir.filePath("event.txt"));
    if (!note.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        error = QString("Could not write event.txt in %1").arg(dirPath);
        return dirPath;
    }

    {
        QTextStream out(&note);
        out << "onset_frame_index: " << clip.onsetFrameIndex << "\n";
        out << "trigger_frame_index: " << clip.triggerFrameIndex << "\n";
        out << "trigger_time: " << utcText(clip.triggerTime) << "\n";
        out << "delta_threshold: " << clip.deltaThreshold << "\n";
        out << "min_bright_pixels: " << clip.minBrightPixels << "\n";
        out << "bright_pixel_count: " << clip.brightPixelCount << "\n";
        out << "frames_before: " << clip.framesBefore << "\n";
        out << "frames_after: " << clip.framesAfter << "\n";
        out << "frames:\n";
        for (size_t i = 0; i < clip.frames.size(); ++i)
        {
            const QString name = QString("%1.png").arg(static_cast<int>(i), 4, 10, QChar('0'));
            out << name << " " << clip.frames[i].frameIndex << " " << utcText(clip.frames[i].timestamp) << "\n";
        }
    }
    note.close();

    for (size_t i = 0; i < clip.frames.size(); ++i)
    {
        const QString name = QString("%1.png").arg(static_cast<int>(i), 4, 10, QChar('0'));
        const QImage image = frameToQImage(clip.frames[i]);
        if (image.isNull() || !image.save(dir.filePath(name), "PNG"))
        {
            error = QString("Could not write %1").arg(dir.filePath(name));
            return dirPath;
        }
    }

    if (!note.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        error = QString("Could not finish event.txt in %1").arg(dirPath);
        return dirPath;
    }
    QTextStream done(&note);
    done << "complete: yes\n";
    return dirPath;
}
