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

enum class BrightnessLevellingMode
{
    Off,
    Global,          // Robust affine match: removes uniform cloud dimming & haze while preserving shadows
    GlobalAndPatchy  // Global match + smooth low-frequency spatial grid for uneven clouds (Moon)
};

enum class QualityFilterMode
{
    LocalMedian,     // Compare each stack against median of surrounding neighbors (robust to airmass/shadow drift)
    GlobalMedian,    // Compare each stack against overall session median
    KeepBestPct      // Keep the sharpest N% of stacks overall
};

// Feature-32: a time-lapse also ends with the session's Ends setting; the
// window stops it then. The values are those saved before Duration and the
// UTC end time moved there.
enum class StopConditionKind
{
    Manual = 0,
    StackCount = 3
};

[[nodiscard]] const char* timelapsePresetName(TimelapsePreset preset);
[[nodiscard]] const char* sharpeningPresetName(SharpeningPreset preset);
[[nodiscard]] const char* brightnessLevellingModeName(BrightnessLevellingMode mode);
[[nodiscard]] const char* qualityFilterModeName(QualityFilterMode mode);

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
    BrightnessLevellingMode brightnessMode{BrightnessLevellingMode::Global};
    QualityFilterMode qualityFilterMode{QualityFilterMode::LocalMedian};
    double minQualityRatio{0.0}; // 0 keeps all stacks; >0 drops stacks below ratio * local/global median quality
    double keepBestPercent{100.0}; // KeepBestPct mode: 100 keeps all; <100 keeps sharpest N% of stacks
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
    int targetStacks{60};
    bool saveRawSer{false};
    bool watchTransits{false};
    SharpeningSettings sharpening;
    VideoToneSettings video;

    [[nodiscard]] bool valid() const;
};

[[nodiscard]] SharpeningSettings sharpeningForPreset(SharpeningPreset preset);
[[nodiscard]] TimelapseSettings defaultTimelapseSettings(TimelapsePreset preset);
[[nodiscard]] QString sanitizeProfileName(const QString& name);

bool saveProfileJson(const QString& path, const SharpeningSettings& sharpening,
    const VideoToneSettings& video, QString& error,
    const std::optional<QRect>& commonOverlap = std::nullopt);
bool loadProfileJson(const QString& path, SharpeningSettings& sharpening,
    VideoToneSettings& video, QString& error,
    std::optional<QRect>* outCommonOverlap = nullptr);

struct SavedSharpeningProfile
{
    QString name;
    SharpeningSettings sharpening;
    VideoToneSettings video;
};

[[nodiscard]] std::vector<SavedSharpeningProfile> loadSavedSharpeningProfiles();
void saveNamedSharpeningProfile(const SavedSharpeningProfile& profile);
void removeNamedSharpeningProfile(const QString& name);
