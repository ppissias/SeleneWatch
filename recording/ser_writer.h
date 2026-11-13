#pragma once

#include "frame.h"

#include <QtCore/QIODevice>
#include <QtCore/QString>

#include <cstdint>
#include <string>
#include <vector>

// Streams unchanged frame bytes and UTC trailer times to a seekable device.
// The frame count stays zero until finish succeeds. A caller closes and
// flushes its file before marking the enclosing clip complete.
//
// Mono16 samples always occupy two bytes. Low-aligned 9..15-bit samples
// name that depth; high-aligned samples name 16 to preserve their position.
// The sidecar records the exact significant bits and alignment.
class SerWriter
{
public:
    explicit SerWriter(QIODevice& output);
    bool start(const Frame& layout, const std::string& camera, QString& error, bool testSource = false);
    bool append(const Frame& frame, QString& error);
    bool finish(QString& error);

private:
    bool write(const char* bytes, qint64 count, QString& error);

    QIODevice& output_;
    int width_{0};
    int height_{0};
    PixelFormat format_{PixelFormat::Mono8};
    BayerPattern bayer_{BayerPattern::None};
    int bits_{0};
    BitAlignment alignment_{BitAlignment::Low};
    bool started_{false};
    bool failed_{false};
    bool finished_{false};
    bool testSource_{false};
    std::vector<int64_t> times_;
};

// UTC in 100 ns ticks since 0001-01-01, as stored in SER.
[[nodiscard]] int64_t timeToSerTicks(std::chrono::system_clock::time_point time);
