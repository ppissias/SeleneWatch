#include "recording/avi_reader.h"

#include "stream_info.h"

#include <algorithm>
#include <cstdlib>
#include <format>

namespace {

constexpr uint32_t fourcc(const char (&text)[5])
{
    return static_cast<uint32_t>(static_cast<uint8_t>(text[0]))
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 8)
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[3])) << 24);
}

constexpr uint32_t kRiff = fourcc("RIFF");
constexpr uint32_t kList = fourcc("LIST");
constexpr uint32_t kAvi = fourcc("AVI ");
constexpr uint32_t kAvix = fourcc("AVIX");
constexpr uint32_t kHdrl = fourcc("hdrl");
constexpr uint32_t kAvih = fourcc("avih");
constexpr uint32_t kStrl = fourcc("strl");
constexpr uint32_t kStrh = fourcc("strh");
constexpr uint32_t kStrf = fourcc("strf");
constexpr uint32_t kIndx = fourcc("indx");
constexpr uint32_t kMovi = fourcc("movi");
constexpr uint32_t kRec = fourcc("rec ");
constexpr uint32_t kIdx1 = fourcc("idx1");
constexpr uint32_t kVids = fourcc("vids");
constexpr uint32_t kY800 = fourcc("Y800");
constexpr uint32_t kY8 = fourcc("Y8  ");
constexpr uint32_t kGrey = fourcc("GREY");
constexpr uint32_t kYuy2 = fourcc("YUY2");

// BITMAPINFOHEADER.biCompression for an uncompressed DIB.
constexpr uint32_t kBiRgb = 0;

// OpenDML index types (bIndexType).
constexpr uint8_t kIndexOfIndexes = 0;
constexpr uint8_t kIndexOfChunks = 1;

// The largest header or index chunk read whole.
constexpr uint32_t kMaxIndexChunk = 256u << 20;

// Lists nested deeper than this inside movi are not followed.
constexpr int kMaxDepth = 4;

uint16_t u16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t u32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

int32_t i32(const uint8_t* bytes)
{
    return static_cast<int32_t>(u32(bytes));
}

uint64_t u64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(u32(bytes)) | (static_cast<uint64_t>(u32(bytes + 4)) << 32);
}

// A compression code as the FourCC it usually is, or as a number.
std::string compressionText(uint32_t code)
{
    std::string text;
    for (int i = 0; i < 4; ++i)
    {
        const char c = static_cast<char>((code >> (8 * i)) & 0xFF);
        if (c < 32 || c > 126)
        {
            return "type " + std::to_string(code);
        }
        text += c;
    }
    return "'" + text + "'";
}

// "00db", "00dc", ... for stream number n.
uint32_t streamChunkId(int stream, char first, char second)
{
    return static_cast<uint32_t>('0' + stream / 10) | (static_cast<uint32_t>('0' + stream % 10) << 8)
        | (static_cast<uint32_t>(first) << 16) | (static_cast<uint32_t>(second) << 24);
}

} // namespace

