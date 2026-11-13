#include "recording/ser_reader.h"
#include "recording/ser_writer.h"

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <chrono>

namespace {

uint64_t word(const QByteArray& bytes, int offset, int count)
{
    uint64_t value = 0;
    for (int i = 0; i < count; ++i)
        value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
    return value;
}

class LimitedBuffer : public QBuffer
{
public:
    qint64 limit{0};
protected:
    qint64 writeData(const char* bytes, qint64 count) override
    {
        const qint64 allowed = std::min(count, limit - pos());
        if (allowed <= 0) { setErrorString("simulated full disk"); return -1; }
        return QBuffer::writeData(bytes, allowed);
    }
};

} // namespace

class SerWriterTest : public QObject
{
    Q_OBJECT
private slots:
    void exactPixelsHeaderAndTimes_data();
    void exactPixelsHeaderAndTimes();
    void failureLeavesZeroFrameCount();
    void changedLayoutIsRefused();
    void testMarkSurvivesWithoutSidecar();
    void aLaterTestFrameMarksTheWholeSource();
};

void SerWriterTest::exactPixelsHeaderAndTimes_data()
{
    QTest::addColumn<int>("format");
    QTest::addColumn<int>("bits");
    QTest::addColumn<bool>("high");
    QTest::addColumn<int>("bayer");
    QTest::addColumn<int>("colorId");
    QTest::newRow("mono8") << 0 << 8 << false << 0 << 0;
    QTest::newRow("rgb") << 2 << 8 << false << 0 << 100;
    QTest::newRow("mono16-low-bits-set") << 1 << 16 << false << 0 << 0;
    QTest::newRow("low12") << 1 << 12 << false << 0 << 0;
    QTest::newRow("high12") << 1 << 12 << true << 0 << 0;
    for (int pattern = 1; pattern <= 4; ++pattern)
        QTest::newRow(qPrintable(QString("bayer%1").arg(pattern))) << 1 << 12 << true << pattern << pattern + 7;
}

void SerWriterTest::exactPixelsHeaderAndTimes()
{
    QFETCH(int, format); QFETCH(int, bits); QFETCH(bool, high); QFETCH(int, bayer); QFETCH(int, colorId);
    Frame frame;
    frame.width = 4; frame.height = 2;
    frame.format = static_cast<PixelFormat>(format);
    frame.significantBits = bits;
    frame.alignment = high ? BitAlignment::High : BitAlignment::Low;
    frame.bayer = static_cast<BayerPattern>(bayer);
    frame.timestamp = std::chrono::system_clock::time_point{} + std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::milliseconds{123} + std::chrono::nanoseconds{456700});
    const int shift = high ? 16 - bits : 0;
    for (int i = 0; i < 8; ++i)
    {
        if (format == 1)
        {
            const uint16_t sample = static_cast<uint16_t>((((1u << bits) - 1u) - i * 17) << shift);
            frame.data.push_back(static_cast<uint8_t>(sample & 255));
            frame.data.push_back(static_cast<uint8_t>(sample >> 8));
        }
        else
        {
            for (int c = 0; c < (format == 2 ? 3 : 1); ++c)
                frame.data.push_back(static_cast<uint8_t>(10 * i + c));
        }
    }
    const std::vector<uint8_t> original = frame.data;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QDir(directory.path()).filePath("roundtrip.ser");
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    SerWriter writer(file);
    QString error;
    QVERIFY2(writer.start(frame, "test camera", error), qPrintable(error));
    QVERIFY2(writer.append(frame, error), qPrintable(error));
    frame.timestamp += std::chrono::milliseconds{37};
    frame.data[0] ^= 16;
    QVERIFY2(writer.append(frame, error), qPrintable(error));
    QVERIFY2(writer.finish(error), qPrintable(error));
    QVERIFY(file.flush());
    file.seek(0);
    const QByteArray bytes = file.readAll();
    QCOMPARE(bytes.left(14), QByteArray("LUCAM-RECORDER"));
    QCOMPARE(word(bytes, 18, 4), static_cast<uint64_t>(colorId));
    QCOMPARE(word(bytes, 22, 4), uint64_t{0});
    QCOMPARE(word(bytes, 26, 4), uint64_t{4});
    QCOMPARE(word(bytes, 30, 4), uint64_t{2});
    QCOMPARE(word(bytes, 34, 4), static_cast<uint64_t>(format == 1 ? (high ? 16 : bits) : 8));
    QCOMPARE(word(bytes, 38, 4), uint64_t{2});
    QCOMPARE(word(bytes, 162, 8), uint64_t{0});
    // Explicit epoch/layout check, independent of the production time conversion.
    QCOMPARE(word(bytes, 170, 8), uint64_t{621355968001234567});
    QCOMPARE(word(bytes, 178 + static_cast<int>(2 * frame.data.size()), 8), uint64_t{621355968001234567});
    QCOMPARE(bytes.size(), qsizetype{178} + static_cast<qsizetype>(2 * frame.data.size()) + 16);
    file.close();
    SerReader reader;
    std::string reason;
    QVERIFY2(reader.open(std::filesystem::path(path.toStdWString()), 30, reason), reason.c_str());
    QCOMPARE(reader.frameCount(), 2);
    Frame read;
    read.data.resize(reader.frameBytes());
    QVERIFY2(reader.readFrame(0, read, reason), reason.c_str());
    QCOMPARE(read.data, original);
    QVERIFY(read.format == frame.format);
    QVERIFY(read.bayer == frame.bayer);
    QCOMPARE(read.significantBits, bits);
    QVERIFY(read.alignment == frame.alignment);
    QVERIFY(reader.frameTime(1) == frame.timestamp);
    QVERIFY2(reader.readFrame(1, read, reason), reason.c_str());
    QCOMPARE(read.data, frame.data);
}

