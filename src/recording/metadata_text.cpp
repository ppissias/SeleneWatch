#include "recording/metadata_text.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStorageInfo>
#include <QtCore/QTimeZone>

QDateTime utcDateTime(std::chrono::system_clock::time_point time)
{
    const qint64 epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC);
}

QString utcStamp(std::chrono::system_clock::time_point time)
{
    return utcDateTime(time).toString("yyyyMMdd-HHmmss-zzz");
}

QString utcText(std::chrono::system_clock::time_point time)
{
    return utcDateTime(time).toString(Qt::ISODateWithMs);
}

QString orUnknown(const std::string& text)
{
    return text.empty() ? QString("unknown") : QString::fromStdString(text);
}

void writeStreamLines(QTextStream& out, const StreamInfo& stream, const Frame* layout)
{
    out << "source: " << orUnknown(stream.source) << "\n";
    out << "camera: " << orUnknown(stream.camera) << "\n";
    out << "exposure: " << orUnknown(stream.exposure) << "\n";
    out << "gain: " << orUnknown(stream.gain) << "\n";
    out << "camera_serial: " << orUnknown(stream.serial) << "\n";
    out << "camera_sdk_version: " << orUnknown(stream.sdkVersion) << "\n";
    out << "camera_device_id: " << orUnknown(stream.deviceId) << "\n";
    out << "transfer_speed: " << orUnknown(stream.transferSpeed) << "\n";
    out << "offset: " << orUnknown(stream.offset) << "\n";
    out << "binning: " << orUnknown(stream.binning) << "\n";
    out << "roi: " << orUnknown(stream.roi) << "\n";
    out << "sensor_adc_bits: " << orUnknown(stream.sensorBits) << "\n";
    out << "sample_layout_policy: " << orUnknown(stream.sampleLayout) << "\n";
    out << "usb_bandwidth: " << orUnknown(stream.usbBandwidth) << "\n";
    out << "high_speed: " << orUnknown(stream.highSpeed) << "\n";
    out << "frame_rate: " << orUnknown(stream.frameRate) << "\n";
    out << "frame_time: " << orUnknown(stream.frameTime) << "\n";
    out << "frame_index: " << orUnknown(stream.frameIndex) << "\n";
    out << "conversion: " << orUnknown(stream.conversion) << "\n";
    if (!stream.playback.empty())
    {
        out << "playback: " << QString::fromStdString(stream.playback) << "\n";
    }
    if (!layout)
    {
        return;
    }
    out << "frame_size: " << layout->width << "x" << layout->height << "\n";
    out << "pixel_format: " << pixelFormatName(layout->format) << "\n";
    out << "significant_bits: " << layout->significantBits << "\n";
    out << "bit_alignment: " << (layout->alignment == BitAlignment::High ? "high" : "low") << "\n";
    out << "bayer_pattern: " << bayerPatternName(layout->bayer) << "\n";
}

namespace {

QString textOr(const std::string& text)
{
    return text.empty() ? QString("not entered") : QString::fromStdString(text);
}

QString numberOr(const std::optional<double>& value, int decimals)
{
    return value ? QString::number(*value, 'f', decimals) : QString("not entered");
}

QString positiveOr(double value, int decimals)
{
    return value > 0 ? QString::number(value, 'f', decimals) : QString("not entered");
}

} // namespace

void writeSiteLines(QTextStream& out, const ObservingSite& site)
{
    out << "observer: " << textOr(site.observer) << "\n";
    out << "site: " << textOr(site.site) << "\n";
    out << "site_latitude_deg: " << numberOr(site.latitudeDeg, 5) << "\n";
    out << "site_longitude_deg: " << numberOr(site.longitudeDeg, 5) << "\n";
    out << "site_height_m: " << numberOr(site.heightM, 0) << "\n";
    out << "telescope: " << textOr(site.telescope) << "\n";
    out << "aperture_mm: " << positiveOr(site.apertureMm, 0) << "\n";
    out << "focal_length_mm: " << positiveOr(site.focalLengthMm, 0) << "\n";
    out << "pixel_size_um: " << positiveOr(site.pixelSizeUm, 2) << "\n";
    const auto scale = site.arcsecPerPixel();
    out << "image_scale_arcsec_per_pixel: " << (scale ? QString::number(*scale, 'f', 3) : QString("unknown")) << "\n";
}

QString siteFields(const ObservingSite& site)
{
    const auto scale = site.arcsecPerPixel();
    return QString(" observer=\"%1\" site=\"%2\" latitude_deg=%3 longitude_deg=%4 height_m=%5 telescope=\"%6\" "
                   "aperture_mm=%7 focal_length_mm=%8 pixel_size_um=%9 image_scale_arcsec_per_pixel=%10")
        .arg(textOr(site.observer), textOr(site.site), numberOr(site.latitudeDeg, 5), numberOr(site.longitudeDeg, 5),
            numberOr(site.heightM, 0), textOr(site.telescope), positiveOr(site.apertureMm, 0),
            positiveOr(site.focalLengthMm, 0), positiveOr(site.pixelSizeUm, 2))
        .arg(scale ? QString::number(*scale, 'f', 3) : QString("unknown"));
}

std::optional<uint64_t> freeBytesAt(const QString& folder)
{
    QString at = QDir::cleanPath(QDir(folder).absolutePath());
    while (!QFileInfo::exists(at))
    {
        const QString parent = QFileInfo(at).path();
        if (parent == at) return std::nullopt;
        at = parent;
    }
    QStorageInfo storage(at);
    if (!storage.isValid() || !storage.isReady()) return std::nullopt;
    return static_cast<uint64_t>(storage.bytesAvailable());
}
