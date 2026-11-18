#include "timelapse/fits_io.h"
#include "recording/metadata_text.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <fitsio.h>

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimeZone>

#include <algorithm>
#include <cstring>

namespace {

QString fitsErrorText(int status)
{
    char msg[FLEN_STATUS]{};
    fits_get_errstatus(status, msg);
    char detail[FLEN_ERRMSG]{};
    QString text = QString::fromLatin1(msg);
    if (fits_read_errmsg(detail))
        text += QString(" (%1)").arg(QString::fromLatin1(detail));
    return text;
}

QString formatFitsUtc(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{}) return {};
    return utcDateTime(tp).toString("yyyy-MM-ddTHH:mm:ss.zzz");
}

std::chrono::system_clock::time_point parseFitsUtc(const char* value)
{
    if (!value || !*value) return {};
    QString s = QString::fromLatin1(value).trimmed();
    if (s.endsWith('Z')) s.chop(1);
    QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!dt.isValid()) dt = QDateTime::fromString(s, Qt::ISODate);
    if (!dt.isValid()) return {};
    dt.setTimeZone(QTimeZone::UTC);
    return std::chrono::system_clock::time_point{} + std::chrono::milliseconds{dt.toMSecsSinceEpoch()};
}

void writeStrKey(fitsfile* fptr, const char* key, const std::string& val, const char* comment, int& status)
{
    if (status || val.empty()) return;
    fits_update_key(fptr, TSTRING, key, const_cast<char*>(val.c_str()), comment, &status);
}

void writeDoubleKey(fitsfile* fptr, const char* key, double val, const char* comment, int& status)
{
    if (status) return;
    fits_update_key(fptr, TDOUBLE, key, &val, comment, &status);
}

void writeLongKey(fitsfile* fptr, const char* key, long long val, const char* comment, int& status)
{
    if (status) return;
    fits_update_key(fptr, TLONGLONG, key, &val, comment, &status);
}

std::string readStrKey(fitsfile* fptr, const char* key)
{
    char buf[FLEN_VALUE]{};
    int status = 0;
    fits_read_key(fptr, TSTRING, key, buf, nullptr, &status);
    if (status != 0) return {};
    return std::string(buf);
}

bool readDoubleKey(fitsfile* fptr, const char* key, double& out)
{
    int status = 0;
    double v = 0;
    fits_read_key(fptr, TDOUBLE, key, &v, nullptr, &status);
    if (status != 0) return false;
    out = v;
    return true;
}

bool readLongKey(fitsfile* fptr, const char* key, uint64_t& out)
{
    int status = 0;
    long long v = 0;
    fits_read_key(fptr, TLONGLONG, key, &v, nullptr, &status);
    if (status != 0 || v < 0) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

} // namespace

bool writeFitsFloat(const QString& path, const FitsImage& image, QString& error)
{
    if (!image.valid())
    {
        error = "invalid FITS image buffer";
        return false;
    }

    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath()))
    {
        error = QString("could not create directory for %1").arg(path);
        return false;
    }
    const QString tmpPath = info.absolutePath() + "/" + info.fileName() + ".tmp";
    QFile::remove(tmpPath);

    int status = 0;
    fitsfile* fptr = nullptr;
    const QByteArray encoded = QFile::encodeName(tmpPath);
    if (fits_create_diskfile(&fptr, encoded.constData(), &status) != 0)
    {
        error = QString("CFITSIO create failed for %1: %2").arg(path, fitsErrorText(status));
        return false;
    }

    long naxes[3]{image.width, image.height, image.channels};
    const int naxis = image.channels == 3 ? 3 : 2;
    fits_create_img(fptr, FLOAT_IMG, naxis, naxes, &status);

    writeDoubleKey(fptr, "BSCALE", 1.0, "Linear scale factor", status);
    writeDoubleKey(fptr, "BZERO", 0.0, "Linear zero offset", status);
    if (image.channels == 3)
        writeStrKey(fptr, "CTYPE3", "RGB", "Plane 1=R, 2=G, 3=B", status);

    const FitsMetadata& m = image.meta;
    if (const QString dObs = formatFitsUtc(m.dateObs); !dObs.isEmpty())
        writeStrKey(fptr, "DATE-OBS", dObs.toStdString(), "UTC start of capture window", status);
    if (const QString dAvg = formatFitsUtc(m.dateAvg); !dAvg.isEmpty())
        writeStrKey(fptr, "DATE-AVG", dAvg.toStdString(), "UTC mid-point of capture window", status);
    if (const QString dEnd = formatFitsUtc(m.dateEnd); !dEnd.isEmpty())
        writeStrKey(fptr, "DATE-END", dEnd.toStdString(), "UTC end of capture window", status);
    if (m.exptimeSeconds > 0)
        writeDoubleKey(fptr, "EXPTIME", m.exptimeSeconds, "Single frame exposure in seconds", status);
    if (m.hasGain)
        writeDoubleKey(fptr, "GAIN", m.gain, "Camera gain setting", status);
    writeStrKey(fptr, "INSTRUME", m.camera, "Camera / instrument", status);
    writeStrKey(fptr, "TELESCOP", m.telescope, "Telescope", status);
    writeStrKey(fptr, "OBSERVER", m.observer, "Observer", status);
    writeStrKey(fptr, "SITENAME", m.siteName, "Observing site", status);
    writeStrKey(fptr, "OBJECT", m.targetPreset, "Time-lapse target preset", status);
    writeStrKey(fptr, "SWCREATE", m.softwareVersion, "Software that created this stack", status);
    writeLongKey(fptr, "RAWCOUNT", static_cast<long long>(m.framesCaptured), "Frames captured in burst window", status);
    writeLongKey(fptr, "STACKCNT", static_cast<long long>(m.framesKept), "Best frames stacked", status);
    writeDoubleKey(fptr, "QUALSCOR", m.qualityScore, "Normalized burst quality score", status);
    writeDoubleKey(fptr, "ALIGNDX", m.shiftX, "Session alignment X shift (px)", status);
    writeDoubleKey(fptr, "ALIGNDY", m.shiftY, "Session alignment Y shift (px)", status);
    writeDoubleKey(fptr, "ALIGNROT", m.rotationDeg, "Session alignment rotation (deg)", status);
    writeStrKey(fptr, "SHARPPRO", m.sharpeningProfile, "Sharpening profile applied", status);
    writeDoubleKey(fptr, "RAWSCALE", m.rawMaxAdu, "Native full-scale ADU corresponding to 1.0", status);

    fits_write_img(fptr, TFLOAT, 1, static_cast<LONGLONG>(image.pixels.size()),
        const_cast<float*>(image.pixels.data()), &status);

    int closeStatus = 0;
    fits_close_file(fptr, &closeStatus);
    if (status != 0 || closeStatus != 0)
    {
        error = QString("CFITSIO write failed for %1: %2").arg(path, fitsErrorText(status != 0 ? status : closeStatus));
        QFile::remove(tmpPath);
        return false;
    }

    QFile::remove(path);
    if (!QFile::rename(tmpPath, path))
    {
        error = QString("could not finalize FITS file %1").arg(path);
        QFile::remove(tmpPath);
        return false;
    }
    return true;
}