void SerWriterTest::failureLeavesZeroFrameCount()
{
    Frame frame;
    frame.width = frame.height = 2;
    frame.data = {0, 1, 2, 3};
    for (const qint64 limit : {qint64{100}, qint64{180}, qint64{185}})
    {
        LimitedBuffer output;
        output.limit = limit;
        QVERIFY(output.open(QIODevice::ReadWrite));
        SerWriter writer(output);
        QString error;
        const bool completed = writer.start(frame, "camera", error) && writer.append(frame, error) && writer.finish(error);
        QVERIFY(!completed);
        QVERIFY(!error.isEmpty());
        QVERIFY(!writer.finish(error));
        if (output.data().size() >= 42) QCOMPARE(word(output.data(), 38, 4), uint64_t{0});
    }
}

void SerWriterTest::changedLayoutIsRefused()
{
    Frame frame;
    frame.width = frame.height = 2;
    frame.data = {0, 1, 2, 3};
    QBuffer output;
    QVERIFY(output.open(QIODevice::ReadWrite));
    SerWriter writer(output);
    QString error;
    QVERIFY(writer.start(frame, "camera", error));
    QVERIFY(writer.append(frame, error));
    frame.bayer = BayerPattern::RGGB;
    QVERIFY(!writer.append(frame, error));
    QVERIFY(!writer.finish(error));
    QCOMPARE(word(output.data(), 38, 4), uint64_t{0});
}

void SerWriterTest::testMarkSurvivesWithoutSidecar()
{
    QTemporaryDir directory;
    Frame frame;
    frame.width = frame.height = 2;
    frame.data = {0, 1, 2, 3};
    const QString path = QDir(directory.path()).filePath("test.ser");
    QFile output(path);
    QVERIFY(output.open(QIODevice::WriteOnly));
    SerWriter writer(output);
    QString error;
    QVERIFY(writer.start(frame, "camera", error, true));
    QVERIFY(writer.append(frame, error));
    QVERIFY(writer.finish(error));
    QVERIFY(output.flush());
    output.close();
    SerReader reader;
    std::string reason;
    QVERIFY2(reader.open(std::filesystem::path(path.toStdWString()), 30, reason), reason.c_str());
    Frame read;
    read.data.resize(reader.frameBytes());
    QVERIFY(reader.readFrame(0, read, reason));
    QVERIFY(read.testSource);
    QVERIFY(!read.blip);
}

void SerWriterTest::aLaterTestFrameMarksTheWholeSource()
{
    QTemporaryDir directory;
    const QString path = QDir(directory.path()).filePath("late-test.ser");
    Frame frame;
    frame.width = frame.height = 2; frame.data = {0, 1, 2, 3};
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    SerWriter writer(file);
    QString error;
    QVERIFY(writer.start(frame, "camera", error));
    QVERIFY(writer.append(frame, error));
    frame.syntheticFlash = true;
    QVERIFY(writer.append(frame, error));
    QVERIFY(writer.finish(error));
    QVERIFY(file.flush()); file.close();
    SerReader reader;
    std::string reason;
    QVERIFY2(reader.open(std::filesystem::path(path.toStdWString()), 30, reason), reason.c_str());
    Frame first;
    first.data.resize(reader.frameBytes());
    QVERIFY(reader.readFrame(0, first, reason));
    QVERIFY(first.testSource);
}

QTEST_GUILESS_MAIN(SerWriterTest)
#include "ser_writer_tests.moc"
