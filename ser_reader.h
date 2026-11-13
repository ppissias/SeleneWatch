#pragma once

#include "frame.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Reads a SER file: its header, any frame by number, and the frame times in
// its trailer. It only reads; it never changes the file.
//
// The layout and the quirks of real files are in the project description
// (SER details that affect correctness): a 178-byte header, the frames,
// then an optional trailer of one 64-bit UTC time per frame in 100 ns ticks
// since 0001-01-01. The LittleEndian flag is used inverted in practice: 0
// means little-endian data.
//
// Mono and raw Bayer at 8 or 16 bits, and 8-bit RGB and BGR, are read. BGR
// is turned into RGB and big-endian samples into little-endian; neither
// changes a value. Anything else is refused when the file is opened.
class SerReader
{
public:
    // Reads the header and the trailer and looks at the first frames for the
    // significant bits. False, with the reason, for a file that cannot be played.
    bool open(const std::filesystem::path& path, std::string& error);

    // Frames that can be read. Fewer than the header names when the file is short.
    [[nodiscard]] int frameCount() const;

    // The frame count the header names.
    [[nodiscard]] int headerFrameCount() const;

    // Bytes of one frame in memory. Acquire frames of this size for readFrame.
    [[nodiscard]] size_t frameBytes() const;

    // True when the trailer holds a time for every frame.
    [[nodiscard]] bool hasFrameTimes() const;

    // The trailer's time for a frame. Only when hasFrameTimes().
    [[nodiscard]] std::chrono::system_clock::time_point frameTime(int index) const;

    // The header's DateTime_UTC. Empty when the file leaves it at zero.
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> startTime() const;

    // The header's Instrument field, usually the camera. May be empty.
    [[nodiscard]] const std::string& instrument() const;

    // Reads one frame: its size, format, bits, Bayer pattern, and pixels.
    // The frame's index and times are left to the caller. frame.data must
    // hold frameBytes() bytes. False, with the reason, on a read error.
    bool readFrame(int index, Frame& frame, std::string& error);

private:
    bool readRaw(int index, Frame& frame, std::string& error);
    bool inspectSignificantBits(int headerDepth, std::string& error);

    std::ifstream file_;
    std::string pathText_;

    int width_{0};
    int height_{0};
    PixelFormat format_{PixelFormat::Mono8};
    BayerPattern bayer_{BayerPattern::None};
    int significantBits_{8};
    BitAlignment alignment_{BitAlignment::Low};
    bool bigEndian_{false};
    bool bgr_{false};

    int frameCount_{0};
    int headerFrameCount_{0};
    size_t frameBytes_{0};
    std::vector<int64_t> trailer_;
    int64_t startTicks_{0};
    std::string instrument_;
};

// SER and .NET time: 100 ns ticks since 0001-01-01 00:00 UTC.
[[nodiscard]] std::chrono::system_clock::time_point serTicksToTime(int64_t ticks);
