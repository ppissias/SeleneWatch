#include "event_capture.h"
#include "event_writer.h"
#include "flash_detector.h"
#include "frame_pool.h"
#include "frame_ring_buffer.h"
#include "avi_reader.h"
#include "frame_source.h"
#include "latest_frame_slot.h"
#include "preview_image.h"
#include "recording_source.h"
#include "ser_reader.h"
#include "session.h"
#include "stream_info.h"
#include "synthetic_scene.h"

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimeZone>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtTest/QTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;

constexpr int kWidth = 640;
constexpr int kHeight = 480;

// A frame with one pixel, for tests that only care about order and identity.
FramePtr tiny(uint64_t index, uint8_t value)
{
    auto frame = std::make_shared<Frame>();
    frame->frameIndex = index;
    frame->data = {value};
    return frame;
}

FramePtr painted(uint64_t index, bool flash)
{
    auto frame = std::make_shared<Frame>();
    paintSyntheticFrame(*frame, kWidth, kHeight, index, flash);
    return frame;
}

void feed(EventCapture& capture, uint64_t& index, int count, bool flash)
{
    for (int n = 0; n < count; ++n)
    {
        capture.onFrame(painted(index, flash));
        ++index;
    }
}

// Four quiet frames establish a reference. Three flash frames follow: the
// onset, the trigger, and one more frame of the same run. Then `quietAfter`
// quiet frames. A clip ends after kPostEventFrames of those.
void runFlash(EventCapture& capture, uint64_t& index, int quietAfter)
{
    feed(capture, index, 4, false);
    feed(capture, index, 3, true);
    feed(capture, index, quietAfter, false);
}

system_clock::time_point testTime(int millisecond)
{
    const sys_days date = year{2020} / January / day{2};
    return date + hours{3} + minutes{4} + seconds{5} + milliseconds{millisecond};
}

QString utcText(system_clock::time_point time)
{
    const qint64 ms = duration_cast<milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODateWithMs);
}

QString utcStamp(system_clock::time_point time)
{
    const qint64 ms = duration_cast<milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString("yyyyMMdd-HHmmss-zzz");
}

FramePtr smallFrame(uint64_t index, system_clock::time_point time, std::vector<uint8_t> pixels,
    int width = 2, int height = 2)
{
    auto frame = std::make_shared<Frame>();
    frame->width = width;
    frame->height = height;
    frame->format = PixelFormat::Mono8;
    frame->frameIndex = index;
    frame->timestamp = time;
    frame->data = std::move(pixels);
    return frame;
}

// Two 2x2 frames around one candidate: onset at frame 4, trigger at frame 5.
EventClip smallClip(system_clock::time_point triggerTime)
{
    EventClip clip;
    clip.triggers.push_back(ClipTrigger{4, 5, triggerTime, 200, false});
    clip.deltaThreshold = 60;
    clip.minBrightPixels = 120;
    clip.framesBefore = 1;
    clip.framesAfter = 0;
    clip.frames.push_back(smallFrame(4, triggerTime - milliseconds{33}, {10, 10, 10, 10}));
    clip.frames.push_back(smallFrame(5, triggerTime, {255, 255, 255, 255}));
    return clip;
}

