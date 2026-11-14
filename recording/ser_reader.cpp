#include "recording/ser_reader.h"

#include "stream_info.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <span>
#include <utility>

namespace {

// The header, as documented. Every number in it is little-endian.
constexpr size_t kHeaderBytes = 178;
constexpr char kFileId[] = "LUCAM-RECORDER";
constexpr size_t kFileIdBytes = 14;
constexpr size_t kColorIdAt = 18;
constexpr size_t kLittleEndianAt = 22;
constexpr size_t kWidthAt = 26;
constexpr size_t kHeightAt = 30;
constexpr size_t kPixelDepthAt = 34;
constexpr size_t kFrameCountAt = 38;
constexpr size_t kInstrumentAt = 82;
constexpr size_t kTextFieldBytes = 40;
constexpr size_t kDateTimeUtcAt = 170;

// ColorID values.
constexpr int kMono = 0;
constexpr int kBayerRggb = 8;
constexpr int kBayerGrbg = 9;
constexpr int kBayerGbrg = 10;
constexpr int kBayerBggr = 11;
constexpr int kRgb = 100;
constexpr int kBgr = 101;

// Frames looked at to find where a 16-bit file's significant bits sit.
constexpr int kFramesInspected = 4;

// 0001-01-01 to 1970-01-01, in 100 ns ticks.
constexpr int64_t kTicksAtUnixEpoch = 621'355'968'000'000'000;

int32_t readInt32(const unsigned char* bytes)
{
    const uint32_t value = static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
    return static_cast<int32_t>(value);
}

int64_t readInt64(const unsigned char* bytes)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
    {
        value = (value << 8) | bytes[i];
    }
    return static_cast<int64_t>(value);
}

// A fixed-width text field: up to the first NUL, without trailing spaces.
std::string readText(const unsigned char* bytes, size_t width)
{
    std::string text(reinterpret_cast<const char*>(bytes), width);
    text = text.substr(0, text.find('\0'));
    while (!text.empty() && text.back() == ' ')
    {
        text.pop_back();
    }
    return text;
}

uint16_t sampleAt(std::span<const uint8_t> data, size_t sample)
{
    return static_cast<uint16_t>(data[2 * sample] | (data[2 * sample + 1] << 8));
}

} // namespace

std::chrono::system_clock::time_point serTicksToTime(int64_t ticks)
{
    using Ticks = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(Ticks(ticks - kTicksAtUnixEpoch)));
}

