#pragma once

#include "frame.h"
#include "stream_info.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

// A recording file read frame by frame: a SER or an AVI file. The reader
// knows the format; RecordingSource plays it.
//
// Every frame has a time: the one stored in the file, or a nominal one
// counted from a start time at a rate. describe() says which.
class RecordingReader
{
public:
    virtual ~RecordingReader() = default;

    // Frames that can be read, numbered by position from 0.
    [[nodiscard]] virtual int frameCount() const = 0;

    // Bytes of one frame in memory. Acquire frames of this size for readFrame.
    [[nodiscard]] virtual size_t frameBytes() const = 0;

    // The frame's number in the recording, from 0. It is the position for a
    // file that stores every frame. An AVI marks a frame dropped during
    // capture with an empty chunk, which leaves a gap in the numbers
    // (Evidence rule 4).
    [[nodiscard]] virtual uint64_t frameNumber(int index) const = 0;

    [[nodiscard]] virtual std::chrono::system_clock::time_point frameTime(int index) const = 0;

    // What the file says about its stream: the camera and its settings,
    // what its frame times and indices mean, and what was done to the
    // samples on the way in. The source and the playback are left to the caller.
    [[nodiscard]] virtual StreamInfo describe() const = 0;

    // Reads one frame: its size, format, bits, and pixels. The frame's index
    // and times are left to the caller. frame.data must hold frameBytes()
    // bytes. False, with the reason, on a read error.
    virtual bool readFrame(int index, Frame& frame, std::string& error) = 0;
};