QString readText(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QStringList candidateLines(const QString& root)
{
    return readText(QDir(root).filePath("candidates.txt")).split('\n', Qt::SkipEmptyParts);
}

QStringList clipFolders(const QString& root)
{
    return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

// ------------------------------------------------------------ SER test files

// A SER file built byte by byte from the documented layout. It shares no
// code with the program's reader, so a mistake made the same way in a
// reader and a writer still shows here.
struct SerFileSpec
{
    int colorId = 0;
    int littleEndianFlag = 0;
    int width = 4;
    int height = 2;
    int pixelDepth = 8;

    // -1: the number of frames given.
    int headerFrameCount = -1;

    std::string instrument;
    int64_t dateTimeUtcTicks = 0;

    // The bytes of each frame, exactly as stored in the file.
    std::vector<std::vector<uint8_t>> frames;

    // One time per frame, in SER ticks. Empty: no trailer.
    std::vector<int64_t> trailer;

    // Extra bytes after the frames, to make a file cut off mid-frame.
    std::vector<uint8_t> tail;
};

void appendLittleEndian(QByteArray& bytes, uint64_t value, int width)
{
    for (int i = 0; i < width; ++i)
    {
        bytes.append(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

void appendTextField(QByteArray& bytes, const std::string& text)
{
    QByteArray field(40, '\0');
    for (size_t i = 0; i < text.size() && i < 40; ++i)
    {
        field[static_cast<qsizetype>(i)] = text[i];
    }
    bytes.append(field);
}

QString writeSerFile(const QString& dir, const QString& name, const SerFileSpec& spec)
{
    const int headerCount = spec.headerFrameCount >= 0
        ? spec.headerFrameCount : static_cast<int>(spec.frames.size());

    QByteArray bytes;
    bytes.append("LUCAM-RECORDER", 14);                          // FileID
    appendLittleEndian(bytes, 0, 4);                              // LuID
    appendLittleEndian(bytes, static_cast<uint32_t>(spec.colorId), 4);
    appendLittleEndian(bytes, static_cast<uint32_t>(spec.littleEndianFlag), 4);
    appendLittleEndian(bytes, static_cast<uint32_t>(spec.width), 4);
    appendLittleEndian(bytes, static_cast<uint32_t>(spec.height), 4);
    appendLittleEndian(bytes, static_cast<uint32_t>(spec.pixelDepth), 4);
    appendLittleEndian(bytes, static_cast<uint32_t>(headerCount), 4);
    appendTextField(bytes, "Observer");
    appendTextField(bytes, spec.instrument);
    appendTextField(bytes, "Telescope");
    appendLittleEndian(bytes, static_cast<uint64_t>(spec.dateTimeUtcTicks), 8);  // DateTime
    appendLittleEndian(bytes, static_cast<uint64_t>(spec.dateTimeUtcTicks), 8);  // DateTime_UTC
    Q_ASSERT(bytes.size() == 178);

    for (const std::vector<uint8_t>& frame : spec.frames)
    {
        bytes.append(reinterpret_cast<const char*>(frame.data()), static_cast<qsizetype>(frame.size()));
    }
    bytes.append(reinterpret_cast<const char*>(spec.tail.data()), static_cast<qsizetype>(spec.tail.size()));
    for (int64_t ticks : spec.trailer)
    {
        appendLittleEndian(bytes, static_cast<uint64_t>(ticks), 8);
    }

    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
    {
        return {};
    }
    return path;
}

// UTC as SER stores it: 100 ns ticks since 0001-01-01, which is
// 621355968000000000 ticks before 1970-01-01.
int64_t serTicks(system_clock::time_point time)
{
    using Ticks = duration<int64_t, std::ratio<1, 10'000'000>>;
    return duration_cast<Ticks>(time.time_since_epoch()).count() + 621'355'968'000'000'000;
}

std::vector<uint8_t> littleEndianSamples(const std::vector<uint16_t>& samples)
{
    std::vector<uint8_t> bytes;
    for (uint16_t sample : samples)
    {
        bytes.push_back(static_cast<uint8_t>(sample & 0xFF));
        bytes.push_back(static_cast<uint8_t>(sample >> 8));
    }
    return bytes;
}

std::vector<uint8_t> bigEndianSamples(const std::vector<uint16_t>& samples)
{
    std::vector<uint8_t> bytes;
    for (uint16_t sample : samples)
    {
        bytes.push_back(static_cast<uint8_t>(sample >> 8));
        bytes.push_back(static_cast<uint8_t>(sample & 0xFF));
    }
    return bytes;
}

std::filesystem::path fsPath(const QString& path)
{
    return std::filesystem::path(path.toStdWString());
}

// A painted 640x480 recording. Frames whose index is in [flashFrom, flashTo]
// carry the synthetic flash. Frame i is 33 ms after testTime(0).
SerFileSpec paintedRecording(int frames, int flashFrom, int flashTo)
{
    SerFileSpec spec;
    spec.width = kWidth;
    spec.height = kHeight;
    spec.instrument = "TestCam";
    spec.dateTimeUtcTicks = serTicks(testTime(0));
    for (int i = 0; i < frames; ++i)
    {
        Frame frame;
        paintSyntheticFrame(frame, kWidth, kHeight, static_cast<uint64_t>(i), i >= flashFrom && i <= flashTo);
        spec.frames.push_back(frame.data);
        spec.trailer.push_back(serTicks(testTime(i * 33)));
    }
    return spec;
}

// The same painted frame as Mono16, its 8-bit values moved into 12
// significant bits at the top or at the bottom of the word.
FramePtr paintedMono16(uint64_t index, bool flash, BitAlignment alignment)
{
    Frame painted8;
    paintSyntheticFrame(painted8, kWidth, kHeight, index, flash);

    auto frame = std::make_shared<Frame>(painted8);
    frame->format = PixelFormat::Mono16;
    frame->significantBits = 12;
    frame->alignment = alignment;
    std::vector<uint16_t> samples;
    for (uint8_t value : painted8.data)
    {
        const uint16_t twelveBit = static_cast<uint16_t>(value << 4);
        samples.push_back(alignment == BitAlignment::High ? static_cast<uint16_t>(twelveBit << 4) : twelveBit);
    }
    frame->data = littleEndianSamples(samples);
    return frame;
}

FramePtr paintedRgb(uint64_t index, bool flash)
{
    Frame painted8;
    paintSyntheticFrame(painted8, kWidth, kHeight, index, flash);

    auto frame = std::make_shared<Frame>(painted8);
    frame->format = PixelFormat::RGB888;
    frame->data.clear();
    for (uint8_t value : painted8.data)
    {
        frame->data.insert(frame->data.end(), {value, value, value});
    }
    return frame;
}

// ------------------------------------------------------------ AVI test files

// Writes RIFF chunks and lists into one byte array. Offsets in the array
// are file offsets, so index entries can be patched in afterwards.
class RiffBuilder
{
public:
    QByteArray bytes;

    // Starts a RIFF or LIST. Returns where its type field is.
    qsizetype beginList(const char* id, const char* type)
    {
        bytes.append(id, 4);
        open_.push_back(bytes.size());
        appendLittleEndian(bytes, 0, 4);
        const qsizetype typeAt = bytes.size();
        bytes.append(type, 4);
        return typeAt;
    }

    void endList()
    {
        const qsizetype sizeAt = open_.back();
        open_.pop_back();
        patch(sizeAt, static_cast<uint64_t>(bytes.size() - sizeAt - 4), 4);
    }

    // Writes a chunk. Returns where its header starts.
    qsizetype chunk(const char* id, const QByteArray& data)
    {
        const qsizetype headerAt = bytes.size();
        bytes.append(id, 4);
        appendLittleEndian(bytes, static_cast<uint64_t>(data.size()), 4);
        bytes.append(data);
        if (data.size() & 1)
        {
            bytes.append('\0');
        }
        return headerAt;
    }

    void patch(qsizetype at, uint64_t value, int width)
    {
        for (int i = 0; i < width; ++i)
        {
            bytes[at + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
        }
    }

private:
    std::vector<qsizetype> open_;
};

uint32_t fourccValue(const char* text)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(text[0])) | (static_cast<uint32_t>(static_cast<uint8_t>(text[1])) << 8)
        | (static_cast<uint32_t>(static_cast<uint8_t>(text[2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(text[3])) << 24);
}

// An AVI file built from the documented layout: RIFF, the AVI headers, a
// BITMAPINFOHEADER, and the AVI 1.0 or OpenDML index. It shares no code
// with the program's reader.
struct AviFileSpec
{
    enum class Kind { GreyDib, Bgr24, Bgra32, Y800, Yuy2, Compressed };
    enum class Index { OpenDml, Idx1Relative, Idx1Absolute, None };

    Kind kind = Kind::GreyDib;
    Index index = Index::OpenDml;
    int width = 5;
    int height = 3;

    // A DIB's rows are stored bottom-up unless this is false (a negative height).
    bool bottomUp = true;

    bool greyPalette = true;
    uint32_t rate = 25;
    uint32_t scale = 1;

    // OpenDML: frames in the first RIFF part. The rest go into an AVIX part.
    int framesInFirstPart = 2;

    // Each frame as the program should read it: top-down rows of grey
    // values, or of RGB triples for the colour kinds.
    std::vector<std::vector<uint8_t>> frames;

    // For Yuy2: the colour samples stored beside the luma. The reader drops them.
    uint8_t chromaU = 17;
    uint8_t chromaV = 230;

    // An empty chunk, a frame dropped during capture, follows each of these frames.
    std::vector<int> emptyChunkAfter;
};

int bitsPerPixel(AviFileSpec::Kind kind)
{
    switch (kind)
    {
    case AviFileSpec::Kind::GreyDib:
    case AviFileSpec::Kind::Y800:
        return 8;
    case AviFileSpec::Kind::Bgr24:
        return 24;
    case AviFileSpec::Kind::Bgra32:
        return 32;
    case AviFileSpec::Kind::Yuy2:
    case AviFileSpec::Kind::Compressed:
        return 16;
    }
    return 8;
}

bool isDib(AviFileSpec::Kind kind)
{
    return kind == AviFileSpec::Kind::GreyDib || kind == AviFileSpec::Kind::Bgr24 || kind == AviFileSpec::Kind::Bgra32;
}

// One frame as it is stored: DIB rows padded to four bytes and bottom-up
// unless told otherwise, BGR order, YUY2 as Y0 U Y1 V.
QByteArray storedFrame(const AviFileSpec& spec, const std::vector<uint8_t>& pixels)
{
    const int bits = bitsPerPixel(spec.kind);
    const int rowBytes = isDib(spec.kind) ? (spec.width * bits + 31) / 32 * 4 : spec.width * bits / 8;
    QByteArray stored(static_cast<qsizetype>(rowBytes) * spec.height, '\0');
    for (int y = 0; y < spec.height; ++y)
    {
        const int storedRow = isDib(spec.kind) && spec.bottomUp ? spec.height - 1 - y : y;
        char* row = stored.data() + static_cast<qsizetype>(storedRow) * rowBytes;
        for (int x = 0; x < spec.width; ++x)
        {
            const size_t grey = static_cast<size_t>(y * spec.width + x);
            const size_t rgb = 3 * grey;
            switch (spec.kind)
            {
            case AviFileSpec::Kind::GreyDib:
            case AviFileSpec::Kind::Y800:
                row[x] = static_cast<char>(pixels[grey]);
                break;
            case AviFileSpec::Kind::Bgr24:
                row[3 * x] = static_cast<char>(pixels[rgb + 2]);
                row[3 * x + 1] = static_cast<char>(pixels[rgb + 1]);
                row[3 * x + 2] = static_cast<char>(pixels[rgb]);
                break;
            case AviFileSpec::Kind::Bgra32:
                row[4 * x] = static_cast<char>(pixels[rgb + 2]);
                row[4 * x + 1] = static_cast<char>(pixels[rgb + 1]);
                row[4 * x + 2] = static_cast<char>(pixels[rgb]);
                row[4 * x + 3] = static_cast<char>(0xFF);
                break;
            case AviFileSpec::Kind::Yuy2:
            case AviFileSpec::Kind::Compressed:
                row[2 * x] = static_cast<char>(pixels[grey]);
                row[2 * x + 1] = static_cast<char>(x % 2 == 0 ? spec.chromaU : spec.chromaV);
                break;
            }
        }
    }
    return stored;
}

QString writeAviFile(const QString& dir, const QString& name, const AviFileSpec& spec)
{
    const int bits = bitsPerPixel(spec.kind);
    const uint32_t compression = spec.kind == AviFileSpec::Kind::Y800 ? fourccValue("Y800")
        : spec.kind == AviFileSpec::Kind::Yuy2 ? fourccValue("YUY2")
        : spec.kind == AviFileSpec::Kind::Compressed ? fourccValue("CRAM")
        : 0;
    const int frameCount = static_cast<int>(spec.frames.size());
    const bool openDml = spec.index == AviFileSpec::Index::OpenDml;
    const int firstPartFrames = openDml ? std::min(spec.framesInFirstPart, frameCount) : frameCount;
    const int parts = openDml && firstPartFrames < frameCount ? 2 : 1;

    QByteArray avih;
    appendLittleEndian(avih, 1'000'000ull * spec.scale / spec.rate, 4);  // dwMicroSecPerFrame
    appendLittleEndian(avih, 0, 4);                                       // dwMaxBytesPerSec
    appendLittleEndian(avih, 0, 4);                                       // dwPaddingGranularity
    appendLittleEndian(avih, 0x10, 4);                                    // dwFlags: has an index
    appendLittleEndian(avih, static_cast<uint64_t>(firstPartFrames), 4);  // dwTotalFrames
    appendLittleEndian(avih, 0, 4);                                       // dwInitialFrames
    appendLittleEndian(avih, 1, 4);                                       // dwStreams
    appendLittleEndian(avih, 0, 4);                                       // dwSuggestedBufferSize
    appendLittleEndian(avih, static_cast<uint64_t>(spec.width), 4);
    appendLittleEndian(avih, static_cast<uint64_t>(spec.height), 4);
    avih.append(QByteArray(16, '\0'));                                    // dwReserved[4]

    QByteArray strh;
    strh.append("vids", 4);
    appendLittleEndian(strh, compression, 4);                             // fccHandler
    appendLittleEndian(strh, 0, 4);                                       // dwFlags
    appendLittleEndian(strh, 0, 4);                                       // wPriority, wLanguage
    appendLittleEndian(strh, 0, 4);                                       // dwInitialFrames
    appendLittleEndian(strh, spec.scale, 4);
    appendLittleEndian(strh, spec.rate, 4);
    appendLittleEndian(strh, 0, 4);                                       // dwStart
    appendLittleEndian(strh, static_cast<uint64_t>(frameCount), 4);       // dwLength
    appendLittleEndian(strh, 0, 4);                                       // dwSuggestedBufferSize
    appendLittleEndian(strh, 0xFFFFFFFFu, 4);                             // dwQuality
    appendLittleEndian(strh, 0, 4);                                       // dwSampleSize
    appendLittleEndian(strh, 0, 8);                                       // rcFrame

    const bool negativeHeight = isDib(spec.kind) && !spec.bottomUp;
    QByteArray strf;
    appendLittleEndian(strf, 40, 4);                                      // biSize
    appendLittleEndian(strf, static_cast<uint32_t>(spec.width), 4);
    appendLittleEndian(strf, static_cast<uint32_t>(negativeHeight ? -spec.height : spec.height), 4);
    appendLittleEndian(strf, 1, 2);                                       // biPlanes
    appendLittleEndian(strf, static_cast<uint64_t>(bits), 2);             // biBitCount
    appendLittleEndian(strf, compression, 4);
    appendLittleEndian(strf, 0, 4);                                       // biSizeImage
    appendLittleEndian(strf, 0, 8);                                       // resolution
    appendLittleEndian(strf, spec.kind == AviFileSpec::Kind::GreyDib ? 256 : 0, 4);  // biClrUsed
    appendLittleEndian(strf, 0, 4);                                       // biClrImportant
    if (spec.kind == AviFileSpec::Kind::GreyDib)
    {
        for (int i = 0; i < 256; ++i)
        {
            const char grey = static_cast<char>(i);
            const char blue = spec.greyPalette ? grey : static_cast<char>(255 - i);
            strf.append(blue).append(grey).append(grey).append('\0');      // RGBQUAD: blue, green, red
        }
    }

    // The OpenDML super index, patched once the standard indexes are written.
    QByteArray indx;
    appendLittleEndian(indx, 4, 2);                                       // wLongsPerEntry
    indx.append('\0').append('\0');                                       // bIndexSubType, bIndexType: of indexes
    appendLittleEndian(indx, static_cast<uint64_t>(parts), 4);            // nEntriesInUse
    indx.append("00db", 4);                                               // dwChunkId
    indx.append(QByteArray(12, '\0'));                                    // dwReserved[3]
    indx.append(QByteArray(16 * parts, '\0'));                            // entries

    RiffBuilder riff;
    riff.beginList("RIFF", "AVI ");
    riff.beginList("LIST", "hdrl");
    riff.chunk("avih", avih);
    riff.beginList("LIST", "strl");
    riff.chunk("strh", strh);
    riff.chunk("strf", strf);
    const qsizetype indxEntriesAt = openDml ? riff.chunk("indx", indx) + 8 + 24 : 0;
    riff.endList();
    riff.endList();

    struct StandardIndex
    {
        qsizetype at;
        int frames;
        int size;
    };
    std::vector<StandardIndex> standardIndexes;
    std::vector<std::pair<qsizetype, int>> idx1Entries;

    int next = 0;
    for (int part = 0; part < parts; ++part)
    {
        if (part > 0)
        {
            riff.endList();  // the previous RIFF part
            riff.beginList("RIFF", "AVIX");
        }
        const qsizetype moviTypeAt = riff.beginList("LIST", "movi");
        const int partEnd = part == 0 ? firstPartFrames : frameCount;
        std::vector<std::pair<qsizetype, int>> partFrames;
        for (; next < partEnd; ++next)
        {
            const QByteArray stored = storedFrame(spec, spec.frames[static_cast<size_t>(next)]);
            const qsizetype headerAt = riff.chunk("00db", stored);
            partFrames.emplace_back(headerAt, static_cast<int>(stored.size()));
            idx1Entries.emplace_back(headerAt, static_cast<int>(stored.size()));

            if (std::find(spec.emptyChunkAfter.begin(), spec.emptyChunkAfter.end(), next) != spec.emptyChunkAfter.end())
            {
                const qsizetype emptyAt = riff.chunk("00db", QByteArray());
                partFrames.emplace_back(emptyAt, 0);
                idx1Entries.emplace_back(emptyAt, 0);
            }
        }
        if (openDml)
        {
            // A standard index: data offsets from a base, here the movi list.
            QByteArray ix;
            appendLittleEndian(ix, 2, 2);                                 // wLongsPerEntry
            ix.append('\0').append('\1');                                 // bIndexSubType, bIndexType: of chunks
            appendLittleEndian(ix, partFrames.size(), 4);
            ix.append("00db", 4);
            appendLittleEndian(ix, static_cast<uint64_t>(moviTypeAt), 8); // qwBaseOffset
            appendLittleEndian(ix, 0, 4);                                 // dwReserved
            for (const auto& [headerAt, size] : partFrames)
            {
                appendLittleEndian(ix, static_cast<uint64_t>(headerAt + 8 - moviTypeAt), 4);
                appendLittleEndian(ix, static_cast<uint64_t>(size), 4);
            }
            const qsizetype ixAt = riff.chunk("ix00", ix);
            standardIndexes.push_back({ixAt, static_cast<int>(partFrames.size()), static_cast<int>(ix.size() + 8)});
        }
        riff.endList();  // movi

        if (part == 0 && (spec.index == AviFileSpec::Index::Idx1Relative || spec.index == AviFileSpec::Index::Idx1Absolute))
        {
            QByteArray idx1;
            for (const auto& [headerAt, size] : idx1Entries)
            {
                idx1.append("00db", 4);
                appendLittleEndian(idx1, 0x10, 4);                        // AVIIF_KEYFRAME
                const qsizetype offset = spec.index == AviFileSpec::Index::Idx1Relative ? headerAt - moviTypeAt : headerAt;
                appendLittleEndian(idx1, static_cast<uint64_t>(offset), 4);
                appendLittleEndian(idx1, static_cast<uint64_t>(size), 4);
            }
            riff.chunk("idx1", idx1);
        }
    }
    riff.endList();  // the last RIFF part

    for (size_t i = 0; i < standardIndexes.size(); ++i)
    {
        const qsizetype entry = indxEntriesAt + static_cast<qsizetype>(16 * i);
        riff.patch(entry, static_cast<uint64_t>(standardIndexes[i].at), 8);
        riff.patch(entry + 8, static_cast<uint64_t>(standardIndexes[i].size), 4);
        riff.patch(entry + 12, static_cast<uint64_t>(standardIndexes[i].frames), 4);
    }

    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(riff.bytes) != riff.bytes.size())
    {
        return {};
    }
    return path;
}

// Frames of distinct values: frame f, sample i holds (f * 37 + i * 11) % 256.
std::vector<std::vector<uint8_t>> patternFrames(int frames, size_t samples)
{
    std::vector<std::vector<uint8_t>> result;
    for (int f = 0; f < frames; ++f)
    {
        std::vector<uint8_t> frame(samples);
        for (size_t i = 0; i < samples; ++i)
        {
            frame[i] = static_cast<uint8_t>((static_cast<size_t>(f) * 37 + i * 11) % 256);
        }
        result.push_back(std::move(frame));
    }
    return result;
}

bool writeTextFile(const QString& path, const QString& text)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Text) && file.write(text.toUtf8()) >= 0;
}

// Keeps what a source delivers. Its methods run on the source thread.
class CollectingListener final : public FrameSourceListener
{
public:
    void onFrame(std::shared_ptr<Frame> frame) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.push_back(std::move(frame));
        sequence_ += 'f';
    }

    void onDiscontinuity() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sequence_ += '|';
    }

    void onEnded() override
    {
        ended_.store(true);
    }

    void onFailed(const std::string& message) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failure_ = message;
    }

    [[nodiscard]] bool ended() const
    {
        return ended_.load();
    }

    [[nodiscard]] size_t frameCount() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

    [[nodiscard]] std::vector<FramePtr> frames() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {frames_.begin(), frames_.end()};
    }

    // 'f' for each frame and '|' for each discontinuity, in order.
    [[nodiscard]] std::string sequence() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sequence_;
    }

    [[nodiscard]] std::string failure() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Frame>> frames_;
    std::string sequence_;
    std::string failure_;
    std::atomic<bool> ended_{false};
};

} // namespace