bool AviReader::open(const std::filesystem::path& path, double fallbackFps, std::string& error)
{
    pathText_ = utf8Path(path);

    std::error_code sizeError;
    fileBytes_ = std::filesystem::file_size(path, sizeError);
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

    uint8_t riff[12];
    if (fileBytes_ < sizeof(riff) || !readAt(0, riff, sizeof(riff)) || u32(riff) != kRiff || u32(riff + 8) != kAvi)
    {
        error = pathText_ + " is not an AVI file: it does not start with a RIFF AVI header";
        return false;
    }

    // The first RIFF part, then the AVIX parts of an OpenDML file.
    uint64_t partEnd = std::min<uint64_t>(8 + static_cast<uint64_t>(u32(riff + 4)), fileBytes_);
    readTopLevel(sizeof(riff), partEnd, true);
    uint64_t at = partEnd + (partEnd & 1);
    while (at + sizeof(riff) <= fileBytes_)
    {
        uint8_t part[12];
        if (!readAt(at, part, sizeof(part)) || u32(part) != kRiff || u32(part + 8) != kAvix)
        {
            break;
        }
        partEnd = std::min<uint64_t>(at + 8 + static_cast<uint64_t>(u32(part + 4)), fileBytes_);
        readTopLevel(at + sizeof(part), partEnd, false);
        at = partEnd + (partEnd & 1);
    }

    if (!haveVideo_)
    {
        error = pathText_ + " has no video stream";
        return false;
    }
    if (!readFormat(error))
    {
        return false;
    }

    if (scale_ > 0 && rate_ > 0)
    {
        frameRate_ = static_cast<double>(rate_) / static_cast<double>(scale_);
        rateFromFile_ = true;
    }
    else if (microSecPerFrame_ > 0)
    {
        frameRate_ = 1'000'000.0 / static_cast<double>(microSecPerFrame_);
        rateFromFile_ = true;
    }
    else
    {
        frameRate_ = std::max(1.0, fallbackFps);
        rateFromFile_ = false;
    }

    // An idx1 covers only the first RIFF part, so a file of several parts
    // without an OpenDML index is walked.
    std::vector<FrameChunk> frames;
    if (findFramesInOpenDmlIndex(frames))
    {
        indexUsed_ = "OpenDML index";
    }
    else if (moviLists_.size() == 1 && findFramesInIdx1(frames))
    {
        indexUsed_ = "idx1";
    }
    else
    {
        frames.clear();
        for (const auto& [from, to] : moviLists_)
        {
            findFramesInMovi(from, to, frames, 0);
        }
        indexUsed_ = "movi walk";
    }

    if (!keepFrames(frames, error))
    {
        return false;
    }

    raw_.resize(storedBytes_);
    log_ = readFireCaptureLog(path);
    return true;
}

bool AviReader::readAt(uint64_t offset, void* bytes, size_t count)
{
    if (offset + count > fileBytes_)
    {
        return false;
    }
    file_.seekg(static_cast<std::streamoff>(offset));
    if (!file_.read(static_cast<char*>(bytes), static_cast<std::streamsize>(count)))
    {
        file_.clear();
        return false;
    }
    return true;
}

bool AviReader::readChunk(uint64_t at, uint64_t end, Chunk& chunk)
{
    uint8_t header[8];
    if (at + sizeof(header) > end || !readAt(at, header, sizeof(header)))
    {
        return false;
    }
    chunk.id = u32(header);
    chunk.size = u32(header + 4);
    chunk.dataAt = at + sizeof(header);
    return true;
}

void AviReader::readTopLevel(uint64_t from, uint64_t to, bool firstPart)
{
    uint64_t at = from;
    Chunk chunk;
    while (readChunk(at, to, chunk))
    {
        const uint64_t end = std::min<uint64_t>(chunk.dataAt + chunk.size, to);
        uint8_t type[4];
        if (chunk.id == kList && chunk.size >= sizeof(type) && readAt(chunk.dataAt, type, sizeof(type)))
        {
            if (u32(type) == kHdrl && firstPart)
            {
                readHeaderList(chunk.dataAt + sizeof(type), end);
            }
            else if (u32(type) == kMovi)
            {
                // idx1 offsets usually count from the 'movi' type field.
                if (!firstMoviAt_)
                {
                    firstMoviAt_ = chunk.dataAt;
                }
                moviLists_.emplace_back(chunk.dataAt + sizeof(type), end);
            }
        }
        else if (chunk.id == kIdx1 && firstPart)
        {
            idx1_ = chunk;
        }
        at = chunk.dataAt + chunk.size + (chunk.size & 1);
    }
}

