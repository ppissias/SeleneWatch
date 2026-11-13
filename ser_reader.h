#pragma once

#include "firecapture_log.h"
#include "frame.h"
#include "recording_reader.h"

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
//
// A file without a trailer gets nominal frame times: the header's start
// time plus the frame's position at the fallback rate, since SER stores no
// frame rate. FireCapture's log beside the file, if there is one, supplies
// the exposure and the gain.
class SerReader final : public RecordingReader
{
public:
    // Reads the header and the trailer and looks at the first frames for the
    // significant bits. fallbackFps times the frames of a file without a
    // trailer. False, with the reason, for a file that cannot be played.
    bool open(const std::filesystem::path& path, double fallbackFps, std::string& error);

    // Frames that can be read. Fewer than the header names when the file is short.
    [[nodiscard]] int frameCount() const override;

    // The frame count the header names.
    [[nodiscard]] int headerFrameCount() const;

    [[nodiscard]] size_t frameBytes() const override;

    // The position: a SER file stores every frame it numbers.
    [[nodiscard]] uint64_t frameNumber(int index) const override;

    // True when the trailer holds a time for every frame.
    [[nodiscard]] bool hasFrameTimes() const;

    // The trailer's time for a frame, or a nominal time without a trailer.
    [[nodiscard]] std::chrono::system_clock::time_point frameTime(int index) const override;

    // The header's DateTime_UTC. Empty when the file leaves it at zero.
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> startTime() const;

    // The header's Instrument field, usually the camera. May be empty.
    [[nodiscard]] const std::string& instrument() const;

    [[nodiscard]] StreamInfo describe() const override;

    // Reads one frame: its size, format, bits, Bayer pattern, and pixels.
    bool readFrame(int index, Frame& frame, std::string& error) override;

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
    double fallbackFps_{30.0};
    std::optional<FireCaptureLog> log_;
};

// SER and .NET time: 100 ns ticks since 0001-01-01 00:00 UTC.
[[nodiscard]] std::chrono::system_clock::time_point serTicksToTime(int64_t ticks);