class FramePoolTest : public QObject
{
    Q_OBJECT

private slots:
    void acquireGivesRequestedSize();
    void warmRingAllocatesNoMoreBuffers();
    void clipKeepsItsFramesWhileRingWraps();
    void droppedClipFreesAllButAFewSpares();
    void frameMayOutliveThePool();
};

class RingBufferTest : public QObject
{
    Q_OBJECT

private slots:
    void oldestFirstBeforeWrap();
    void oldestFirstAfterWrap();
    void storedSharesTheFrames();
    void clearLetsGoOfStoredFrames();
};

class LatestFrameSlotTest : public QObject
{
    Q_OBJECT

private slots:
    void newerFrameReplacesUnreadOne();
};

class FlashDetectorTest : public QObject
{
    Q_OBJECT

private slots:
    void markerAloneNeverFires();
    void runFiresOnceOnItsSecondFrame();
    void oneFrameSpotDoesNotFire();
    void mono16FramesFireLikeMono8();
    void rgbFramesFireLikeMono8();
};

class PreviewImageTest : public QObject
{
    Q_OBJECT

private slots:
    void twelveBitsAreVisibleAtTopAndBottom();
};

class SerReaderTest : public QObject
{
    Q_OBJECT

private slots:
    void readsMono8FramesAndTrailerTimes();
    void findsTwelveBitsAtTheTop();
    void findsTwelveBitsAtTheBottom();
    void keepsSixteenBitsWhenTheLowBitsAreUsed();
    void readsBigEndianSamplesWhenTheFlagIsOne();
    void turnsBgrIntoRgb();
    void recordsTheBayerPattern();
    void fileWithoutTrailerHasNoFrameTimes();
    void shortFilePlaysTheWholeFramesItHolds();
    void refusesWhatItCannotPlay();
};

class AviReaderTest : public QObject
{
    Q_OBJECT

private slots:
    void readsGreyDibSplitIntoTwoRiffParts();
    void readsBgr24SplitIntoTwoRiffParts();
    void readsThroughIdx1RelativeAndAbsolute();
    void walksAFileWithoutIndex();
    void refusesWhatItCannotPlay();
    void fireCaptureLogSuppliesStartAndSettings();
    void ignoresLogsThatCannotGiveTheStart();
};

class RecordingSourceTest : public QObject
{
    Q_OBJECT

private slots:
    void aviPlaysThroughTheSameSource();
    void droppedFramesLeaveGapsInIndexAndTime();
    void wholeFilePlaysEveryFrameThenEnds();
    void loopRestartsAndKeepsCounting();
    void realTimeFollowsTheFileIntervals();
    void fileWithoutTrailerGetsNominalTimes();
};

class SessionTest : public QObject
{
    Q_OBJECT

private slots:
    void serFileThroughSessionWritesAClip();
    void endOfFileHandsOffTheOpenClip();
    void loopJumpFiresNoCandidate();
    void unreadableFileDoesNotStart();
};

class EventCaptureTest : public QObject
{
    Q_OBJECT

private slots:
    void oneClipPerFlash();
    void secondFlashJoinsTheOpenClip();
    void flashWhilePreviousClipWaitsStartsNewClip();
    void endlessTriggerSplitsAtTheCap();
    void partialClipOnStop();
    void stopWhileIdleSavesNothing();
};

class EventWriterTest : public QObject
{
    Q_OBJECT

private slots:
    void writesMetadataAndCandidateLog();
    void pngRoundTripIsExact();
    void mono16PngRoundTripIsExact();
    void eventTxtDescribesTheStream();
    void sessionStartLineNamesTheSource();
    void clipBeyondQueueLimitIsLoggedNotSaved();
    void badClipDoesNotStopTheWriter();
    void sameTriggerTimeGetsTwoFolders();
    void unwritableRootIsARecordFailure();
    void submitAfterShutdownIsRefused();
    void captureToWriterEndToEnd();
};

// ---------------------------------------------------------------- FramePool

void FramePoolTest::acquireGivesRequestedSize()
{
    FramePool pool;
    const std::shared_ptr<Frame> frame = pool.acquire(100);
    QCOMPARE(frame->data.size(), static_cast<size_t>(100));
    QCOMPARE(pool.allocatedFrames(), static_cast<size_t>(1));
    QVERIFY(pool.allocatedBytes() >= 100);
}

void FramePoolTest::warmRingAllocatesNoMoreBuffers()
{
    FramePool pool;
    FrameRingBuffer ring(5);

    auto pushFrames = [&](int count) {
        for (int n = 0; n < count; ++n)
        {
            std::shared_ptr<Frame> frame = pool.acquire(64);
            ring.push(std::move(frame));
        }
    };

    // The new frame is acquired before the ring lets the oldest go, so a
    // warm ring of 5 keeps 6 frames in circulation.
    pushFrames(12);
    const size_t warm = pool.allocatedFrames();
    QVERIFY(warm <= ring.capacity() + 1);

    pushFrames(200);
    QCOMPARE(pool.allocatedFrames(), warm);
}

void FramePoolTest::clipKeepsItsFramesWhileRingWraps()
{
    FramePool pool;
    FrameRingBuffer ring(3);

    auto pushValue = [&](uint8_t value) {
        std::shared_ptr<Frame> frame = pool.acquire(4);
        std::fill(frame->data.begin(), frame->data.end(), value);
        ring.push(std::move(frame));
    };

    pushValue(1);
    pushValue(2);
    pushValue(3);
    std::vector<FramePtr> clip = ring.stored();
    QCOMPARE(clip.size(), static_cast<size_t>(3));

    // The ring wraps twice. The clip's frames are held, so new buffers are made.
    for (uint8_t value = 10; value < 16; ++value)
    {
        pushValue(value);
    }
    QCOMPARE(clip[0]->data, std::vector<uint8_t>(4, 1));
    QCOMPARE(clip[1]->data, std::vector<uint8_t>(4, 2));
    QCOMPARE(clip[2]->data, std::vector<uint8_t>(4, 3));

    const std::vector<FramePtr> inRing = ring.stored();
    for (const FramePtr& held : clip)
    {
        for (const FramePtr& current : inRing)
        {
            QVERIFY(held.get() != current.get());
        }
    }

    // Dropping the clip returns its three buffers. Further frames reuse them.
    const size_t beforeDrop = pool.allocatedFrames();
    clip.clear();
    for (uint8_t value = 20; value < 40; ++value)
    {
        pushValue(value);
    }
    QCOMPARE(pool.allocatedFrames(), beforeDrop);
}

void FramePoolTest::droppedClipFreesAllButAFewSpares()
{
    FramePool pool;

    // A clip holding many frames, as while it waits for the writer.
    std::vector<FramePtr> clip;
    for (int n = 0; n < 100; ++n)
    {
        clip.push_back(pool.acquire(1000));
    }
    QCOMPARE(pool.allocatedFrames(), static_cast<size_t>(100));
    QVERIFY(pool.allocatedBytes() >= 100 * 1000);

    // Writing it lets all of them go at once. The pool keeps a few spares.
    clip.clear();
    QCOMPARE(pool.allocatedFrames(), FramePool::kMaxSpareFrames);
    QVERIFY(pool.allocatedBytes() < 100 * 1000);
}

void FramePoolTest::frameMayOutliveThePool()
{
    std::shared_ptr<Frame> frame;
    {
        FramePool pool;
        frame = pool.acquire(8);
        frame->data[0] = 42;
    }
    QCOMPARE(frame->data[0], static_cast<uint8_t>(42));
    frame.reset();
}

// -------------------------------------------------------------- RingBuffer

void RingBufferTest::oldestFirstBeforeWrap()
{
    FrameRingBuffer buffer(3);
    buffer.push(tiny(1, 1));
    buffer.push(tiny(2, 2));

    const std::vector<FramePtr> stored = buffer.stored();
    QCOMPARE(stored.size(), static_cast<size_t>(2));
    QCOMPARE(stored[0]->frameIndex, static_cast<uint64_t>(1));
    QCOMPARE(stored[1]->frameIndex, static_cast<uint64_t>(2));
    QCOMPARE(stored[0]->data, std::vector<uint8_t>({1}));
    QCOMPARE(stored[1]->data, std::vector<uint8_t>({2}));
}

void RingBufferTest::oldestFirstAfterWrap()
{
    FrameRingBuffer buffer(3);
    for (uint64_t index = 1; index <= 4; ++index)
    {
        buffer.push(tiny(index, static_cast<uint8_t>(index)));
    }

    const std::vector<FramePtr> stored = buffer.stored();
    QCOMPARE(stored.size(), static_cast<size_t>(3));
    QCOMPARE(stored[0]->frameIndex, static_cast<uint64_t>(2));
    QCOMPARE(stored[1]->frameIndex, static_cast<uint64_t>(3));
    QCOMPARE(stored[2]->frameIndex, static_cast<uint64_t>(4));
}

void RingBufferTest::storedSharesTheFrames()
{
    FrameRingBuffer buffer(3);
    std::vector<FramePtr> pushed;
    for (uint64_t index = 1; index <= 5; ++index)
    {
        pushed.push_back(tiny(index, static_cast<uint8_t>(index)));
        buffer.push(pushed.back());
    }

    // The ring holds the last three. stored() hands out the same objects.
    const std::vector<FramePtr> stored = buffer.stored();
    QCOMPARE(stored.size(), static_cast<size_t>(3));
    QVERIFY(stored[0].get() == pushed[2].get());
    QVERIFY(stored[1].get() == pushed[3].get());
    QVERIFY(stored[2].get() == pushed[4].get());
}

void RingBufferTest::clearLetsGoOfStoredFrames()
{
    FrameRingBuffer buffer(3);
    const FramePtr frame = tiny(7, 9);
    buffer.push(frame);
    QCOMPARE(frame.use_count(), 2L);

    buffer.clear();
    QCOMPARE(buffer.size(), static_cast<size_t>(0));
    QVERIFY(buffer.stored().empty());
    QCOMPARE(frame.use_count(), 1L);

    buffer.push(tiny(8, 4));
    const std::vector<FramePtr> stored = buffer.stored();
    QCOMPARE(stored.size(), static_cast<size_t>(1));
    QCOMPARE(stored[0]->frameIndex, static_cast<uint64_t>(8));
}

// --------------------------------------------------------- LatestFrameSlot

void LatestFrameSlotTest::newerFrameReplacesUnreadOne()
{
    LatestFrameSlot slot;
    QVERIFY(slot.take() == nullptr);

    // Only the first put into an empty slot asks for a wake-up.
    QVERIFY(slot.put(tiny(1, 1)));
    QVERIFY(!slot.put(tiny(2, 2)));

    const FramePtr taken = slot.take();
    QVERIFY(taken != nullptr);
    QCOMPARE(taken->frameIndex, static_cast<uint64_t>(2));
    QVERIFY(slot.take() == nullptr);

    QVERIFY(slot.put(tiny(3, 3)));
}

// ----------------------------------------------------------- FlashDetector

void FlashDetectorTest::markerAloneNeverFires()
{
    FlashDetector detector;
    int maxRising = 0;
    for (uint64_t index = 0; index < 200; ++index)
    {
        QVERIFY(!detector.observe(*painted(index, false)));
        QVERIFY(!detector.firing());
        maxRising = std::max(maxRising, detector.lastBrightCount());
    }
    QVERIFY(maxRising < FlashDetector::kMinBrightPixels);
}

