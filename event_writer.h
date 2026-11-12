#pragma once

#include "event_clip.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// Writes finished clips on a background thread. The source thread only submits.
// An exception on the writer thread is reported through Finished, and the thread stops.
class EventWriter
{
public:
    using Finished = std::function<void(const QString& path, const QString& error)>;

    EventWriter(QObject* context, Finished finished);
    ~EventWriter();

    EventWriter(const EventWriter&) = delete;
    EventWriter& operator=(const EventWriter&) = delete;

    void submit(EventClip clip);
    void shutdown();

private:
    void run(std::stop_token stopToken);
    void deliver(const QString& path, const QString& error);
    static QString writeClip(const EventClip& clip, QString& error);

    QObject* context_;
    Finished finished_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<EventClip> queue_;
    std::jthread thread_;
};
