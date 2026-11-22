#pragma once

#include "core/frame.h"

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
    // camera goes into the header's Instrument field and telescope, when
    // given, into its Telescope field (Feature-14). The Observer field holds
    // SeleneWatch's sample-layout and test mark.
    bool start(const Frame& layout, const std::string& camera, QString& error, bool testSource = false,
        const std::string& telescope = {});
    bool append(const Frame& frame, QString& error);
    // A long recording (Feature-14): writes the count of frames appended so
    // far into the header, so a file whose finish never runs can still be
    // read up to here. It has no trailer of times then.
    bool updateCount(QString& error);
    bool finish(QString& error);
    [[nodiscard]] size_t frames() const { return times_.size(); }

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