void FlashDetectorTest::runFiresOnceOnItsSecondFrame()
{
    FlashDetector detector;
    for (uint64_t index = 0; index < 4; ++index)
    {
        QVERIFY(!detector.observe(*painted(index, false)));
    }

    QVERIFY(!detector.observe(*painted(4, true)));
    QVERIFY(!detector.firing());
    QVERIFY(detector.lastBrightCount() >= FlashDetector::kMinBrightPixels);
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(4));

    QVERIFY(detector.observe(*painted(5, true)));
    QVERIFY(detector.firing());
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(4));

    // Later frames of the same run are the same candidate.
    QVERIFY(!detector.observe(*painted(6, true)));
    QVERIFY(detector.firing());
    QVERIFY(!detector.observe(*painted(7, true)));
    QVERIFY(detector.firing());

    // A quiet frame ends the run. The next run is a new candidate.
    QVERIFY(!detector.observe(*painted(8, false)));
    QVERIFY(!detector.firing());
    QVERIFY(!detector.observe(*painted(9, true)));
    QVERIFY(detector.observe(*painted(10, true)));
    QCOMPARE(detector.onsetFrameIndex(), static_cast<uint64_t>(9));
}

void FlashDetectorTest::oneFrameSpotDoesNotFire()
{
    FlashDetector detector;
    for (uint64_t index = 0; index < 2; ++index)
    {
        QVERIFY(!detector.observe(*painted(index, false)));
    }

    QVERIFY(!detector.observe(*painted(2, true)));
    QVERIFY(detector.lastBrightCount() >= FlashDetector::kMinBrightPixels);

    QVERIFY(!detector.observe(*painted(3, false)));
    QVERIFY(!detector.firing());
}

// 12-bit data, at the top or the bottom of the word, is read as its top 8
// bits, so the 60-level threshold means what it means for Mono8.
void FlashDetectorTest::mono16FramesFireLikeMono8()
{
    for (const BitAlignment alignment : {BitAlignment::High, BitAlignment::Low})
    {
        FlashDetector mono8;
        FlashDetector mono16;
        for (uint64_t index = 0; index < 4; ++index)
        {
            QVERIFY(!mono8.observe(*painted(index, false)));
            QVERIFY(!mono16.observe(*paintedMono16(index, false, alignment)));
            QCOMPARE(mono16.lastBrightCount(), mono8.lastBrightCount());
        }

        QVERIFY(!mono8.observe(*painted(4, true)));
        QVERIFY(!mono16.observe(*paintedMono16(4, true, alignment)));
        QCOMPARE(mono16.lastBrightCount(), mono8.lastBrightCount());
        QVERIFY(mono16.lastBrightCount() >= FlashDetector::kMinBrightPixels);

        QVERIFY(mono16.observe(*paintedMono16(5, true, alignment)));
        QVERIFY(mono16.firing());
        QCOMPARE(mono16.onsetFrameIndex(), static_cast<uint64_t>(4));
    }
}

// Grey stored as colour is read as the mean of its channels: the grey value.
void FlashDetectorTest::rgbFramesFireLikeMono8()
{
    FlashDetector mono8;
    FlashDetector rgb;
    for (uint64_t index = 0; index < 4; ++index)
    {
        QVERIFY(!mono8.observe(*painted(index, false)));
        QVERIFY(!rgb.observe(*paintedRgb(index, false)));
    }
    QVERIFY(!mono8.observe(*painted(4, true)));
    QVERIFY(!rgb.observe(*paintedRgb(4, true)));
    QCOMPARE(rgb.lastBrightCount(), mono8.lastBrightCount());
    QVERIFY(rgb.observe(*paintedRgb(5, true)));
}

// ------------------------------------------------------------ PreviewImage

void PreviewImageTest::twelveBitsAreVisibleAtTopAndBottom()
{
    struct Case
    {
        std::vector<uint16_t> samples;
        int significantBits;
        BitAlignment alignment;
    };
    const std::vector<Case> cases = {
        {{0xFFF0, 0x8000, 0x0010}, 12, BitAlignment::High},
        {{0x0FFF, 0x0800, 0x0001}, 12, BitAlignment::Low},
        {{0xFFFF, 0x8000, 0x0001}, 16, BitAlignment::Low},
    };

    for (const Case& c : cases)
    {
        Frame frame;
        frame.width = 3;
        frame.height = 1;
        frame.format = PixelFormat::Mono16;
        frame.significantBits = c.significantBits;
        frame.alignment = c.alignment;
        frame.data = littleEndianSamples(c.samples);
        const std::vector<uint8_t> stored = frame.data;

        const QImage image = frameToPreviewImage(frame);
        QCOMPARE(image.format(), QImage::Format_Grayscale8);
        const uchar* row = image.constScanLine(0);
        QCOMPARE(static_cast<int>(row[0]), 255);
        QCOMPARE(static_cast<int>(row[1]), 128);
        QCOMPARE(static_cast<int>(row[2]), 0);

        // The preview works on a copy. The stored samples are unchanged.
        QCOMPARE(frame.data, stored);
    }
}

// --------------------------------------------------------------- SerReader

void SerReaderTest::readsMono8FramesAndTrailerTimes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SerFileSpec spec;
    spec.instrument = "Chameleon";
    spec.dateTimeUtcTicks = serTicks(testTime(0));
    for (int i = 0; i < 3; ++i)
    {
        spec.frames.push_back({static_cast<uint8_t>(i), static_cast<uint8_t>(10 + i), 20, 30, 40, 50, 60,
            static_cast<uint8_t>(255 - i)});
        spec.trailer.push_back(serTicks(testTime(40 * i)));
    }
    const QString path = writeSerFile(dir.path(), "mono8.ser", spec);
    QVERIFY(!path.isEmpty());

    SerReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
    QCOMPARE(reader.frameCount(), 3);
    QCOMPARE(reader.headerFrameCount(), 3);
    QCOMPARE(reader.frameBytes(), static_cast<size_t>(8));
    QCOMPARE(QString::fromStdString(reader.instrument()), QString("Chameleon"));
    QVERIFY(reader.startTime().has_value());
    QVERIFY(*reader.startTime() == testTime(0));
    QVERIFY(reader.hasFrameTimes());

    for (int i = 0; i < 3; ++i)
    {
        QVERIFY(reader.frameTime(i) == testTime(40 * i));

        Frame frame;
        frame.data.resize(reader.frameBytes());
        QVERIFY2(reader.readFrame(i, frame, error), error.c_str());
        QCOMPARE(frame.width, 4);
        QCOMPARE(frame.height, 2);
        QVERIFY(frame.format == PixelFormat::Mono8);
        QCOMPARE(frame.significantBits, 8);
        QVERIFY(frame.alignment == BitAlignment::Low);
        QVERIFY(frame.bayer == BayerPattern::None);
        QCOMPARE(frame.data, spec.frames[static_cast<size_t>(i)]);
    }
}

// FireCapture writes PixelDepthPerPlane 16 for 12-bit data in the top bits.
// A header that says 12 for the same data is read the same way.
void SerReaderTest::findsTwelveBitsAtTheTop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const std::vector<uint16_t> samples = {0xFFF0, 0x0010, 0x1230, 0x8000, 0x0000, 0x4560, 0x7890, 0xABC0};
    for (const int headerDepth : {16, 12})
    {
        SerFileSpec spec;
        spec.pixelDepth = headerDepth;
        spec.frames = {littleEndianSamples(samples), littleEndianSamples(samples)};
        const QString path = writeSerFile(dir.path(), QString("top%1.ser").arg(headerDepth), spec);

        SerReader reader;
        std::string error;
        QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
        Frame frame;
        frame.data.resize(reader.frameBytes());
        QVERIFY2(reader.readFrame(1, frame, error), error.c_str());
        QVERIFY(frame.format == PixelFormat::Mono16);
        QCOMPARE(frame.significantBits, 12);
        QVERIFY(frame.alignment == BitAlignment::High);
        QCOMPARE(frame.data, littleEndianSamples(samples));
    }
}

void SerReaderTest::findsTwelveBitsAtTheBottom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const std::vector<uint16_t> samples = {0x0FFF, 0x0001, 0x0123, 0x0800, 0x0000, 0x0456, 0x0789, 0x0ABC};
    SerFileSpec spec;
    spec.pixelDepth = 12;
    spec.frames = {littleEndianSamples(samples)};
    const QString path = writeSerFile(dir.path(), "bottom.ser", spec);

    SerReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
    Frame frame;
    frame.data.resize(reader.frameBytes());
    QVERIFY2(reader.readFrame(0, frame, error), error.c_str());
    QCOMPARE(frame.significantBits, 12);
    QVERIFY(frame.alignment == BitAlignment::Low);
    QCOMPARE(frame.data, littleEndianSamples(samples));
}

void SerReaderTest::keepsSixteenBitsWhenTheLowBitsAreUsed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const std::vector<uint16_t> samples = {0xFFFF, 0x0001, 0x1234, 0x8000, 0x0000, 0x4567, 0x789A, 0xBCDE};
    SerFileSpec spec;
    spec.pixelDepth = 16;
    spec.frames = {littleEndianSamples(samples)};
    const QString path = writeSerFile(dir.path(), "sixteen.ser", spec);

    SerReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
    Frame frame;
    frame.data.resize(reader.frameBytes());
    QVERIFY2(reader.readFrame(0, frame, error), error.c_str());
    QCOMPARE(frame.significantBits, 16);
    QCOMPARE(frame.data, littleEndianSamples(samples));
}

// The flag is used inverted in practice: 1 means big-endian data.
void SerReaderTest::readsBigEndianSamplesWhenTheFlagIsOne()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const std::vector<uint16_t> samples = {0x1234, 0xABCD, 0x0001, 0xFF00, 0x0F0F, 0x8001, 0x7FFE, 0x0100};
    SerFileSpec spec;
    spec.pixelDepth = 16;
    spec.littleEndianFlag = 1;
    spec.frames = {bigEndianSamples(samples)};
    const QString path = writeSerFile(dir.path(), "big.ser", spec);

    SerReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
    Frame frame;
    frame.data.resize(reader.frameBytes());
    QVERIFY2(reader.readFrame(0, frame, error), error.c_str());
    QCOMPARE(frame.data, littleEndianSamples(samples));
}

void SerReaderTest::turnsBgrIntoRgb()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    for (const int colorId : {100, 101})
    {
        SerFileSpec spec;
        spec.colorId = colorId;
        spec.width = 2;
        spec.height = 1;
        spec.frames = {{1, 2, 3, 4, 5, 6}};
        const QString path = writeSerFile(dir.path(), QString("colour%1.ser").arg(colorId), spec);

        SerReader reader;
        std::string error;
        QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
        Frame frame;
        frame.data.resize(reader.frameBytes());
        QVERIFY2(reader.readFrame(0, frame, error), error.c_str());
        QVERIFY(frame.format == PixelFormat::RGB888);

        const std::vector<uint8_t> expected = colorId == 101
            ? std::vector<uint8_t>{3, 2, 1, 6, 5, 4}
            : std::vector<uint8_t>{1, 2, 3, 4, 5, 6};
        QCOMPARE(frame.data, expected);
    }
}

void SerReaderTest::recordsTheBayerPattern()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const std::vector<std::pair<int, BayerPattern>> patterns = {
        {8, BayerPattern::RGGB}, {9, BayerPattern::GRBG}, {10, BayerPattern::GBRG}, {11, BayerPattern::BGGR}};
    for (const auto& [colorId, pattern] : patterns)
    {
        SerFileSpec spec;
        spec.colorId = colorId;
        spec.frames = {{1, 2, 3, 4, 5, 6, 7, 8}};
        const QString path = writeSerFile(dir.path(), QString("bayer%1.ser").arg(colorId), spec);

        SerReader reader;
        std::string error;
        QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
        Frame frame;
        frame.data.resize(reader.frameBytes());
        QVERIFY2(reader.readFrame(0, frame, error), error.c_str());
        QVERIFY(frame.format == PixelFormat::Mono8);
        QVERIFY(frame.bayer == pattern);
        QCOMPARE(frame.data, spec.frames[0]);
    }
}