void AviReader::readHeaderList(uint64_t from, uint64_t to)
{
    int streamNumber = 0;
    uint64_t at = from;
    Chunk chunk;
    while (readChunk(at, to, chunk))
    {
        const uint64_t end = std::min<uint64_t>(chunk.dataAt + chunk.size, to);
        uint8_t data[4];
        if (chunk.id == kAvih && chunk.size >= sizeof(data) && readAt(chunk.dataAt, data, sizeof(data)))
        {
            microSecPerFrame_ = u32(data);
        }
        else if (chunk.id == kList && chunk.size >= sizeof(data) && readAt(chunk.dataAt, data, sizeof(data))
            && u32(data) == kStrl)
        {
            readStreamList(chunk.dataAt + sizeof(data), end, streamNumber++);
        }
        at = chunk.dataAt + chunk.size + (chunk.size & 1);
    }
}

// The first video stream is the one played. Its header, its format, and its
// OpenDML super index are kept.
void AviReader::readStreamList(uint64_t from, uint64_t to, int streamNumber)
{
    std::vector<uint8_t> header;
    std::vector<uint8_t> format;
    std::vector<uint8_t> superIndex;

    uint64_t at = from;
    Chunk chunk;
    while (readChunk(at, to, chunk))
    {
        std::vector<uint8_t>* keep = chunk.id == kStrh ? &header
            : chunk.id == kStrf ? &format
            : chunk.id == kIndx ? &superIndex
            : nullptr;
        if (keep && chunk.size <= kMaxIndexChunk)
        {
            keep->resize(chunk.size);
            if (!readAt(chunk.dataAt, keep->data(), keep->size()))
            {
                keep->clear();
            }
        }
        at = chunk.dataAt + chunk.size + (chunk.size & 1);
    }

    // AVIStreamHeader: fccType at 0, dwScale at 20, dwRate at 24.
    if (haveVideo_ || header.size() < 28 || u32(header.data()) != kVids)
    {
        return;
    }
    haveVideo_ = true;
    scale_ = u32(header.data() + 20);
    rate_ = u32(header.data() + 24);
    format_ = std::move(format);
    superIndex_ = std::move(superIndex);
    frameChunkDb_ = streamChunkId(streamNumber, 'd', 'b');
    frameChunkDc_ = streamChunkId(streamNumber, 'd', 'c');
}

// BITMAPINFOHEADER: biSize at 0, biWidth at 4, biHeight at 8, biBitCount at
// 14, biCompression at 16, biClrUsed at 32. A palette follows biSize bytes in.
bool AviReader::readFormat(std::string& error)
{
    if (format_.size() < 40)
    {
        error = pathText_ + " has no video format";
        return false;
    }

    const uint8_t* f = format_.data();
    const uint32_t headerBytes = u32(f);
    width_ = i32(f + 4);
    const int32_t height = i32(f + 8);
    const uint16_t bits = u16(f + 14);
    const uint32_t compression = u32(f + 16);
    const uint32_t coloursUsed = u32(f + 32);

    if (width_ <= 0 || height == 0 || height == INT32_MIN)
    {
        error = pathText_ + " has an invalid frame size";
        return false;
    }
    height_ = std::abs(height);

    if (compression == kBiRgb)
    {
        // A DIB: rows padded to four bytes, bottom-up unless the height is negative.
        bottomUp_ = height > 0;
        stride_ = (static_cast<size_t>(width_) * bits + 31) / 32 * 4;
        if (bits == 8)
        {
            layout_ = Layout::GreyDib;
            const uint32_t entries = coloursUsed != 0 ? coloursUsed : 256;
            if (entries > 256 || headerBytes + 4ull * entries > format_.size())
            {
                error = pathText_ + " is an 8-bit DIB without a complete palette";
                return false;
            }
            for (uint32_t i = 0; i < entries; ++i)
            {
                const uint8_t* entry = f + headerBytes + 4 * i;
                if (entry[0] != i || entry[1] != i || entry[2] != i)
                {
                    error = pathText_ + " has an 8-bit palette that is not a grey ramp, so its bytes are not "
                        "the pixel values";
                    return false;
                }
            }
        }
        else if (bits == 24)
        {
            layout_ = Layout::Bgr24;
        }
        else if (bits == 32)
        {
            layout_ = Layout::Bgra32;
        }
        else
        {
            error = std::format("{} is a {}-bit DIB, which is not supported", pathText_, bits);
            return false;
        }
    }
    else if ((compression == kY800 || compression == kY8 || compression == kGrey) && bits == 8)
    {
        layout_ = Layout::Y800;
        bottomUp_ = false;
        stride_ = static_cast<size_t>(width_);
    }
    else if (compression == kYuy2 && bits == 16 && width_ % 2 == 0)
    {
        layout_ = Layout::Yuy2;
        bottomUp_ = false;
        stride_ = 2 * static_cast<size_t>(width_);
    }
    else
    {
        error = std::format("{} is compressed as {} at {} bits per pixel, which SeleneWatch does not decode",
            pathText_, compressionText(compression), bits);
        return false;
    }

    storedBytes_ = stride_ * static_cast<size_t>(height_);
    return true;
}

