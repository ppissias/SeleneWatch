#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QRect>
#include <QtCore/QString>

#include <optional>
#include <vector>

enum class TimelapsePreset
{
    Moon,
    Planet
};

enum class SharpeningPreset
{
    Off,
    Mild,
    Medium,
    Custom
};

enum class StopConditionKind
{
    Manual,
    Duration,
    UtcEnd,
    StackCount
};

[[nodiscard]] const char* timelapsePresetName(TimelapsePreset preset);
[[nodiscard]] const char* sharpeningPresetName(SharpeningPreset preset);

// Linear float sharpening settings applied identically to every stack in
// sharpened/ and in Re-sharpen (FITS remains linear; video stretch is separate).
struct SharpeningSettings
{
    SharpeningPreset preset{SharpeningPreset::Mild};
    double radius{1.4};       // Gaussian blur sigma in pixels (unsharp mask)
    double amount{0.6};       // Unsharp mask strength
    double threshold{0.004};  // Soft noise-floor threshold in normalized [0, 1] units
    double denoise{0.0};      // Optional light pre-sharpen denoise [0, 1]
    QString profileName{"Mild"};

    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool operator==(const SharpeningSettings&) const = default;
};

// Display/video tone-mapping and assembly settings applied only during MP4/GIF
// rendering, leaving FITS stacks untouched.
struct VideoToneSettings
{
    bool autoStretch{true};
    double blackLevel{0.0};
    double whiteLevel{1.0};
    double gamma{1.0};
    bool brightnessLevelling{true};
    double minQualityRatio{0.0}; // 0 keeps all stacks; >0 drops stacks below ratio * reference quality
    bool cropToOverlap{true};
    int fps{12};
    bool writeMp4{true};
    bool writeGif{false};

    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool operator==(const VideoToneSettings&) const = default;
};

struct TimelapseSettings
{
    static constexpr int kMaxFramesCap = 200;

    TimelapsePreset preset{TimelapsePreset::Moon};
    double intervalSeconds{60.0};
    double captureWindowSeconds{15.0};
    double keepPercent{25.0};
    int maxFramesPerPeriod{150};
    bool cropToLock{false};
    std::optional<QRect> region;
    // Optional sunlit lunar area expected to keep steady illumination across
    // the session for rigid translation + rotation alignment.
    std::optional<QRect> lunarAnchor;
    StopConditionKind stopKind{StopConditionKind::Manual};
    double durationMinutes{60.0};
    QDateTime utcEndTime;
    int targetStacks{60};
    bool saveRawSer{false};
    SharpeningSettings sharpening;
    VideoToneSettings video;

    [[nodiscard]] bool valid() const;
};

[[nodiscard]] SharpeningSettings sharpeningForPreset(SharpeningPreset preset);
[[nodiscard]] TimelapseSettings defaultTimelapseSettings(TimelapsePreset preset);
[[nodiscard]] QString sanitizeProfileName(const QString& name);

bool saveProfileJson(const QString& path, const SharpeningSettings& sharpening,
    const VideoToneSettings& video, QString& error);
bool loadProfileJson(const QString& path, SharpeningSettings& sharpening,
    VideoToneSettings& video, QString& error);

struct SavedSharpeningProfile
{
    QString name;
    SharpeningSettings sharpening;
    VideoToneSettings video;
};

[[nodiscard]] std::vector<SavedSharpeningProfile> loadSavedSharpeningProfiles();
void saveNamedSharpeningProfile(const SavedSharpeningProfile& profile);
void removeNamedSharpeningProfile(const QString& name);