void SerReaderTest::fileWithoutTrailerHasNoFrameTimes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SerFileSpec spec;
    spec.dateTimeUtcTicks = serTicks(testTime(0));
    spec.frames = {std::vector<uint8_t>(8, 1), std::vector<uint8_t>(8, 2)};
    SerReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(writeSerFile(dir.path(), "dated.ser", spec)), 30.0, error), error.c_str());
    QVERIFY(!reader.hasFrameTimes());
    QVERIFY(reader.startTime().has_value());
    QVERIFY(*reader.startTime() == testTime(0));

    spec.dateTimeUtcTicks = 0;
    SerReader undated;
    QVERIFY2(undated.open(fsPath(writeSerFile(dir.path(), "undated.ser", spec)), 30.0, error), error.c_str());
    QVERIFY(!undated.startTime().has_value());
}

void SerReaderTest::shortFilePlaysTheWholeFramesItHolds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // The header names five frames. The file holds two and a half, so it
    // has no trailer either.
    SerFileSpec spec;
    spec.headerFrameCount = 5;
    spec.frames = {std::vector<uint8_t>(8, 1), std::vector<uint8_t>(8, 2)};
    spec.tail = {3, 3, 3, 3};
    const QString path = writeSerFile(dir.path(), "short.ser", spec);

    SerReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
    QCOMPARE(reader.frameCount(), 2);
    QCOMPARE(reader.headerFrameCount(), 5);
    QVERIFY(!reader.hasFrameTimes());

    Frame frame;
    frame.data.resize(reader.frameBytes());
    QVERIFY2(reader.readFrame(1, frame, error), error.c_str());
    QCOMPARE(frame.data, spec.frames[1]);
    QVERIFY(!reader.readFrame(2, frame, error));
}

void SerReaderTest::refusesWhatItCannotPlay()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    auto writeBytes = [&dir](const QString& name, const QByteArray& bytes) {
        const QString path = QDir(dir.path()).filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
        {
            file.write(bytes);
        }
        return path;
    };

    SerFileSpec colour16;
    colour16.colorId = 16;
    colour16.frames = {std::vector<uint8_t>(8, 0)};

    SerFileSpec rgb16;
    rgb16.colorId = 100;
    rgb16.pixelDepth = 16;
    rgb16.frames = {std::vector<uint8_t>(48, 0)};

    SerFileSpec empty;

    const std::vector<std::pair<QString, std::string>> cases = {
        {QDir(dir.path()).filePath("missing.ser"), "cannot"},
        {writeBytes("text.ser", QByteArray(300, 'x')), "not a SER file"},
        {writeBytes("tiny.ser", QByteArray(100, 'x')), "shorter than the 178-byte header"},
        {writeSerFile(dir.path(), "cyym.ser", colour16), "colour ID 16"},
        {writeSerFile(dir.path(), "rgb16.ser", rgb16), "16-bit colour"},
        {writeSerFile(dir.path(), "empty.ser", empty), "no complete frame"},
    };

    for (const auto& [path, reason] : cases)
    {
        SerReader reader;
        std::string error;
        QVERIFY(!reader.open(fsPath(path), 30.0, error));
        QVERIFY2(error.find(reason) != std::string::npos, error.c_str());
    }
}

// --------------------------------------------------------------- AviReader

namespace {

// Opens an AVI and reads every frame. Fails the calling test on an error.
std::vector<Frame> readAllAviFrames(AviReader& reader)
{
    std::vector<Frame> frames;
    for (int i = 0; i < reader.frameCount(); ++i)
    {
        Frame frame;
        frame.data.resize(reader.frameBytes());
        std::string error;
        if (!reader.readFrame(i, frame, error))
        {
            qWarning("%s", error.c_str());
            return {};
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

} // namespace

// An OpenDML file of two RIFF parts. A width of 5 pads each stored row to 8
// bytes, and the rows are stored bottom-up.
void AviReaderTest::readsGreyDibSplitIntoTwoRiffParts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AviFileSpec spec;
    spec.kind = AviFileSpec::Kind::GreyDib;
    spec.width = 5;
    spec.height = 3;
    spec.framesInFirstPart = 2;
    spec.frames = patternFrames(5, 15);
    const QString path = writeAviFile(dir.path(), "grey.avi", spec);
    QVERIFY(!path.isEmpty());

    AviReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
    QCOMPARE(QString::fromStdString(reader.indexUsed()), QString("OpenDML index"));
    QCOMPARE(reader.frameCount(), 5);
    QCOMPARE(reader.frameBytes(), static_cast<size_t>(15));

    const std::vector<Frame> frames = readAllAviFrames(reader);
    QCOMPARE(frames.size(), static_cast<size_t>(5));
    for (size_t i = 0; i < frames.size(); ++i)
    {
        QCOMPARE(frames[i].width, 5);
        QCOMPARE(frames[i].height, 3);
        QVERIFY(frames[i].format == PixelFormat::Mono8);
        QCOMPARE(frames[i].data, spec.frames[i]);
    }
    QCOMPARE(QString::fromStdString(reader.describe().conversion), QString("rows turned the right way up; no value changed"));
}

// A width of 3 makes 9-byte rows, padded to 12.
void AviReaderTest::readsBgr24SplitIntoTwoRiffParts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AviFileSpec spec;
    spec.kind = AviFileSpec::Kind::Bgr24;
    spec.width = 3;
    spec.height = 2;
    spec.framesInFirstPart = 1;
    spec.frames = patternFrames(3, 18);
    const QString path = writeAviFile(dir.path(), "colour.avi", spec);

    AviReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
    QCOMPARE(QString::fromStdString(reader.indexUsed()), QString("OpenDML index"));
    QCOMPARE(reader.frameCount(), 3);

    const std::vector<Frame> frames = readAllAviFrames(reader);
    QCOMPARE(frames.size(), static_cast<size_t>(3));
    for (size_t i = 0; i < frames.size(); ++i)
    {
        QVERIFY(frames[i].format == PixelFormat::RGB888);
        QCOMPARE(frames[i].data, spec.frames[i]);
    }
    QVERIFY(QString::fromStdString(reader.describe().conversion).contains("BGR turned into RGB"));
}

// idx1 offsets count from the 'movi' type field in most files and from the
// file start in some. A negative height stores the rows top-down.
void AviReaderTest::readsThroughIdx1RelativeAndAbsolute()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    for (const auto index : {AviFileSpec::Index::Idx1Relative, AviFileSpec::Index::Idx1Absolute})
    {
        AviFileSpec spec;
        spec.kind = AviFileSpec::Kind::Bgra32;
        spec.index = index;
        spec.width = 2;
        spec.height = 2;
        spec.bottomUp = false;
        spec.frames = patternFrames(3, 12);
        const QString name = index == AviFileSpec::Index::Idx1Relative ? "relative.avi" : "absolute.avi";
        const QString path = writeAviFile(dir.path(), name, spec);

        AviReader reader;
        std::string error;
        QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
        QCOMPARE(QString::fromStdString(reader.indexUsed()), QString("idx1"));

        const std::vector<Frame> frames = readAllAviFrames(reader);
        QCOMPARE(frames.size(), static_cast<size_t>(3));
        for (size_t i = 0; i < frames.size(); ++i)
        {
            QCOMPARE(frames[i].data, spec.frames[i]);
        }
        const QString conversion = QString::fromStdString(reader.describe().conversion);
        QVERIFY(!conversion.contains("rows turned"));
        QVERIFY(conversion.contains("fourth byte dropped"));
    }
}

// Y800 is kept as stored. YUY2 keeps only its luma, and says the pixels are derived.
void AviReaderTest::walksAFileWithoutIndex()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    for (const auto kind : {AviFileSpec::Kind::Y800, AviFileSpec::Kind::Yuy2})
    {
        AviFileSpec spec;
        spec.kind = kind;
        spec.index = AviFileSpec::Index::None;
        spec.width = 4;
        spec.height = 3;
        spec.frames = patternFrames(4, 12);
        const QString path = writeAviFile(dir.path(), kind == AviFileSpec::Kind::Y800 ? "y800.avi" : "yuy2.avi", spec);

        AviReader reader;
        std::string error;
        QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
        QCOMPARE(QString::fromStdString(reader.indexUsed()), QString("movi walk"));

        const std::vector<Frame> frames = readAllAviFrames(reader);
        QCOMPARE(frames.size(), static_cast<size_t>(4));
        for (size_t i = 0; i < frames.size(); ++i)
        {
            QVERIFY(frames[i].format == PixelFormat::Mono8);
            QCOMPARE(frames[i].data, spec.frames[i]);
        }

        const QString conversion = QString::fromStdString(reader.describe().conversion);
        if (kind == AviFileSpec::Kind::Y800)
        {
            QCOMPARE(conversion, QString("none"));
        }
        else
        {
            QVERIFY(conversion.startsWith("derived"));
        }
    }
}

void AviReaderTest::refusesWhatItCannotPlay()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AviFileSpec compressed;
    compressed.kind = AviFileSpec::Kind::Compressed;
    compressed.width = 4;
    compressed.frames = patternFrames(2, 12);

    AviFileSpec colourPalette;
    colourPalette.greyPalette = false;
    colourPalette.frames = patternFrames(2, 15);

    AviFileSpec noFrames;

    const QString text = QDir(dir.path()).filePath("text.avi");
    QVERIFY(writeTextFile(text, QString(300, QChar('x'))));

    const std::vector<std::pair<QString, std::string>> cases = {
        {writeAviFile(dir.path(), "cram.avi", compressed), "'CRAM'"},
        {writeAviFile(dir.path(), "palette.avi", colourPalette), "not a grey ramp"},
        {writeAviFile(dir.path(), "empty.avi", noFrames), "no complete video frame"},
        {text, "not an AVI file"},
        {QDir(dir.path()).filePath("missing.avi"), "cannot"},
    };

    for (const auto& [path, reason] : cases)
    {
        AviReader reader;
        std::string error;
        QVERIFY(!reader.open(fsPath(path), 30.0, error));
        QVERIFY2(error.find(reason) != std::string::npos, error.c_str());
    }
}

// The capture started at 00:30 local time on 1 January 2016, which was
// 23:30 UT on 31 December 2015. The local date is not the UT date.
void AviReaderTest::fireCaptureLogSuppliesStartAndSettings()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AviFileSpec spec;
    spec.rate = 25;
    spec.frames = patternFrames(3, 15);
    const QString path = writeAviFile(dir.path(), "night.avi", spec);
    QVERIFY(writeTextFile(QDir(dir.path()).filePath("night.txt"),
        "FireCapture v2.4  Settings\n"
        "------------------------------------\n"
        "Camera=Chameleon CMLN-13S2M\n"
        "Filename=night.avi\n"
        "Date=010116\n"
        "Start=003000.250\n"
        "Start(UT)=233000.250\n"
        "Date_format=ddMMyy\n"
        "Shutter=8.088ms\n"
        "Gain=510\n"));

    AviReader reader;
    std::string error;
    QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());

    const sys_days lastDay = year{2015} / December / day{31};
    const system_clock::time_point start = lastDay + hours{23} + minutes{30} + milliseconds{250};
    QVERIFY(reader.frameTime(0) == start);
    QVERIFY(reader.frameTime(2) == start + milliseconds{80});

    const StreamInfo info = reader.describe();
    QCOMPARE(QString::fromStdString(info.camera), QString("Chameleon CMLN-13S2M"));
    QCOMPARE(QString::fromStdString(info.exposure), QString("8.088ms"));
    QCOMPARE(QString::fromStdString(info.gain), QString("510"));
    QCOMPARE(QString::fromStdString(info.frameRate), QString("25.000 fps, from the AVI header"));
    QVERIFY(QString::fromStdString(info.frameTime).contains("FireCapture's log night.txt"));
}

// FireCapture 2.2 gives only the local start. A log that names another file
// is another recording's.
void AviReaderTest::ignoresLogsThatCannotGiveTheStart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AviFileSpec spec;
    spec.frames = patternFrames(2, 15);

    const QString oldPath = writeAviFile(dir.path(), "old.avi", spec);
    QVERIFY(writeTextFile(QDir(dir.path()).filePath("old.txt"),
        "FireCapture v2.2 Settings\n-----------------------------\nCamera=Chameleon\nFilename=old.avi\n"
        "Date=250613\nStart=202643\nShutter=0.763ms\nGain=611\n"));

    AviReader old;
    std::string error;
    QVERIFY2(old.open(fsPath(oldPath), 30.0, error), error.c_str());
    QVERIFY(old.frameTime(0) == system_clock::time_point{});
    QVERIFY(QString::fromStdString(old.describe().frameTime).contains("unknown start time"));
    QCOMPARE(QString::fromStdString(old.describe().gain), QString("611"));

    const QString otherPath = writeAviFile(dir.path(), "this.avi", spec);
    QVERIFY(writeTextFile(QDir(dir.path()).filePath("this.txt"),
        "FireCapture v2.4  Settings\nCamera=Elsewhere\nFilename=that.avi\nDate=010116\nStart=003000\n"
        "Start(UT)=233000\n"));

    AviReader other;
    QVERIFY2(other.open(fsPath(otherPath), 30.0, error), error.c_str());
    QVERIFY(other.frameTime(0) == system_clock::time_point{});
    QVERIFY(other.describe().camera.empty());
}

