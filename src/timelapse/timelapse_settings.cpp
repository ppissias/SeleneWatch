#include "timelapse/timelapse_settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>

#include <algorithm>
#include <cmath>

const char* timelapsePresetName(TimelapsePreset preset)
{
    return preset == TimelapsePreset::Planet ? "planet" : "moon";
}

const char* sharpeningPresetName(SharpeningPreset preset)
{
    switch (preset)
    {
    case SharpeningPreset::Off: return "off";
    case SharpeningPreset::Mild: return "mild";
    case SharpeningPreset::Medium: return "medium";
    case SharpeningPreset::Custom: return "custom";
    }
    return "mild";
}

bool SharpeningSettings::valid() const
{
    return std::isfinite(radius) && radius >= 0.3 && radius <= 20.0
        && std::isfinite(amount) && amount >= 0.0 && amount <= 10.0
        && std::isfinite(threshold) && threshold >= 0.0 && threshold <= 0.5
        && std::isfinite(denoise) && denoise >= 0.0 && denoise <= 1.0;
}

bool VideoToneSettings::valid() const
{
    return std::isfinite(blackLevel) && std::isfinite(whiteLevel)
        && blackLevel >= 0.0 && whiteLevel <= 1.0 && blackLevel < whiteLevel
        && std::isfinite(gamma) && gamma >= 0.2 && gamma <= 4.0
        && std::isfinite(minQualityRatio) && minQualityRatio >= 0.0 && minQualityRatio <= 0.99
        && fps >= 1 && fps <= 60;
}

bool TimelapseSettings::valid() const
{
    if (watchTransits && preset != TimelapsePreset::Moon) return false;
    if (!std::isfinite(intervalSeconds) || intervalSeconds < 0.2 || intervalSeconds > 86400.0) return false;
    if (!std::isfinite(captureWindowSeconds) || captureWindowSeconds < 0.1 || captureWindowSeconds > intervalSeconds) return false;
    if (!std::isfinite(keepPercent) || keepPercent < 1.0 || keepPercent > 100.0) return false;
    if (maxFramesPerPeriod < 1 || maxFramesPerPeriod > kMaxFramesCap) return false;
    if (stopKind == StopConditionKind::StackCount && targetStacks < 1) return false;
    if (!sharpening.valid() || !video.valid()) return false;
    return true;
}

SharpeningSettings sharpeningForPreset(SharpeningPreset preset)
{
    SharpeningSettings s;
    s.preset = preset;
    switch (preset)
    {
    case SharpeningPreset::Off:
        s.amount = 0.0;
        s.denoise = 0.0;
        s.profileName = "Off";
        break;
    case SharpeningPreset::Mild:
        s.radius = 1.4;
        s.amount = 0.6;
        s.threshold = 0.004;
        s.denoise = 0.0;
        s.profileName = "Mild";
        break;
    case SharpeningPreset::Medium:
        s.radius = 1.8;
        s.amount = 1.2;
        s.threshold = 0.004;
        s.denoise = 0.1;
        s.profileName = "Medium";
        break;
    case SharpeningPreset::Custom:
        s.profileName = "Custom";
        break;
    }
    return s;
}

TimelapseSettings defaultTimelapseSettings(TimelapsePreset preset)
{
    TimelapseSettings s;
    s.preset = preset;
    s.intervalSeconds = 60.0;
    s.captureWindowSeconds = 15.0;
    if (preset == TimelapsePreset::Planet)
    {
        s.keepPercent = 15.0;
        s.maxFramesPerPeriod = 200;
    }
    else
    {
        s.keepPercent = 25.0;
        s.maxFramesPerPeriod = 150;
    }
    s.sharpening = sharpeningForPreset(SharpeningPreset::Mild);
    s.video.fps = 12;
    return s;
}

QString sanitizeProfileName(const QString& name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name.trimmed())
    {
        if (ch.isLetterOrNumber() || ch == '_' || ch == '-')
            out.append(ch);
        else if (ch.isSpace())
            out.append('_');
    }
    while (out.contains("__")) out.replace("__", "_");
    if (out.isEmpty()) out = "profile";
    return out.left(40);
}

