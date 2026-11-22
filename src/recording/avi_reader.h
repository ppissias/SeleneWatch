#pragma once

#include "recording/firecapture_log.h"
#include "recording/recording_reader.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

// Reads an uncompressed AVI file: its first video stream, frame by frame.
// It only reads; it never changes the file.
//
// It reads the kinds found in the observer's archive (Feature-6, AVI reader):
// - an 8-bit DIB with a grey palette, and Y800, as Mono8;
// - a 24-bit DIB as RGB888, and a 32-bit DIB as RGB888 without its unused
//   fourth byte;
// - YUY2 as Mono8 from its luma samples only. These pixels are derived, not
//   as stored, and describe() says so.
// A DIB is stored bottom-up unless its height is negative. Its rows are
// turned the right way up and BGR is turned into RGB; neither changes a
// value. A compressed stream, an 8-bit palette that is not a grey ramp, or
// a frame chunk of the wrong size is refused when the file is opened.
//
// An empty frame chunk marks a frame dropped during capture. It holds no
// image, so it is not played, but it keeps its place: the frames after it
// keep their numbers and their times, and the gap shows in the index.
//
// Frames are found through the file's index, so that opening a large file
// from a slow disk does not seek once per frame: the OpenDML index of a file
// in several RIFF parts, else the AVI 1.0 index (idx1), else by walking the
// movi lists. A walk stops at bytes that cannot be a chunk: a capture that
// stopped early can leave the rest of a reserved file as zeros, and those
// bytes are reported as not read.
//
// AVI stores no frame times. A frame's time is a start time plus its
// position at the frame rate. The start time comes from FireCapture's log
// beside the file; without one it is unknown and the times count from zero.
// The rate is the log's mean when the log gives its Duration to the
// millisecond and counts the frames the file holds: FireCapture writes the
// rate at the start of a capture, rounded, into the header, and the camera
// can fall far behind it. Otherwise the rate is the header's.
class AviReader final : public RecordingReader
{
public:
    // Reads the headers, the index, and FireCapture's log if there is one.
    // fallbackFps is used when the header states no frame rate. False, with
    // the reason, for a file that cannot be played.
    bool open(const std::filesystem::path& path, double fallbackFps, std::string& error);

    [[nodiscard]] int frameCount() const override;
    [[nodiscard]] size_t frameBytes() const override;
    [[nodiscard]] uint64_t frameNumber(int index) const override;
    [[nodiscard]] std::chrono::system_clock::time_point frameTime(int index) const override;
    [[nodiscard]] StreamInfo describe() const override;
    bool readFrame(int index, Frame& frame, std::string& error) override;

    // How the frames were found: "OpenDML index", "idx1", or "movi walk".
    [[nodiscard]] const std::string& indexUsed() const;

private:
    // Where the frame rate comes from.
    enum class RateSource
    {
        Header,
        FireCaptureLog,
        Window
    };

    // How a frame is stored in the file.
    enum class Layout
    {
        GreyDib,
        Y800,
        Bgr24,
        Bgra32,
        Yuy2
    };

    // A chunk: its id, its size, and where its data starts.
    struct Chunk
    {
        uint32_t id{0};
        uint32_t size{0};
        uint64_t dataAt{0};
    };

    // A frame chunk's data, as an index or the walk found it.
    struct FrameChunk
    {
        uint64_t dataAt{0};
        uint32_t size{0};
    };

    bool readAt(uint64_t offset, void* bytes, size_t count);
    bool readChunk(uint64_t at, uint64_t end, Chunk& chunk);
    void readTopLevel(uint64_t from, uint64_t to, bool firstPart);
    void readHeaderList(uint64_t from, uint64_t to);
    void readStreamList(uint64_t from, uint64_t to, int streamNumber);
    bool readFormat(std::string& error);

    bool findFramesInOpenDmlIndex(std::vector<FrameChunk>& frames);
    bool findFramesInIdx1(std::vector<FrameChunk>& frames);
    void findFramesInMovi(uint64_t from, uint64_t to, std::vector<FrameChunk>& frames, int depth);
    bool isFrameChunk(uint32_t id) const;
    bool keepFrames(const std::vector<FrameChunk>& frames, std::string& error);
    void chooseFrameRate(double fallbackFps);

    std::ifstream file_;
    std::string pathText_;
    uint64_t fileBytes_{0};

    // From the headers.
    uint32_t microSecPerFrame_{0};
    bool haveVideo_{false};
    uint32_t frameChunkDb_{0};
    uint32_t frameChunkDc_{0};
    uint32_t rate_{0};
    uint32_t scale_{0};
    std::vector<uint8_t> format_;
    std::vector<uint8_t> superIndex_;
    std::optional<uint64_t> firstMoviAt_;
    std::vector<std::pair<uint64_t, uint64_t>> moviLists_;
    std::optional<Chunk> idx1_;

    // The video stream.
    Layout layout_{Layout::GreyDib};
    int width_{0};
    int height_{0};
    bool bottomUp_{false};
    size_t stride_{0};
    size_t storedBytes_{0};
    double headerRate_{0.0};
    double frameRate_{0.0};
    RateSource rateSource_{RateSource::Header};
    // Why the log's duration was not used, when it has one.
    std::string logRateRefused_;

    // Where each frame's data starts, and its number among all frame chunks.
    std::vector<uint64_t> frames_;
    std::vector<uint64_t> numbers_;
    int frameChunks_{0};
    int emptyChunks_{0};
    int framesBeyondEnd_{0};
    // Bytes at the end of movi lists that a walk found to hold no chunk.
    uint64_t unreadMoviBytes_{0};
    std::string indexUsed_;
    std::optional<FireCaptureLog> log_;
    std::vector<uint8_t> raw_;
};