// OpenDML: a super index in the stream header lists standard index chunks
// (ix00), one per RIFF part. Each lists the frames' data offsets from a base.
bool AviReader::findFramesInOpenDmlIndex(std::vector<FrameChunk>& frames)
{
    frames.clear();
    const std::vector<uint8_t>& super = superIndex_;
    if (super.size() < 24 || u16(super.data()) != 4 || super[3] != kIndexOfIndexes)
    {
        return false;
    }

    const uint32_t entries = u32(super.data() + 4);
    if (24 + 16ull * entries > super.size())
    {
        return false;
    }

    for (uint32_t e = 0; e < entries; ++e)
    {
        Chunk chunk;
        if (!readChunk(u64(super.data() + 24 + 16 * e), fileBytes_, chunk) || chunk.size < 24
            || chunk.size > kMaxIndexChunk)
        {
            return false;
        }
        std::vector<uint8_t> index(chunk.size);
        if (!readAt(chunk.dataAt, index.data(), index.size()))
        {
            return false;
        }

        const uint8_t* x = index.data();
        const uint32_t count = u32(x + 4);
        if (u16(x) != 2 || x[3] != kIndexOfChunks || !isFrameChunk(u32(x + 8)) || 24 + 8ull * count > index.size())
        {
            return false;
        }
        const uint64_t base = u64(x + 12);
        for (uint32_t k = 0; k < count; ++k)
        {
            const uint8_t* item = x + 24 + 8 * k;
            // The top bit of the size marks a frame that is not a key frame.
            frames.push_back(FrameChunk{base + u32(item), u32(item + 4) & 0x7FFFFFFFu});
        }
    }
    return !frames.empty();
}

// AVI 1.0: idx1 lists every chunk of the first RIFF part. Its offsets count
// from the 'movi' type field in most files and from the file start in some;
// the first frame's chunk header tells which.
bool AviReader::findFramesInIdx1(std::vector<FrameChunk>& frames)
{
    frames.clear();
    if (!idx1_ || !firstMoviAt_ || idx1_->size < 16 || idx1_->size > kMaxIndexChunk)
    {
        return false;
    }

    std::vector<uint8_t> index(idx1_->size);
    if (!readAt(idx1_->dataAt, index.data(), index.size()))
    {
        return false;
    }

    std::vector<std::pair<uint32_t, uint32_t>> entries;
    uint32_t firstId = 0;
    for (size_t at = 0; at + 16 <= index.size(); at += 16)
    {
        const uint32_t id = u32(index.data() + at);
        if (isFrameChunk(id))
        {
            if (entries.empty())
            {
                firstId = id;
            }
            entries.emplace_back(u32(index.data() + at + 8), u32(index.data() + at + 12));
        }
    }
    if (entries.empty())
    {
        return false;
    }

    const auto headerIsFirstFrame = [&](uint64_t at) {
        Chunk chunk;
        return readChunk(at, fileBytes_, chunk) && chunk.id == firstId;
    };
    uint64_t base = 0;
    if (headerIsFirstFrame(*firstMoviAt_ + entries.front().first))
    {
        base = *firstMoviAt_;
    }
    else if (!headerIsFirstFrame(entries.front().first))
    {
        return false;
    }

    for (const auto& [offset, size] : entries)
    {
        frames.push_back(FrameChunk{base + offset + 8, size});
    }
    return true;
}