// --------------------------------------------------------- RecordingSource

void RecordingSourceTest::aviPlaysThroughTheSameSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AviFileSpec spec;
    spec.rate = 30000;
    spec.scale = 1001;
    spec.frames = patternFrames(4, 15);
    const QString path = writeAviFile(dir.path(), "played.AVI", spec);

    RecordingSource source(fsPath(path));
    source.setSpeed(RecordingSource::Speed::Fast);
    std::string error;
    QVERIFY2(source.open(error), error.c_str());
    const StreamInfo info = source.streamInfo();
    QCOMPARE(QString::fromStdString(info.source), path);
    QCOMPARE(QString::fromStdString(info.playback), QString("whole file once, frames 0 to 3"));
    QVERIFY(QString::fromStdString(info.frameRate).startsWith("29.970 fps"));

    CollectingListener listener;
    source.start(listener);
    QTRY_VERIFY_WITH_TIMEOUT(listener.ended(), 10000);
    source.stop();

    const std::vector<FramePtr> frames = listener.frames();
    QCOMPARE(frames.size(), static_cast<size_t>(4));
    for (size_t i = 0; i < frames.size(); ++i)
    {
        QCOMPARE(frames[i]->frameIndex, static_cast<uint64_t>(i));
        QCOMPARE(frames[i]->data, spec.frames[i]);
        const auto expected = duration_cast<system_clock::duration>(duration<double>(static_cast<double>(i) * 1001.0 / 30000.0));
        const auto offset = frames[i]->timestamp.time_since_epoch() - expected;
        QVERIFY(offset < microseconds{1} && offset > -microseconds{1});
    }
}

// Four frames at 25 fps, with an empty chunk after frame 1: the frames are
// numbered 0, 1, 3, 4 and timed at 0, 40, 120, 160 ms, through every index.
// Looped, the next lap continues at 5, 6, 8, 9.
void RecordingSourceTest::droppedFramesLeaveGapsInIndexAndTime()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    for (const auto index : {AviFileSpec::Index::OpenDml, AviFileSpec::Index::Idx1Relative, AviFileSpec::Index::None})
    {
        AviFileSpec spec;
        spec.index = index;
        spec.framesInFirstPart = 2;
        spec.frames = patternFrames(4, 15);
        spec.emptyChunkAfter = {1};
        const QString path = writeAviFile(dir.path(), QString("dropped%1.avi").arg(static_cast<int>(index)), spec);

        AviReader reader;
        std::string error;
        QVERIFY2(reader.open(fsPath(path), 30.0, error), error.c_str());
        QCOMPARE(reader.frameCount(), 4);
        QVERIFY(QString::fromStdString(reader.describe().frameIndex).contains("1 empty chunks"));

        for (const double loopSeconds : {0.0, 100.0})
        {
            RecordingSource source(fsPath(path));
            source.setLoopSeconds(loopSeconds);
            source.setSpeed(RecordingSource::Speed::Fast);
            QVERIFY2(source.open(error), error.c_str());

            CollectingListener listener;
            source.start(listener);
            if (loopSeconds > 0.0)
            {
                QTRY_VERIFY_WITH_TIMEOUT(listener.frameCount() >= 8, 10000);
            }
            else
            {
                QTRY_VERIFY_WITH_TIMEOUT(listener.ended(), 10000);
            }
            source.stop();

            const std::vector<uint64_t> expected = {0, 1, 3, 4, 5, 6, 8, 9};
            const std::vector<FramePtr> frames = listener.frames();
            const size_t checked = loopSeconds > 0.0 ? 8 : 4;
            QVERIFY(frames.size() >= checked);
            for (size_t k = 0; k < checked; ++k)
            {
                QCOMPARE(frames[k]->frameIndex, expected[k]);
                QCOMPARE(frames[k]->data, spec.frames[k % 4]);
                const auto time = milliseconds{40} * static_cast<int64_t>(expected[k]);
                const auto offset = frames[k]->timestamp.time_since_epoch() - time;
                QVERIFY2(offset < microseconds{1} && offset > -microseconds{1},
                    qPrintable(QString("frame %1 is %2 us off").arg(k).arg(duration_cast<microseconds>(offset).count())));
            }
        }
    }
}

void RecordingSourceTest::wholeFilePlaysEveryFrameThenEnds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SerFileSpec spec;
    for (int i = 0; i < 5; ++i)
    {
        spec.frames.push_back(std::vector<uint8_t>(8, static_cast<uint8_t>(10 * i)));
        spec.trailer.push_back(serTicks(testTime(40 * i)));
    }
    const QString path = writeSerFile(dir.path(), "whole.ser", spec);

    RecordingSource source(fsPath(path));
    source.setSpeed(RecordingSource::Speed::Fast);
    std::string error;
    QVERIFY2(source.open(error), error.c_str());
    const StreamInfo info = source.streamInfo();
    QCOMPARE(QString::fromStdString(info.source), path);
    QCOMPARE(QString::fromStdString(info.playback), QString("whole file once, frames 0 to 4"));

    CollectingListener listener;
    source.start(listener);
    QTRY_VERIFY_WITH_TIMEOUT(listener.ended(), 10000);
    source.stop();

    QCOMPARE(QString::fromStdString(listener.sequence()), QString("fffff"));
    const std::vector<FramePtr> frames = listener.frames();
    for (size_t i = 0; i < frames.size(); ++i)
    {
        QCOMPARE(frames[i]->frameIndex, static_cast<uint64_t>(i));
        QVERIFY(frames[i]->timestamp == testTime(40 * static_cast<int>(i)));
        QCOMPARE(frames[i]->data, spec.frames[i]);
    }
}

// Ten frames 100 ms apart. A loop over the first 0.35 s plays frames 0-3.
// Each lap is 0.4 s, so frame k of the run has the time of frame k at the
// file's rate, and its index is k.
void RecordingSourceTest::loopRestartsAndKeepsCounting()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SerFileSpec spec;
    for (int i = 0; i < 10; ++i)
    {
        spec.frames.push_back(std::vector<uint8_t>(8, static_cast<uint8_t>(i)));
        spec.trailer.push_back(serTicks(testTime(100 * i)));
    }
    const QString path = writeSerFile(dir.path(), "loop.ser", spec);

    RecordingSource source(fsPath(path));
    source.setLoopSeconds(0.35);
    source.setSpeed(RecordingSource::Speed::Fast);
    std::string error;
    QVERIFY2(source.open(error), error.c_str());
    QVERIFY(QString::fromStdString(source.streamInfo().playback).startsWith("loop over frames 0 to 3 of 10"));

    CollectingListener listener;
    source.start(listener);
    QTRY_VERIFY_WITH_TIMEOUT(listener.frameCount() >= 12, 10000);
    source.stop();
    QVERIFY(!listener.ended());

    QVERIFY(QString::fromStdString(listener.sequence()).startsWith("ffff|ffff|ffff"));
    const std::vector<FramePtr> frames = listener.frames();
    for (size_t k = 0; k < 12; ++k)
    {
        QCOMPARE(frames[k]->frameIndex, static_cast<uint64_t>(k));
        QVERIFY(frames[k]->timestamp == testTime(100 * static_cast<int>(k)));
        QCOMPARE(frames[k]->data, spec.frames[k % 4]);
    }
}

void RecordingSourceTest::realTimeFollowsTheFileIntervals()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SerFileSpec spec;
    for (int i = 0; i < 4; ++i)
    {
        spec.frames.push_back(std::vector<uint8_t>(8, 0));
        spec.trailer.push_back(serTicks(testTime(50 * i)));
    }
    const QString path = writeSerFile(dir.path(), "realtime.ser", spec);

    RecordingSource source(fsPath(path));
    std::string error;
    QVERIFY2(source.open(error), error.c_str());
    CollectingListener listener;
    source.start(listener);
    QTRY_VERIFY_WITH_TIMEOUT(listener.ended(), 10000);
    source.stop();

    // Three intervals of 50 ms. Allow for a coarse timer.
    const std::vector<FramePtr> frames = listener.frames();
    QCOMPARE(frames.size(), static_cast<size_t>(4));
    const auto played = frames.back()->steadyTimestamp - frames.front()->steadyTimestamp;
    QVERIFY2(played >= milliseconds{130},
        qPrintable(QString("played in %1 ms").arg(duration_cast<milliseconds>(played).count())));
}

void RecordingSourceTest::fileWithoutTrailerGetsNominalTimes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SerFileSpec spec;
    spec.dateTimeUtcTicks = serTicks(testTime(0));
    spec.frames = {std::vector<uint8_t>(8, 0), std::vector<uint8_t>(8, 1), std::vector<uint8_t>(8, 2)};
    const QString path = writeSerFile(dir.path(), "nominal.ser", spec);

    RecordingSource source(fsPath(path));
    source.setNominalFps(20.0);
    source.setSpeed(RecordingSource::Speed::Fast);
    std::string error;
    QVERIFY2(source.open(error), error.c_str());
    QVERIFY(QString::fromStdString(source.streamInfo().frameTime).startsWith("nominal"));

    CollectingListener listener;
    source.start(listener);
    QTRY_VERIFY_WITH_TIMEOUT(listener.ended(), 10000);
    source.stop();

    const std::vector<FramePtr> frames = listener.frames();
    QCOMPARE(frames.size(), static_cast<size_t>(3));
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const auto offset = frames[i]->timestamp - testTime(50 * static_cast<int>(i));
        QVERIFY(offset < microseconds{1} && offset > -microseconds{1});
    }
}

// ----------------------------------------------------------------- Session

// A SER file runs through a Session with no window, and the clip folder is
// named after the trigger frame's time in the file.
void SessionTest::serFileThroughSessionWritesAClip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeSerFile(dir.path(), "painted.ser", paintedRecording(60, 20, 22));
    const QString root = QDir(dir.path()).filePath("events");

    QObject context;
    std::vector<ClipWriteResult> results;
    bool ended = false;
    int candidates = 0;
    {
        Session::Notifications notifications;
        notifications.ended = [&ended] { ended = true; };
        notifications.candidateStarted = [&candidates] { ++candidates; };
        notifications.clipWritten = [&results](const ClipWriteResult& result) { results.push_back(result); };
        Session session(&context, root, std::move(notifications));

        auto source = std::make_unique<RecordingSource>(fsPath(path));
        source->setSpeed(RecordingSource::Speed::Fast);
        session.setSource(std::move(source));

        QString error;
        QVERIFY2(session.start(error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(ended, 30000);
        QVERIFY(session.sourceFinished());
        session.stop();
        QCOMPARE(session.acquiredFrames(), static_cast<uint64_t>(60));
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 30000);
    }

    QCOMPARE(candidates, 1);
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    const system_clock::time_point triggerTime = testTime(21 * 33);
    QCOMPARE(QDir(results[0].folder).dirName(), utcStamp(triggerTime));

    const QString text = readText(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(text.contains("source: " + path + "\n"));
    QVERIFY(text.contains("camera: TestCam\n"));
    QVERIFY(text.contains("frame_time: UTC from the file's trailer"));
    QVERIFY(text.contains("frame_index: the position in the file, from 0\n"));
    QVERIFY(text.contains("playback: whole file once, frames 0 to 59\n"));
    QVERIFY(text.contains("frame_size: 640x480\n"));
    QVERIFY(text.contains("pixel_format: Mono8\n"));
    QVERIFY(text.contains("trigger: onset_frame_index=20 trigger_frame_index=21 "));
    QVERIFY(text.trimmed().endsWith("complete: yes"));

    const QStringList lines = candidateLines(root);
    QCOMPARE(lines.size(), static_cast<qsizetype>(2));
    QVERIFY2(lines[0].endsWith(" session_start camera=\"TestCam\" playback=\"whole file once, frames 0 to 59\" source=\""
        + path + "\""), qPrintable(lines[0]));
    QVERIFY(lines[1].startsWith(utcText(triggerTime) + " onset_frame_index=20 trigger_frame_index=21 "));
    QVERIFY(lines[1].endsWith("clip=" + utcStamp(triggerTime)));
}

// A flash near the end: the file ends while its clip is still collecting.
void SessionTest::endOfFileHandsOffTheOpenClip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeSerFile(dir.path(), "late.ser", paintedRecording(30, 25, 27));
    const QString root = QDir(dir.path()).filePath("events");

    QObject context;
    std::vector<ClipWriteResult> results;
    bool ended = false;
    {
        Session::Notifications notifications;
        notifications.ended = [&ended] { ended = true; };
        notifications.clipWritten = [&results](const ClipWriteResult& result) { results.push_back(result); };
        Session session(&context, root, std::move(notifications));

        auto source = std::make_unique<RecordingSource>(fsPath(path));
        source->setSpeed(RecordingSource::Speed::Fast);
        session.setSource(std::move(source));

        QString error;
        QVERIFY2(session.start(error), qPrintable(error));

        // The clip is handed off at the end, before the window stops anything.
        QTRY_VERIFY_WITH_TIMEOUT(ended, 30000);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 30000);
        session.stop();
    }

    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    const QString text = readText(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(text.contains("trigger: onset_frame_index=25 trigger_frame_index=26 "));
    QVERIFY(text.contains("frames_before: 26\n"));
    QVERIFY(text.contains("frames_after: 3\n"));
    QVERIFY(text.trimmed().endsWith("complete: yes"));
}