namespace {

QJsonObject toJson(const SharpeningSettings& s)
{
    QJsonObject obj;
    obj["preset"] = sharpeningPresetName(s.preset);
    obj["profileName"] = s.profileName;
    obj["radius"] = s.radius;
    obj["amount"] = s.amount;
    obj["threshold"] = s.threshold;
    obj["denoise"] = s.denoise;
    return obj;
}

QJsonObject toJson(const VideoToneSettings& v)
{
    QJsonObject obj;
    obj["autoStretch"] = v.autoStretch;
    obj["blackLevel"] = v.blackLevel;
    obj["whiteLevel"] = v.whiteLevel;
    obj["gamma"] = v.gamma;
    obj["brightnessLevelling"] = v.brightnessLevelling;
    obj["minQualityRatio"] = v.minQualityRatio;
    obj["cropToOverlap"] = v.cropToOverlap;
    obj["fps"] = v.fps;
    obj["writeMp4"] = v.writeMp4;
    obj["writeGif"] = v.writeGif;
    return obj;
}

SharpeningSettings sharpeningFromJson(const QJsonObject& obj)
{
    SharpeningSettings s;
    const QString preset = obj["preset"].toString("mild");
    if (preset == "off") s.preset = SharpeningPreset::Off;
    else if (preset == "medium") s.preset = SharpeningPreset::Medium;
    else if (preset == "custom") s.preset = SharpeningPreset::Custom;
    else s.preset = SharpeningPreset::Mild;
    s.profileName = obj["profileName"].toString(s.profileName);
    s.radius = obj["radius"].toDouble(s.radius);
    s.amount = obj["amount"].toDouble(s.amount);
    s.threshold = obj["threshold"].toDouble(s.threshold);
    s.denoise = obj["denoise"].toDouble(s.denoise);
    return s.valid() ? s : SharpeningSettings{};
}

VideoToneSettings videoFromJson(const QJsonObject& obj)
{
    VideoToneSettings v;
    v.autoStretch = obj["autoStretch"].toBool(v.autoStretch);
    v.blackLevel = obj["blackLevel"].toDouble(v.blackLevel);
    v.whiteLevel = obj["whiteLevel"].toDouble(v.whiteLevel);
    v.gamma = obj["gamma"].toDouble(v.gamma);
    v.brightnessLevelling = obj["brightnessLevelling"].toBool(v.brightnessLevelling);
    v.minQualityRatio = obj["minQualityRatio"].toDouble(v.minQualityRatio);
    v.cropToOverlap = obj["cropToOverlap"].toBool(v.cropToOverlap);
    v.fps = obj["fps"].toInt(v.fps);
    v.writeMp4 = obj["writeMp4"].toBool(v.writeMp4);
    v.writeGif = obj["writeGif"].toBool(v.writeGif);
    return v.valid() ? v : VideoToneSettings{};
}

} // namespace

bool saveProfileJson(const QString& path, const SharpeningSettings& sharpening,
    const VideoToneSettings& video, QString& error,
    const std::optional<QRect>& commonOverlap)
{
    QJsonObject root;
    root["version"] = 1;
    root["sharpening"] = toJson(sharpening);
    root["video"] = toJson(video);
    if (commonOverlap && commonOverlap->isValid() && !commonOverlap->isEmpty())
    {
        QJsonObject ov;
        ov["x"] = commonOverlap->x();
        ov["y"] = commonOverlap->y();
        ov["width"] = commonOverlap->width();
        ov["height"] = commonOverlap->height();
        root["commonOverlap"] = ov;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        error = QString("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.flush())
    {
        error = QString("Could not flush %1").arg(path);
        return false;
    }
    return true;
}

bool loadProfileJson(const QString& path, SharpeningSettings& sharpening,
    VideoToneSettings& video, QString& error,
    std::optional<QRect>* outCommonOverlap)
{
    if (outCommonOverlap) outCommonOverlap->reset();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QString("Could not open %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    {
        error = QString("Invalid profile JSON in %1: %2").arg(path, parseError.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.contains("sharpening") && root["sharpening"].isObject())
        sharpening = sharpeningFromJson(root["sharpening"].toObject());
    if (root.contains("video") && root["video"].isObject())
        video = videoFromJson(root["video"].toObject());
    if (outCommonOverlap && root.contains("commonOverlap") && root["commonOverlap"].isObject())
    {
        const QJsonObject ov = root["commonOverlap"].toObject();
        const int x = ov["x"].toInt(-1);
        const int y = ov["y"].toInt(-1);
        const int w = ov["width"].toInt(0);
        const int h = ov["height"].toInt(0);
        if (x >= 0 && y >= 0 && w >= 2 && h >= 2)
            *outCommonOverlap = QRect(x, y, w, h);
    }
    return true;
}

std::vector<SavedSharpeningProfile> loadSavedSharpeningProfiles()
{
    std::vector<SavedSharpeningProfile> list;
    QSettings saved;
    saved.beginGroup("timelapse/profiles");
    for (const QString& key : saved.childGroups())
    {
        saved.beginGroup(key);
        const QJsonDocument doc = QJsonDocument::fromJson(saved.value("json").toByteArray());
        if (doc.isObject())
        {
            const QJsonObject obj = doc.object();
            SavedSharpeningProfile p;
            p.name = obj["name"].toString(key);
            p.sharpening = sharpeningFromJson(obj["sharpening"].toObject());
            p.video = videoFromJson(obj["video"].toObject());
            list.push_back(std::move(p));
        }
        saved.endGroup();
    }
    saved.endGroup();
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return list;
}

void saveNamedSharpeningProfile(const SavedSharpeningProfile& profile)
{
    if (profile.name.trimmed().isEmpty()) return;
    QJsonObject obj;
    obj["name"] = profile.name.trimmed();
    obj["sharpening"] = toJson(profile.sharpening);
    obj["video"] = toJson(profile.video);
    QSettings saved;
    saved.beginGroup("timelapse/profiles");
    saved.beginGroup(sanitizeProfileName(profile.name));
    saved.setValue("json", QJsonDocument(obj).toJson(QJsonDocument::Compact));
    saved.endGroup();
    saved.endGroup();
}

void removeNamedSharpeningProfile(const QString& name)
{
    QSettings saved;
    saved.beginGroup("timelapse/profiles");
    saved.remove(sanitizeProfileName(name));
    saved.endGroup();
}