void AviReader::findFramesInMovi(uint64_t from, uint64_t to, std::vector<FrameChunk>& frames, int depth)
{
    uint64_t at = from;
    Chunk chunk;
    while (readChunk(at, to, chunk))
    {
        uint8_t type[4];
        if (chunk.id == kList && depth < kMaxDepth && chunk.size >= sizeof(type)
            && readAt(chunk.dataAt, type, sizeof(type)) && u32(type) == kRec)
        {
            findFramesInMovi(chunk.dataAt + sizeof(type), std::min<uint64_t>(chunk.dataAt + chunk.size, to),
                frames, depth + 1);
        }
        else if (isFrameChunk(chunk.id))
        {
            frames.push_back(FrameChunk{chunk.dataAt, chunk.size});
        }
        at = chunk.dataAt + chunk.size + (chunk.size & 1);
    }
}

bool AviReader::isFrameChunk(uint32_t id) const
{
    return id == frameChunkDb_ || id == frameChunkDc_;
}

bool AviReader::keepFrames(const std::vector<FrameChunk>& frames, std::string& error)
{
    frames_.clear();
    numbers_.clear();
    emptyChunks_ = 0;
    framesBeyondEnd_ = 0;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const FrameChunk& frame = frames[i];
        if (frame.size == 0)
        {
            // A frame dropped during capture. Its number is left unused.
            ++emptyChunks_;
            continue;
        }
        if (frame.size != storedBytes_)
        {
            error = std::format("{}: frame chunk {} holds {} bytes, but a {}x{} frame of this kind needs {}",
                pathText_, i, frame.size, width_, height_, storedBytes_);
            return false;
        }
        if (frame.dataAt + storedBytes_ > fileBytes_)
        {
            ++framesBeyondEnd_;
            continue;
        }
        frames_.push_back(frame.dataAt);
        numbers_.push_back(static_cast<uint64_t>(i));
    }

    if (frames_.empty())
    {
        error = pathText_ + " holds no complete video frame";
        return false;
    }

    // The index must point at a video frame.
    Chunk first;
    if (frames_.front() < 8 || !readChunk(frames_.front() - 8, fileBytes_, first) || !isFrameChunk(first.id))
    {
        error = pathText_ + ": its index does not point at a video frame";
        return false;
    }
    return true;
}

int AviReader::frameCount() const
{
    return static_cast<int>(frames_.size());
}

size_t AviReader::frameBytes() const
{
    const bool colour = layout_ == Layout::Bgr24 || layout_ == Layout::Bgra32;
    return ::frameBytes(width_, height_, colour ? PixelFormat::RGB888 : PixelFormat::Mono8);
}

uint64_t AviReader::frameNumber(int index) const
{
    return numbers_.at(static_cast<size_t>(index));
}

// AVI timing is by position: frame n is shown n frame intervals after the start.
std::chrono::system_clock::time_point AviReader::frameTime(int index) const
{
    const auto start = log_ && log_->startUtc ? *log_->startUtc : std::chrono::system_clock::time_point{};
    return start + std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::duration<double>(static_cast<double>(frameNumber(index)) / frameRate_));
}

const std::string& AviReader::indexUsed() const
{
    return indexUsed_;
}