bool SerReader::open(const std::filesystem::path& path, double fallbackFps, std::string& error)
{
    pathText_ = utf8Path(path);
    testSource_ = false;
    fallbackFps_ = std::max(1.0, fallbackFps);

    std::error_code sizeError;
    const uint64_t fileBytes = std::filesystem::file_size(path, sizeError);
    if (sizeError)
    {
        error = "cannot read " + pathText_ + ": " + sizeError.message();
        return false;
    }

    file_.open(path, std::ios::binary);
    if (!file_)
    {
        error = "cannot open " + pathText_;
        return false;
    }

    if (fileBytes < kHeaderBytes)
    {
        error = pathText_ + " is not a SER file: it is shorter than the 178-byte header";
        return false;
    }

    unsigned char header[kHeaderBytes];
    if (!file_.read(reinterpret_cast<char*>(header), kHeaderBytes))
    {
        error = "cannot read the header of " + pathText_;
        return false;
    }

    if (std::memcmp(header, kFileId, kFileIdBytes) != 0)
    {
        error = pathText_ + " is not a SER file: it does not start with LUCAM-RECORDER";
        return false;
    }

    const int colorId = readInt32(header + kColorIdAt);
    const int littleEndianFlag = readInt32(header + kLittleEndianAt);
    width_ = readInt32(header + kWidthAt);
    height_ = readInt32(header + kHeightAt);
    const int depth = readInt32(header + kPixelDepthAt);
    headerFrameCount_ = readInt32(header + kFrameCountAt);
    instrument_ = readText(header + kInstrumentAt, kTextFieldBytes);
    testSource_ = readText(header + 42, kTextFieldBytes).starts_with("SeleneWatch TEST DATA");
    startTicks_ = readInt64(header + kDateTimeUtcAt);

    if (width_ <= 0 || height_ <= 0)
    {
        error = pathText_ + " has an invalid frame size";
        return false;
    }
    if (depth < 1 || depth > 16)
    {
        error = pathText_ + " has an invalid PixelDepthPerPlane of " + std::to_string(depth);
        return false;
    }
    if (headerFrameCount_ < 0)
    {
        error = pathText_ + " has an invalid frame count";
        return false;
    }

    bool rgb = false;
    switch (colorId)
    {
    case kMono:
        bayer_ = BayerPattern::None;
        break;
    case kBayerRggb:
        bayer_ = BayerPattern::RGGB;
        break;
    case kBayerGrbg:
        bayer_ = BayerPattern::GRBG;
        break;
    case kBayerGbrg:
        bayer_ = BayerPattern::GBRG;
        break;
    case kBayerBggr:
        bayer_ = BayerPattern::BGGR;
        break;
    case kRgb:
    case kBgr:
        rgb = true;
        bgr_ = colorId == kBgr;
        break;
    default:
        error = pathText_ + " has colour ID " + std::to_string(colorId) + ", which is not supported";
        return false;
    }

    if (rgb && depth > 8)
    {
        error = pathText_ + " holds 16-bit colour, which is not supported";
        return false;
    }

    format_ = rgb ? PixelFormat::RGB888 : (depth > 8 ? PixelFormat::Mono16 : PixelFormat::Mono8);
    // The flag is used inverted in practice: 0 is little-endian data.
    bigEndian_ = depth > 8 && littleEndianFlag != 0;
    frameBytes_ = ::frameBytes(width_, height_, format_);

    // A file cut short plays the whole frames it holds, without a trailer.
    const uint64_t dataBytes = fileBytes - kHeaderBytes;
    const uint64_t wholeFrames = dataBytes / frameBytes_;
    frameCount_ = static_cast<int>(std::min<uint64_t>(wholeFrames, static_cast<uint64_t>(headerFrameCount_)));
    if (frameCount_ == 0)
    {
        error = pathText_ + " holds no complete frame";
        return false;
    }

    const uint64_t trailerAt = kHeaderBytes + static_cast<uint64_t>(headerFrameCount_) * frameBytes_;
    const uint64_t trailerBytes = static_cast<uint64_t>(headerFrameCount_) * sizeof(int64_t);
    if (frameCount_ == headerFrameCount_ && fileBytes >= trailerAt + trailerBytes)
    {
        std::vector<unsigned char> raw(trailerBytes);
        file_.seekg(static_cast<std::streamoff>(trailerAt));
        if (!file_.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size())))
        {
            error = "cannot read the trailer of " + pathText_;
            return false;
        }
        trailer_.resize(static_cast<size_t>(headerFrameCount_));
        for (size_t i = 0; i < trailer_.size(); ++i)
        {
            trailer_[i] = readInt64(raw.data() + i * sizeof(int64_t));
        }
    }

    if (format_ == PixelFormat::Mono16)
    {
        if (!inspectSignificantBits(depth, error))
        {
            return false;
        }
    }
    else
    {
        significantBits_ = format_ == PixelFormat::Mono8 ? depth : 8;
        alignment_ = BitAlignment::Low;
    }

    log_ = readFireCaptureLog(path);
    return true;
}

StreamInfo SerReader::describe() const
{
    StreamInfo info;
    info.camera = instrument_;
    if (log_)
    {
        info.exposure = log_->shutter;
        info.gain = log_->gain;
    }
    info.frameRate = "not stored in SER files";

    if (hasFrameTimes())
    {
        info.frameTime = "UTC from the file's trailer, as the capture program wrote it";
    }
    else
    {
        info.frameTime = std::format("nominal, not measured: {} plus the frame's position at {:.3f} fps, "
            "the rate set in the window",
            startTime() ? "the header's start time" : "an unknown start time counted from zero", fallbackFps_);
    }

    info.frameIndex = "the position in the file, from 0";
    if (frameCount_ < headerFrameCount_)
    {
        info.frameIndex += std::format("; the file holds {} of the {} frames its header names",
            frameCount_, headerFrameCount_);
    }

    if (bgr_)
    {
        info.conversion = "BGR turned into RGB; no value changed";
    }
    else if (bigEndian_)
    {
        info.conversion = "big-endian samples turned little-endian; no value changed";
    }
    else
    {
        info.conversion = "none";
    }
    if (testSource_) info.conversion += "; test data, marked by the originating SeleneWatch writer";
    return info;
}