// Each frame of the file is darker than the one before, so the jump from the
// last frame back to the first is a large rise over the whole frame.
void SessionTest::loopJumpFiresNoCandidate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int width = 64;
    const int height = 48;
    SerFileSpec spec;
    spec.width = width;
    spec.height = height;
    for (int i = 0; i < 10; ++i)
    {
        spec.frames.push_back(std::vector<uint8_t>(static_cast<size_t>(width * height),
            static_cast<uint8_t>(200 - 10 * i)));
        spec.trailer.push_back(serTicks(testTime(100 * i)));
    }
    const QString path = writeSerFile(dir.path(), "darkening.ser", spec);
    const QString root = QDir(dir.path()).filePath("events");

    // Without a restart at the jump, the capture fires on it.
    {
        int fired = 0;
        EventCapture capture(50, [&fired] { ++fired; }, [](EventClip) {});
        for (int k = 0; k < 20; ++k)
        {
            capture.onFrame(smallFrame(static_cast<uint64_t>(k), testTime(100 * k),
                spec.frames[static_cast<size_t>(k % 10)], width, height));
        }
        QVERIFY(fired >= 1);
    }

    QObject context;
    std::vector<ClipWriteResult> results;
    int candidates = 0;
    {
        Session::Notifications notifications;
        notifications.candidateStarted = [&candidates] { ++candidates; };
        notifications.clipWritten = [&results](const ClipWriteResult& result) { results.push_back(result); };
        Session session(&context, root, std::move(notifications));

        // A loop longer than the file loops the whole file.
        auto source = std::make_unique<RecordingSource>(fsPath(path));
        source->setLoopSeconds(100.0);
        source->setSpeed(RecordingSource::Speed::Fast);
        session.setSource(std::move(source));

        QString error;
        QVERIFY2(session.start(error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(session.acquiredFrames() >= 50, 10000);
        session.stop();
        QVERIFY(!session.sourceFinished());
    }
    QTest::qWait(100);

    QCOMPARE(candidates, 0);
    QVERIFY(results.empty());
    QVERIFY(clipFolders(root).isEmpty());
    const QStringList lines = candidateLines(root);
    QCOMPARE(lines.size(), static_cast<qsizetype>(1));
    QVERIFY(lines[0].contains(" session_start "));
}

void SessionTest::unreadableFileDoesNotStart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("bad.ser");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(300, 'x'));
    file.close();

    QObject context;
    Session session(&context, QDir(dir.path()).filePath("events"), {});
    session.setSource(std::make_unique<RecordingSource>(fsPath(path)));

    QString error;
    QVERIFY(!session.start(error));
    QVERIFY2(error.contains("not a SER file"), qPrintable(error));
    QVERIFY(!session.isRunning());
}

// ------------------------------------------------------------ EventCapture

void EventCaptureTest::oneClipPerFlash()
{
    std::vector<EventClip> clips;
    int candidates = 0;
    EventCapture capture(50, [&candidates] { ++candidates; }, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    runFlash(capture, index, kPostEventFrames - 1);
    QVERIFY(clips.empty());
    feed(capture, index, 1, false);

    QCOMPARE(candidates, 1);
    QCOMPARE(clips.size(), static_cast<size_t>(1));
    const EventClip& clip = clips[0];
    QCOMPARE(clip.triggers.size(), static_cast<size_t>(1));
    QCOMPARE(clip.triggers[0].onsetFrameIndex, static_cast<uint64_t>(4));
    QCOMPARE(clip.triggers[0].triggerFrameIndex, static_cast<uint64_t>(5));
    QVERIFY(!clip.triggers[0].continuesPreviousClip);
    QVERIFY(clip.triggers[0].brightPixelCount >= FlashDetector::kMinBrightPixels);
    QCOMPARE(clip.deltaThreshold, FlashDetector::kDeltaThreshold);
    QCOMPARE(clip.minBrightPixels, FlashDetector::kMinBrightPixels);

    // Frames 0-4 come before the trigger frame. After it: the third flash
    // frame, then the quiet tail.
    QCOMPARE(clip.framesBefore, 5);
    QCOMPARE(clip.framesAfter, 1 + kPostEventFrames);
    QCOMPARE(clip.frames.size(), static_cast<size_t>(5 + 1 + 1 + kPostEventFrames));
    for (size_t i = 0; i < clip.frames.size(); ++i)
    {
        QCOMPARE(clip.frames[i]->frameIndex, static_cast<uint64_t>(i));
    }
}

void EventCaptureTest::secondFlashJoinsTheOpenClip()
{
    std::vector<EventClip> clips;
    int candidates = 0;
    EventCapture capture(100, [&candidates] { ++candidates; }, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    // First flash: onset 4, trigger 5, last flash frame 6.
    // Second flash ten frames later: onset 14, trigger 15, last flash frame 16.
    uint64_t index = 0;
    feed(capture, index, 4, false);
    feed(capture, index, 3, true);
    feed(capture, index, 7, false);
    feed(capture, index, 3, true);

    // The tail is counted from the end of the second flash.
    feed(capture, index, kPostEventFrames - 1, false);
    QVERIFY(clips.empty());
    feed(capture, index, 1, false);

    QCOMPARE(candidates, 2);
    QCOMPARE(clips.size(), static_cast<size_t>(1));
    const EventClip& clip = clips[0];
    QCOMPARE(clip.triggers.size(), static_cast<size_t>(2));
    QCOMPARE(clip.triggers[0].onsetFrameIndex, static_cast<uint64_t>(4));
    QCOMPARE(clip.triggers[0].triggerFrameIndex, static_cast<uint64_t>(5));
    QCOMPARE(clip.triggers[1].onsetFrameIndex, static_cast<uint64_t>(14));
    QCOMPARE(clip.triggers[1].triggerFrameIndex, static_cast<uint64_t>(15));

    const uint64_t lastIndex = 16 + kPostEventFrames;
    QCOMPARE(clip.frames.back()->frameIndex, lastIndex);
    QCOMPARE(clip.framesAfter, static_cast<int>(lastIndex - 5));
    QCOMPARE(clip.frames.size(), static_cast<size_t>(lastIndex + 1));
}

void EventCaptureTest::flashWhilePreviousClipWaitsStartsNewClip()
{
    // The clips collected here stand for clips waiting in the writer's queue.
    std::vector<EventClip> clips;
    EventCapture capture(50, [] {}, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    runFlash(capture, index, kPostEventFrames);
    QCOMPARE(clips.size(), static_cast<size_t>(1));

    // Nothing has been "written". A second flash is still detected and kept.
    runFlash(capture, index, kPostEventFrames);
    QCOMPARE(clips.size(), static_cast<size_t>(2));
    QVERIFY(clips[1].triggers[0].triggerFrameIndex > clips[0].triggers[0].triggerFrameIndex);

    // The second clip's history overlaps the first clip's tail. The frames
    // they have in common are the same objects, not copies.
    int shared = 0;
    for (const FramePtr& first : clips[0].frames)
    {
        for (const FramePtr& second : clips[1].frames)
        {
            if (first->frameIndex == second->frameIndex)
            {
                QVERIFY(first.get() == second.get());
                ++shared;
            }
        }
    }
    QVERIFY(shared > 0);
}

void EventCaptureTest::endlessTriggerSplitsAtTheCap()
{
    std::vector<EventClip> clips;
    int candidates = 0;
    EventCapture capture(50, [&candidates] { ++candidates; }, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    // The flash never ends: onset 4, trigger 5, and every later frame fires.
    uint64_t index = 0;
    feed(capture, index, 4, false);
    feed(capture, index, 2, true);
    feed(capture, index, kMaxPostEventFrames - 1, true);
    QVERIFY(clips.empty());

    feed(capture, index, 1, true);
    QCOMPARE(clips.size(), static_cast<size_t>(1));
    QCOMPARE(clips[0].framesAfter, kMaxPostEventFrames);
    QCOMPARE(clips[0].frames.back()->frameIndex, static_cast<uint64_t>(5 + kMaxPostEventFrames));

    // A new clip started at once and carries on. It is not a new candidate.
    feed(capture, index, 10, true);
    capture.finishPartial();
    QCOMPARE(clips.size(), static_cast<size_t>(2));
    QCOMPARE(candidates, 1);

    const EventClip& next = clips[1];
    QCOMPARE(next.triggers.size(), static_cast<size_t>(1));
    QVERIFY(next.triggers[0].continuesPreviousClip);
    QCOMPARE(next.triggers[0].onsetFrameIndex, static_cast<uint64_t>(4));
    QCOMPARE(next.triggers[0].triggerFrameIndex, static_cast<uint64_t>(5 + kMaxPostEventFrames));
    QCOMPARE(next.framesAfter, 10);

    // Its history is the ring at that moment, so it overlaps the first clip.
    QVERIFY(next.frames.front()->frameIndex < clips[0].frames.back()->frameIndex);
}

void EventCaptureTest::partialClipOnStop()
{
    std::vector<EventClip> clips;
    EventCapture capture(50, [] {}, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    // The third flash frame plus 9 quiet frames is a 10-frame tail.
    runFlash(capture, index, 9);
    QVERIFY(clips.empty());
    capture.finishPartial();

    QCOMPARE(clips.size(), static_cast<size_t>(1));
    QCOMPARE(clips[0].framesAfter, 10);
    QCOMPARE(clips[0].triggers[0].triggerFrameIndex, static_cast<uint64_t>(5));
}

void EventCaptureTest::stopWhileIdleSavesNothing()
{
    std::vector<EventClip> clips;
    EventCapture capture(50, [] {}, [&clips](EventClip clip) {
        clips.push_back(std::move(clip));
    });

    uint64_t index = 0;
    feed(capture, index, 3, false);
    capture.finishPartial();
    QVERIFY(clips.empty());
}

// ------------------------------------------------------------- EventWriter

void EventWriterTest::writesMetadataAndCandidateLog()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const system_clock::time_point triggerTime = testTime(6);
    QVERIFY(utcText(triggerTime).endsWith(QLatin1Char('Z')));

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(smallClip(triggerTime));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);

    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));
    QVERIFY(!results[0].recordFailed);
    QCOMPARE(QDir(results[0].folder).dirName(), utcStamp(triggerTime));

    const QString text = readText(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(text.contains("triggers: 1\n"));
    QVERIFY(text.contains("trigger: onset_frame_index=4 trigger_frame_index=5 bright_pixel_count=200 trigger_time="
        + utcText(triggerTime) + "\n"));
    QVERIFY(text.contains("delta_threshold: 60\n"));
    QVERIFY(text.contains("min_bright_pixels: 120\n"));
    QVERIFY(text.contains("frames_before: 1\n"));
    QVERIFY(text.contains("0000.png 4 " + utcText(triggerTime - milliseconds{33}) + "\n"));
    QVERIFY(text.contains("0001.png 5 " + utcText(triggerTime) + "\n"));
    QVERIFY(text.trimmed().endsWith("complete: yes"));
    QCOMPARE(QDir(results[0].folder).entryList(QStringList() << "*.png").size(), static_cast<qsizetype>(2));

    const QStringList lines = candidateLines(root.path());
    QCOMPARE(lines.size(), static_cast<qsizetype>(1));
    QVERIFY(lines[0].startsWith(utcText(triggerTime) + " onset_frame_index=4 trigger_frame_index=5 "));
    QVERIFY(lines[0].contains("delta_threshold=60 min_bright_pixels=120"));
    QVERIFY(lines[0].endsWith("clip=" + utcStamp(triggerTime)));
}

void EventWriterTest::pngRoundTripIsExact()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    // Every 8-bit value, in an image whose width is not a multiple of four.
    const int width = 17;
    const int height = 16;
    std::vector<uint8_t> pixels(static_cast<size_t>(width * height));
    for (size_t i = 0; i < pixels.size(); ++i)
    {
        pixels[i] = static_cast<uint8_t>((i * 7) % 256);
    }

    EventClip clip;
    clip.triggers.push_back(ClipTrigger{0, 0, testTime(100), 0, false});
    clip.frames.push_back(smallFrame(0, testTime(100), pixels, width, height));

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(std::move(clip));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));

    QImage image(QDir(results[0].folder).filePath("0000.png"));
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), width);
    QCOMPARE(image.height(), height);
    QCOMPARE(image.format(), QImage::Format_Grayscale8);
    for (int y = 0; y < height; ++y)
    {
        const uchar* row = image.constScanLine(y);
        for (int x = 0; x < width; ++x)
        {
            QCOMPARE(row[x], pixels[static_cast<size_t>(y * width + x)]);
        }
    }
}