StreamInfo AviReader::describe() const
{
    StreamInfo info;
    if (log_)
    {
        info.camera = log_->camera;
        info.exposure = log_->shutter;
        info.gain = log_->gain;
    }

    info.frameRate = rateFromFile_
        ? std::format("{:.3f} fps, from the AVI header", frameRate_)
        : std::format("{:.3f} fps, the rate set in the window; the AVI header states none", frameRate_);

    const std::string start = log_ && log_->startUtc
        ? std::format("the start time in FireCapture's log {} (Date, Start, and Start(UT))", log_->fileName)
        : std::string("an unknown start time counted from zero");
    info.frameTime = std::format("nominal, not measured: {} plus the frame's number at the frame rate", start);

    info.frameIndex = "the frame's number among the file's frame chunks, from 0";
    if (emptyChunks_ > 0)
    {
        info.frameIndex += std::format("; {} empty chunks mark frames dropped during capture and leave gaps",
            emptyChunks_);
    }
    info.frameIndex += "; AVI has no frame counter, so a frame dropped without an empty chunk does not show";
    if (framesBeyondEnd_ > 0)
    {
        info.frameIndex += std::format("; the file ends before {} more frames its index names", framesBeyondEnd_);
    }

    const std::string turned = bottomUp_ ? "rows turned the right way up" : "";
    switch (layout_)
    {
    case Layout::GreyDib:
        info.conversion = bottomUp_ ? turned + "; no value changed" : "none";
        break;
    case Layout::Y800:
        info.conversion = "none";
        break;
    case Layout::Bgr24:
        info.conversion = (bottomUp_ ? turned + ", " : std::string()) + "BGR turned into RGB; no value changed";
        break;
    case Layout::Bgra32:
        info.conversion = (bottomUp_ ? turned + ", " : std::string())
            + "BGR turned into RGB, the unused fourth byte dropped; no value changed";
        break;
    case Layout::Yuy2:
        info.conversion = "derived: only the luma (Y) samples of YUY2 are kept; the colour samples were dropped";
        break;
    }
    return info;
}

bool AviReader::readFrame(int index, Frame& frame, std::string& error)
{
    if (index < 0 || index >= frameCount())
    {
        error = std::format("frame {} is outside {}", index, pathText_);
        return false;
    }
    if (frame.data.size() != frameBytes())
    {
        error = "a frame buffer of the wrong size was given for " + pathText_;
        return false;
    }
    if (!readAt(frames_[static_cast<size_t>(index)], raw_.data(), storedBytes_))
    {
        error = std::format("cannot read frame {} of {}", index, pathText_);
        return false;
    }

    const bool colour = layout_ == Layout::Bgr24 || layout_ == Layout::Bgra32;
    const size_t width = static_cast<size_t>(width_);
    const size_t rowBytes = width * (colour ? 3 : 1);
    for (int y = 0; y < height_; ++y)
    {
        const size_t storedRow = static_cast<size_t>(bottomUp_ ? height_ - 1 - y : y);
        const uint8_t* source = raw_.data() + storedRow * stride_;
        uint8_t* row = frame.data.data() + static_cast<size_t>(y) * rowBytes;
        switch (layout_)
        {
        case Layout::GreyDib:
        case Layout::Y800:
            std::copy(source, source + width, row);
            break;
        case Layout::Bgr24:
            for (size_t x = 0; x < width; ++x)
            {
                row[3 * x] = source[3 * x + 2];
                row[3 * x + 1] = source[3 * x + 1];
                row[3 * x + 2] = source[3 * x];
            }
            break;
        case Layout::Bgra32:
            for (size_t x = 0; x < width; ++x)
            {
                row[3 * x] = source[4 * x + 2];
                row[3 * x + 1] = source[4 * x + 1];
                row[3 * x + 2] = source[4 * x];
            }
            break;
        case Layout::Yuy2:
            // Y0 U Y1 V: the luma samples are the even bytes.
            for (size_t x = 0; x < width; ++x)
            {
                row[x] = source[2 * x];
            }
            break;
        }
    }

    frame.width = width_;
    frame.height = height_;
    frame.format = colour ? PixelFormat::RGB888 : PixelFormat::Mono8;
    frame.significantBits = 8;
    frame.alignment = BitAlignment::Low;
    frame.bayer = BayerPattern::None;
    frame.blip.reset();
    frame.syntheticFlash = false;
    frame.testSource = false;
    return true;
}