// PixelDepthPerPlane cannot be relied on for 16-bit data. FireCapture
// writes 16 for 12-bit data in the top bits. So the samples of the first
// frames are combined: bits at the bottom of the word that are zero in all
// of them carry no data.
bool SerReader::inspectSignificantBits(int headerDepth, std::string& error)
{
    Frame scratch;
    scratch.data.resize(frameBytes_);
    uint16_t combined = 0;
    const int frames = std::min(frameCount_, kFramesInspected);
    const size_t samples = frameBytes_ / 2;
    for (int index = 0; index < frames; ++index)
    {
        if (!readRaw(index, scratch, error))
        {
            return false;
        }
        for (size_t sample = 0; sample < samples; ++sample)
        {
            combined |= sampleAt(scratch.data, sample);
        }
    }

    if (combined == 0)
    {
        // Black frames say nothing. The header is all there is.
        significantBits_ = headerDepth;
        alignment_ = BitAlignment::Low;
    }
    else if (headerDepth < 16 && (combined >> headerDepth) == 0)
    {
        // The data fits in the bits the header names, at the bottom.
        significantBits_ = headerDepth;
        alignment_ = BitAlignment::Low;
    }
    else
    {
        const int unusedLowBits = std::countr_zero(combined);
        significantBits_ = 16 - unusedLowBits;
        alignment_ = unusedLowBits > 0 ? BitAlignment::High : BitAlignment::Low;
    }
    return true;
}

int SerReader::frameCount() const
{
    return frameCount_;
}

int SerReader::headerFrameCount() const
{
    return headerFrameCount_;
}

size_t SerReader::frameBytes() const
{
    return frameBytes_;
}

uint64_t SerReader::frameNumber(int index) const
{
    return static_cast<uint64_t>(index);
}

bool SerReader::hasFrameTimes() const
{
    return !trailer_.empty();
}

std::chrono::system_clock::time_point SerReader::frameTime(int index) const
{
    if (hasFrameTimes())
    {
        return serTicksToTime(trailer_.at(static_cast<size_t>(index)));
    }
    const auto start = startTime().value_or(std::chrono::system_clock::time_point{});
    return start + std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::duration<double>(index / fallbackFps_));
}

std::optional<std::chrono::system_clock::time_point> SerReader::startTime() const
{
    if (startTicks_ == 0)
    {
        return std::nullopt;
    }
    return serTicksToTime(startTicks_);
}

const std::string& SerReader::instrument() const
{
    return instrument_;
}

bool SerReader::readFrame(int index, Frame& frame, std::string& error)
{
    if (!readRaw(index, frame, error))
    {
        return false;
    }
    frame.width = width_;
    frame.height = height_;
    frame.format = format_;
    frame.significantBits = significantBits_;
    frame.alignment = alignment_;
    frame.bayer = bayer_;
    frame.blip.reset();
    frame.syntheticFlash = false;
    frame.testSource = testSource_;
    return true;
}

// The stored bytes of one frame, as little-endian samples and RGB order.
bool SerReader::readRaw(int index, Frame& frame, std::string& error)
{
    if (index < 0 || index >= frameCount_)
    {
        error = "frame " + std::to_string(index) + " is outside " + pathText_;
        return false;
    }
    if (frame.data.size() != frameBytes_)
    {
        error = "a frame buffer of the wrong size was given for " + pathText_;
        return false;
    }

    const uint64_t offset = kHeaderBytes + static_cast<uint64_t>(index) * frameBytes_;
    file_.seekg(static_cast<std::streamoff>(offset));
    if (!file_.read(reinterpret_cast<char*>(frame.data.data()), static_cast<std::streamsize>(frameBytes_)))
    {
        file_.clear();
        error = "cannot read frame " + std::to_string(index) + " of " + pathText_;
        return false;
    }

    if (bigEndian_)
    {
        for (size_t i = 0; i + 1 < frame.data.size(); i += 2)
        {
            std::swap(frame.data[i], frame.data[i + 1]);
        }
    }
    if (bgr_)
    {
        for (size_t i = 0; i + 2 < frame.data.size(); i += 3)
        {
            std::swap(frame.data[i], frame.data[i + 2]);
        }
    }
    return true;
}