void EventWriterTest::clipBeyondQueueLimitIsLoggedNotSaved()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    // Room for one waiting frame. The clip has two.
    EventWriter writer(&context, root.path(), 1, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });

    EventClip clip = smallClip(testTime(200));
    const FramePtr watched = clip.frames[0];
    writer.submit(std::move(clip));

    // The frames were dropped at once, before the writer thread did anything.
    QCOMPARE(watched.use_count(), 1L);

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);
    QVERIFY(results[0].folder.isEmpty());
    QCOMPARE(results[0].notSaved, QString("writer busy"));
    QVERIFY(!results[0].recordFailed);

    QVERIFY(clipFolders(root.path()).isEmpty());
    const QStringList lines = candidateLines(root.path());
    QCOMPARE(lines.size(), static_cast<qsizetype>(1));
    QVERIFY(lines[0].startsWith(utcText(testTime(200))));
    QVERIFY(lines[0].endsWith("not_saved=writer busy"));
}

void EventWriterTest::badClipDoesNotStopTheWriter()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });

    // A 2x2 frame that holds three bytes cannot be written.
    EventClip bad = smallClip(testTime(300));
    bad.frames[1] = smallFrame(5, testTime(300), {1, 2, 3});
    writer.submit(std::move(bad));
    writer.submit(smallClip(testTime(400)));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(2), 10000);

    QVERIFY(results[0].folder.isEmpty());
    QVERIFY(results[0].notSaved.contains("holds 3 bytes"));
    QVERIFY(!results[0].recordFailed);

    QVERIFY2(results[1].notSaved.isEmpty(), qPrintable(results[1].notSaved));
    QCOMPARE(QDir(results[1].folder).dirName(), utcStamp(testTime(400)));
    QVERIFY(readText(QDir(results[1].folder).filePath("event.txt")).trimmed().endsWith("complete: yes"));

    // The failed clip left a folder without the completion marker.
    const QString partial = readText(QDir(root.path()).filePath(utcStamp(testTime(300)) + "/event.txt"));
    QVERIFY(!partial.isEmpty());
    QVERIFY(!partial.contains("complete: yes"));

    const QStringList lines = candidateLines(root.path());
    QCOMPARE(lines.size(), static_cast<qsizetype>(2));
    QVERIFY(lines[0].contains("not_saved="));
    QVERIFY(lines[0].contains("holds 3 bytes"));
    QVERIFY(lines[1].endsWith("clip=" + utcStamp(testTime(400))));
}

void EventWriterTest::sameTriggerTimeGetsTwoFolders()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(smallClip(testTime(500)));
    writer.submit(smallClip(testTime(500)));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(2), 10000);

    QCOMPARE(QDir(results[0].folder).dirName(), utcStamp(testTime(500)));
    QCOMPARE(QDir(results[1].folder).dirName(), utcStamp(testTime(500)) + "-2");
    QCOMPARE(clipFolders(root.path()).size(), static_cast<qsizetype>(2));
    for (const ClipWriteResult& result : results)
    {
        QVERIFY(readText(QDir(result.folder).filePath("event.txt")).trimmed().endsWith("complete: yes"));
    }
}

void EventWriterTest::unwritableRootIsARecordFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // The output root lies under a file, so nothing can be created there.
    const QString blocker = QDir(dir.path()).filePath("blocker");
    QFile file(blocker);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, blocker + "/events", 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(smallClip(testTime(600)));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);

    QVERIFY(results[0].folder.isEmpty());
    QVERIFY(!results[0].notSaved.isEmpty());
    QVERIFY(results[0].recordFailed);
}

void EventWriterTest::submitAfterShutdownIsRefused()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.shutdown();

    EventClip clip = smallClip(testTime(700));
    const FramePtr watched = clip.frames[0];
    writer.submit(std::move(clip));
    QCOMPARE(watched.use_count(), 1L);

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);
    QVERIFY(results[0].folder.isEmpty());
    QVERIFY(results[0].recordFailed);
    QVERIFY(clipFolders(root.path()).isEmpty());
}

void EventWriterTest::captureToWriterEndToEnd()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 1000, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    EventCapture capture(50, [] {}, [&writer](EventClip clip) {
        writer.submit(std::move(clip));
    });

    // Two flashes far enough apart to make two clips, the second while the
    // first may still be with the writer.
    uint64_t index = 0;
    runFlash(capture, index, kPostEventFrames);
    runFlash(capture, index, kPostEventFrames);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(2), 30000);

    for (const ClipWriteResult& result : results)
    {
        QVERIFY2(result.notSaved.isEmpty(), qPrintable(result.notSaved));
        QVERIFY(readText(QDir(result.folder).filePath("event.txt")).trimmed().endsWith("complete: yes"));
    }
    QCOMPARE(QDir(results[0].folder).entryList(QStringList() << "*.png").size(),
        static_cast<qsizetype>(5 + 1 + 1 + kPostEventFrames));
    QCOMPARE(candidateLines(root.path()).size(), static_cast<qsizetype>(2));
}

// Feature-7's item 9 for Mono16: values above 255, with the low bits set,
// come back from the PNG exactly.
void EventWriterTest::mono16PngRoundTripIsExact()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const int width = 17;
    const int height = 16;
    std::vector<uint16_t> samples(static_cast<size_t>(width * height));
    for (size_t i = 0; i < samples.size(); ++i)
    {
        samples[i] = static_cast<uint16_t>((i * 241 + 3) % 65536);
    }

    auto frame = std::make_shared<Frame>();
    frame->width = width;
    frame->height = height;
    frame->format = PixelFormat::Mono16;
    frame->significantBits = 16;
    frame->timestamp = testTime(150);
    frame->data = littleEndianSamples(samples);

    EventClip clip;
    clip.triggers.push_back(ClipTrigger{0, 0, testTime(150), 0, false});
    clip.frames.push_back(frame);

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(std::move(clip));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));

    QImage image(QDir(results[0].folder).filePath("0000.png"));
    QVERIFY(!image.isNull());
    QCOMPARE(image.format(), QImage::Format_Grayscale16);
    QCOMPARE(image.width(), width);
    QCOMPARE(image.height(), height);
    for (int y = 0; y < height; ++y)
    {
        const auto* row = reinterpret_cast<const quint16*>(image.constScanLine(y));
        for (int x = 0; x < width; ++x)
        {
            QCOMPARE(row[x], samples[static_cast<size_t>(y * width + x)]);
        }
    }
}

void EventWriterTest::eventTxtDescribesTheStream()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    auto mono16 = [](uint64_t index, system_clock::time_point time) {
        auto frame = std::make_shared<Frame>();
        frame->width = 2;
        frame->height = 2;
        frame->format = PixelFormat::Mono16;
        frame->significantBits = 12;
        frame->alignment = BitAlignment::High;
        frame->frameIndex = index;
        frame->timestamp = time;
        frame->data = littleEndianSamples({0x0010, 0x0020, 0xFFF0, 0x1230});
        return FramePtr(frame);
    };

    EventClip played;
    played.triggers.push_back(ClipTrigger{4, 5, testTime(800), 200, false});
    played.frames = {mono16(4, testTime(767)), mono16(5, testTime(800))};
    played.stream.source = "D:/space/Moon 1.ser";
    played.stream.exposure = "8.088ms";
    played.stream.gain = "510";
    played.stream.frameRate = "not stored in SER files";
    played.stream.frameTime = "UTC from the file's trailer";
    played.stream.frameIndex = "the position in the file, from 0";
    played.stream.conversion = "none";
    played.stream.playback = "whole file once, frames 0 to 9";

    // A live source has no playback line.
    EventClip live = smallClip(testTime(850));
    live.stream.source = "synthetic";

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.submit(std::move(played));
    writer.submit(std::move(live));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(2), 10000);
    QVERIFY2(results[0].notSaved.isEmpty(), qPrintable(results[0].notSaved));

    const QString text = readText(QDir(results[0].folder).filePath("event.txt"));
    QVERIFY(text.startsWith("source: D:/space/Moon 1.ser\n"));
    QVERIFY(text.contains("camera: unknown\n"));
    QVERIFY(text.contains("exposure: 8.088ms\n"));
    QVERIFY(text.contains("gain: 510\n"));
    QVERIFY(text.contains("frame_rate: not stored in SER files\n"));
    QVERIFY(text.contains("conversion: none\n"));
    QVERIFY(text.contains("frame_time: UTC from the file's trailer\n"));
    QVERIFY(text.contains("frame_index: the position in the file, from 0\n"));
    QVERIFY(text.contains("playback: whole file once, frames 0 to 9\n"));
    QVERIFY(text.contains("frame_size: 2x2\n"));
    QVERIFY(text.contains("pixel_format: Mono16\n"));
    QVERIFY(text.contains("significant_bits: 12\n"));
    QVERIFY(text.contains("bit_alignment: high\n"));
    QVERIFY(text.contains("bayer_pattern: none\n"));
    QVERIFY(text.trimmed().endsWith("complete: yes"));

    const QString liveText = readText(QDir(results[1].folder).filePath("event.txt"));
    QVERIFY(liveText.startsWith("source: synthetic\n"));
    QVERIFY(!liveText.contains("playback:"));
    QVERIFY(liveText.contains("pixel_format: Mono8\n"));
}

void EventWriterTest::sessionStartLineNamesTheSource()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());

    StreamInfo recording;
    recording.source = "C:/data/Moon 2015.ser";
    recording.camera = "Chameleon";
    recording.playback = "loop over frames 0 to 3 of 10";

    StreamInfo synthetic;
    synthetic.source = "synthetic";

    QObject context;
    std::vector<ClipWriteResult> results;
    EventWriter writer(&context, root.path(), 100, [&results](const ClipWriteResult& result) {
        results.push_back(result);
    });
    writer.logSessionStart(recording);
    writer.submit(smallClip(testTime(900)));
    writer.logSessionStart(synthetic);

    // Only the clip is reported. A session_start line is reported only when
    // it cannot be written.
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), static_cast<size_t>(1), 10000);
    writer.shutdown();
    QCOMPARE(results.size(), static_cast<size_t>(1));

    const QStringList lines = candidateLines(root.path());
    QCOMPARE(lines.size(), static_cast<qsizetype>(3));
    QVERIFY2(lines[0].endsWith(
        " session_start camera=\"Chameleon\" playback=\"loop over frames 0 to 3 of 10\" source=\"C:/data/Moon 2015.ser\""),
        qPrintable(lines[0]));
    QVERIFY(lines[1].startsWith(utcText(testTime(900)) + " onset_frame_index=4 "));
    QVERIFY2(lines[2].endsWith(" session_start camera=\"unknown\" source=\"synthetic\""), qPrintable(lines[2]));
}

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    int status = 0;
    {
        FramePoolTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        RingBufferTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        LatestFrameSlotTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        FlashDetectorTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        PreviewImageTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        SerReaderTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        AviReaderTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        RecordingSourceTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        EventCaptureTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        EventWriterTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    {
        SessionTest tests;
        status |= QTest::qExec(&tests, argc, argv);
    }
    return status;
}

#include "core_tests.moc"