bool readFitsFloat(const QString& path, FitsImage& image, QString& error)
{
    int status = 0;
    fitsfile* fptr = nullptr;
    const QByteArray encoded = QFile::encodeName(path);
    if (fits_open_diskfile(&fptr, encoded.constData(), READONLY, &status) != 0)
    {
        error = QString("CFITSIO open failed for %1: %2").arg(path, fitsErrorText(status));
        return false;
    }

    int bitpix = 0;
    int naxis = 0;
    long naxes[3]{0, 0, 1};
    fits_get_img_param(fptr, 3, &bitpix, &naxis, naxes, &status);
    if (status != 0 || (naxis != 2 && !(naxis == 3 && (naxes[2] == 1 || naxes[2] == 3)))
        || naxes[0] <= 0 || naxes[1] <= 0)
    {
        error = status != 0
            ? QString("CFITSIO read header failed: %1").arg(fitsErrorText(status))
            : QString("unsupported FITS image dimensions (NAXIS=%1)").arg(naxis);
        int cs = 0;
        fits_close_file(fptr, &cs);
        return false;
    }

    FitsImage result;
    result.width = static_cast<int>(naxes[0]);
    result.height = static_cast<int>(naxes[1]);
    result.channels = (naxis == 3 && naxes[2] == 3) ? 3 : 1;
    const size_t total = static_cast<size_t>(result.width) * result.height * result.channels;
    result.pixels.resize(total);

    float nulval = 0.0f;
    int anynul = 0;
    fits_read_img(fptr, TFLOAT, 1, static_cast<LONGLONG>(total), &nulval, result.pixels.data(), &anynul, &status);

    // Normalize external integer FITS (e.g., 8-bit or 16-bit Siril/SharpCap files) into [0, 1].
    if (status == 0 && bitpix > 0)
    {
        const float maxPx = *std::max_element(result.pixels.begin(), result.pixels.end());
        if (maxPx > 1.0f)
        {
            const float denom = bitpix == 8 ? 255.0f : (maxPx > 4095.0f ? 65535.0f : 4095.0f);
            for (float& p : result.pixels) p /= denom;
        }
    }

    FitsMetadata& m = result.meta;
    m.dateObs = parseFitsUtc(readStrKey(fptr, "DATE-OBS").c_str());
    m.dateAvg = parseFitsUtc(readStrKey(fptr, "DATE-AVG").c_str());
    m.dateEnd = parseFitsUtc(readStrKey(fptr, "DATE-END").c_str());
    (void)readDoubleKey(fptr, "EXPTIME", m.exptimeSeconds);
    m.hasGain = readDoubleKey(fptr, "GAIN", m.gain);
    m.camera = readStrKey(fptr, "INSTRUME");
    m.telescope = readStrKey(fptr, "TELESCOP");
    m.observer = readStrKey(fptr, "OBSERVER");
    m.siteName = readStrKey(fptr, "SITENAME");
    m.targetPreset = readStrKey(fptr, "OBJECT");
    m.softwareVersion = readStrKey(fptr, "SWCREATE");
    (void)readLongKey(fptr, "RAWCOUNT", m.framesCaptured);
    (void)readLongKey(fptr, "STACKCNT", m.framesKept);
    (void)readDoubleKey(fptr, "QUALSCOR", m.qualityScore);
    (void)readDoubleKey(fptr, "ALIGNDX", m.shiftX);
    (void)readDoubleKey(fptr, "ALIGNDY", m.shiftY);
    (void)readDoubleKey(fptr, "ALIGNROT", m.rotationDeg);
    m.sharpeningProfile = readStrKey(fptr, "SHARPPRO");
    if (m.sharpeningProfile.empty()) m.sharpeningProfile = "none";
    (void)readDoubleKey(fptr, "RAWSCALE", m.rawMaxAdu);

    int cs = 0;
    fits_close_file(fptr, &cs);
    if (status != 0)
    {
        error = QString("CFITSIO read pixels failed: %1").arg(fitsErrorText(status));
        return false;
    }

    image = std::move(result);
    return true;
}
