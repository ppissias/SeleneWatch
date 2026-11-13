#include "recording/ser_writer.h"

#include <QtCore/QByteArray>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

void put(QByteArray& bytes, qsizetype at, uint64_t value, int width)
{
    for (int i = 0; i < width; ++i)
    {
        bytes[at + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
}

int colorId(const Frame& frame)
{
    if (frame.format == PixelFormat::RGB888)
    {
        return 100;
    }
    switch (frame.bayer)
    {
    case BayerPattern::None: return 0;
    case BayerPattern::RGGB: return 8;
    case BayerPattern::GRBG: return 9;
    case BayerPattern::GBRG: return 10;
    case BayerPattern::BGGR: return 11;
    }
    return -1;
}

bool validFrame(const Frame& frame, QString& error)
{
    if (frame.width <= 0 || frame.height <= 0 || bytesPerPixel(frame.format) == 0
        || frame.data.size() != frameBytes(frame.width, frame.height, frame.format))
    {
        error = QString("frame %1 is %2x%3 but holds %4 bytes")
            .arg(frame.frameIndex).arg(frame.width).arg(frame.height).arg(frame.data.size());
        return false;
    }
    if (frame.significantBits < 1 || frame.significantBits > 16 || colorId(frame) < 0
        || (frame.format != PixelFormat::Mono16 && frame.significantBits != 8)
        || (frame.format == PixelFormat::RGB888 && frame.bayer != BayerPattern::None))
    {
        error = "invalid pixel layout for SER output";
        return false;
    }
    return true;
}

} // namespace

int64_t timeToSerTicks(std::chrono::system_clock::time_point time)
{
    using Ticks = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;
    constexpr int64_t epoch = 621'355'968'000'000'000;
    const int64_t ticks = std::chrono::duration_cast<Ticks>(time.time_since_epoch()).count();
    if (ticks < -epoch || ticks > std::numeric_limits<int64_t>::max() - epoch)
    {
        throw std::invalid_argument("frame time is outside the SER timestamp range");
    }
    return epoch + ticks;
}

SerWriter::SerWriter(QIODevice& output)
    : output_(output)
{
}

bool SerWriter::write(const char* bytes, qint64 count, QString& error)
{
    while (count > 0)
    {
        const qint64 written = output_.write(bytes, count);
        if (written <= 0)
        {
            failed_ = true;
            error = "could not write SER data: " + output_.errorString();
            return false;
        }
        count -= written;
        bytes += written;
    }
    return true;
}

bool SerWriter::start(const Frame& layout, const std::string& camera, QString& error, bool testSource)
{
    if (started_ || !output_.isWritable() || output_.isSequential() || output_.pos() != 0)
    {
        error = "SER output needs a new, writable, seekable device";
        return false;
    }
    if (!validFrame(layout, error))
    {
        return false;
    }
    width_ = layout.width;
    height_ = layout.height;
    format_ = layout.format;
    bayer_ = layout.bayer;
    bits_ = layout.significantBits;
    alignment_ = layout.alignment;
    QByteArray header(178, '\0');
    std::copy_n("LUCAM-RECORDER", 14, header.data());
    put(header, 18, static_cast<uint32_t>(colorId(layout)), 4);
    put(header, 22, 0, 4); // Inverted in the ecosystem: 0 means little-endian.
    put(header, 26, static_cast<uint32_t>(width_), 4);
    put(header, 30, static_cast<uint32_t>(height_), 4);
    const int depth = format_ != PixelFormat::Mono16 ? 8
        : (alignment_ == BitAlignment::Low && bits_ >= 9 ? bits_ : 16);
    put(header, 34, static_cast<uint32_t>(depth), 4);
    if (testSource)
    {
        constexpr std::string_view mark = "SeleneWatch TEST DATA";
        std::copy(mark.begin(), mark.end(), header.data() + 42);
    }
    std::copy_n(camera.data(), std::min(camera.size(), size_t{40}), header.data() + 82);
    // Local time is unknown. Only DateTime_UTC is populated.
    put(header, 170, static_cast<uint64_t>(timeToSerTicks(layout.timestamp)), 8);
    started_ = true;
    return write(header.constData(), header.size(), error);
}

bool SerWriter::append(const Frame& frame, QString& error)
{
    if (!started_ || failed_ || finished_)
    {
        error = "SER writer is not accepting frames";
        return false;
    }
    if (!validFrame(frame, error) || frame.width != width_ || frame.height != height_
        || frame.format != format_ || frame.bayer != bayer_ || frame.significantBits != bits_
        || frame.alignment != alignment_ || times_.size() >= std::numeric_limits<uint32_t>::max())
    {
        failed_ = true;
        if (error.isEmpty()) error = "frame layout changed inside a SER clip";
        return false;
    }
    const int64_t time = timeToSerTicks(frame.timestamp);
    if (!write(reinterpret_cast<const char*>(frame.data.data()), static_cast<qint64>(frame.data.size()), error))
    {
        return false;
    }
    times_.push_back(time);
    return true;
}

bool SerWriter::finish(QString& error)
{
    if (!started_ || failed_ || finished_ || times_.empty())
    {
        error = "SER clip cannot be completed";
        return false;
    }
    QByteArray trailer(static_cast<qsizetype>(times_.size()) * 8, '\0');
    for (size_t i = 0; i < times_.size(); ++i)
    {
        put(trailer, static_cast<qsizetype>(i) * 8, static_cast<uint64_t>(times_[i]), 8);
    }
    if (!write(trailer.constData(), trailer.size(), error)) return false;
    const qint64 end = output_.pos();
    QByteArray count(4, '\0');
    put(count, 0, times_.size(), 4);
    if (!output_.seek(38) || !write(count.constData(), count.size(), error) || !output_.seek(end))
    {
        failed_ = true;
        if (error.isEmpty()) error = "could not finalize the SER frame count";
        return false;
    }
    finished_ = true;
    return true;
}
