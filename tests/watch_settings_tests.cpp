#include "settings/detection_profiles.h"
#include "settings/watch_settings.h"
#include "detection/target_defaults.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtTest/QtTest>

// Design-1: each watch's saved settings, kept by settings/watch_settings. Every
// test runs twice: in the per-user registry the program uses by default, where
// values keep their types, and in the portable SeleneWatch.ini, where they come
// back as text.
class WatchSettingsTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void eachWatchKeepsItsOwnSettings_data() { formats(); }
    void eachWatchKeepsItsOwnSettings();
    void theKeysEarlierVersionsWroteAreRead_data() { formats(); }
    void theKeysEarlierVersionsWroteAreRead();
    void namedProfilesSaveTheSameFields_data() { formats(); }
    void namedProfilesSaveTheSameFields();
    void invalidValuesReadAsTheWatchsDefaults_data() { formats(); }
    void invalidValuesReadAsTheWatchsDefaults();

private:
    static void formats();
    QTemporaryDir ini_;
};

namespace {

constexpr Watch kWatches[] = {Watch::MoonFlashes, Watch::MoonShadows, Watch::Sky, Watch::SkySlow};

// A valid detection setting that differs for each watch, with a sky mask.
DetectionSettings distinctDetection(Watch watch, double sigma)
{
    DetectionSettings settings = defaultDetectionSettings(watch);
    settings.sigma = sigma;
    settings.minPixels = static_cast<int>(sigma);
    settings.referenceTimeConstantSeconds = sigma / 2;
    if (watchTarget(watch) == Target::Sky)
    {
        settings.releaseSeconds = sigma * 3;
        settings.mask = DetectionMask::fromText("10,10,40,30");
    }
    return settings;
}

} // namespace

void WatchSettingsTest::formats()
{
    QTest::addColumn<bool>("ini");
    QTest::newRow("registry") << false;
    QTest::newRow("ini") << true;
}

void WatchSettingsTest::init()
{
    QFETCH(bool, ini);
    QSettings::setDefaultFormat(ini ? QSettings::IniFormat : QSettings::NativeFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, ini_.path());
    QSettings().clear();
}

void WatchSettingsTest::cleanup()
{
    QSettings saved;
    saved.clear();
    saved.sync();
}

void WatchSettingsTest::eachWatchKeepsItsOwnSettings()
{
    double sigma = 7;
    for (const Watch watch : kWatches)
    {
        saveDetectionSettings(watch, distinctDetection(watch, sigma));
        sigma += 1;
    }
    CaptureSettings moon = defaultCaptureSettings(Watch::MoonFlashes);
    moon.historySeconds = 7;
    saveCaptureSettings(Watch::MoonShadows, moon);
    CaptureSettings sky = defaultCaptureSettings(Watch::Sky);
    sky.historySeconds = 4;
    saveCaptureSettings(Watch::Sky, sky);
    CaptureSettings slow = defaultCaptureSettings(Watch::SkySlow);
    slow.historySeconds = 30;
    slow.diskReserveBytes = uint64_t{3} * 1024 * 1024 * 1024;
    saveCaptureSettings(Watch::SkySlow, slow);
    SkyExtras fast;
    fast.follow = true;
    fast.maxExposureMs = 40;
    saveSkyExtras(Watch::Sky, fast);
    SkyExtras slowFollow;
    slowFollow.maxGain = 300;
    saveSkyExtras(Watch::SkySlow, slowFollow);
    saveLongestExposureMs(Watch::Sky, 33);
    saveLongestExposureMs(Watch::SkySlow, 1500);
    saveLongestExposureMs(Watch::MoonFlashes, 99);
    saveWatchTimelapse({true, 60});
    saveWatch(Watch::SkySlow);

    sigma = 7;
    for (const Watch watch : kWatches)
    {
        const DetectionSettings read = savedDetectionSettings(watch);
        QCOMPARE(detectionFields(read, watch), detectionFields(distinctDetection(watch, sigma), watch));
        QCOMPARE(read.mask.text(), distinctDetection(watch, sigma).mask.text());
        sigma += 1;
    }
    // Both Moon watches share the Moon's capture settings; each sky watch has its own.
    QVERIFY(sameCaptureSettings(Watch::MoonFlashes, Watch::MoonShadows));
    QVERIFY(!sameCaptureSettings(Watch::Sky, Watch::SkySlow));
    QCOMPARE(savedCaptureSettings(Watch::MoonFlashes).historySeconds, 7.0);
    QCOMPARE(savedCaptureSettings(Watch::Sky).historySeconds, 4.0);
    QCOMPARE(savedCaptureSettings(Watch::SkySlow).historySeconds, 30.0);
    QCOMPARE(savedCaptureSettings(Watch::SkySlow).diskReserveBytes, slow.diskReserveBytes);
    QCOMPARE(savedSkyExtras(Watch::Sky), fast);
    QCOMPARE(savedSkyExtras(Watch::SkySlow).maxGain, 300.0);
    QCOMPARE(savedSkyExtras(Watch::MoonFlashes), fast); // The Moon has no follower of its own.
    QCOMPARE(savedLongestExposureMs(Watch::Sky), 33.0);
    QCOMPARE(savedLongestExposureMs(Watch::SkySlow), 1500.0);
    QCOMPARE(savedLongestExposureMs(Watch::MoonFlashes), defaultLongestExposureMs(Watch::MoonFlashes));
    QCOMPARE(savedWatchTimelapse(), (WatchTimelapse{true, 60}));
    QCOMPARE(savedWatch(), Watch::SkySlow);
    saveWatch(Watch::MoonShadows);
    QCOMPARE(savedWatch(), Watch::MoonShadows);
}

// The keys an observer's settings already hold, written by earlier versions,
// read unchanged. The slow sky's longest exposure is under skySlow/ (Design-1).
void WatchSettingsTest::theKeysEarlierVersionsWroteAreRead()
{
    {
        QSettings saved;
        saved.setValue("detection/sigma", 7.5);
        saved.setValue("moonShadows/detection/sigma", 8.5);
        saved.setValue("sky/detection/sigma", 9.5);
        saved.setValue("sky/detection/mask", "1,2,30,40");
        saved.setValue("skySlow/detection/sigma", 10.5);
        saved.setValue("capture/history", 6.0);
        saved.setValue("sky/capture/history", 2.0);
        saved.setValue("skySlow/capture/history", 25.0);
        saved.setValue("capture/memoryMiB", 1024);
        saved.setValue("capture/diskReserveGiB", 5);
        saved.setValue("sky/follow/enabled", true);
        saved.setValue("sky/follow/maxExposureMs", 2000.0);
        saved.setValue("skySlow/follow/maxGain", 250.0);
        saved.setValue("skySlow/timelapse/enabled", true);
        saved.setValue("skySlow/timelapse/intervalSeconds", 45);
        saved.setValue("sky/autotune/longestExposureMs", 30.0);
        saved.setValue("skySlow/autotune/longestExposureMs", 1800.0);
        saved.setValue("target", 1);
        saved.setValue("sky/watch", "fast");
    }
    QCOMPARE(savedDetectionSettings(Watch::MoonFlashes).sigma, 7.5);
    QCOMPARE(savedDetectionSettings(Watch::MoonShadows).sigma, 8.5);
    QCOMPARE(savedDetectionSettings(Watch::Sky).sigma, 9.5);
    QCOMPARE(savedDetectionSettings(Watch::Sky).mask.text(), DetectionMask::fromText("1,2,30,40").text());
    QCOMPARE(savedDetectionSettings(Watch::SkySlow).sigma, 10.5);
    QCOMPARE(savedCaptureSettings(Watch::MoonShadows).historySeconds, 6.0);
    QCOMPARE(savedCaptureSettings(Watch::MoonFlashes).memoryBytes, size_t{1024} * 1024 * 1024);
    QCOMPARE(savedCaptureSettings(Watch::MoonFlashes).diskReserveBytes, uint64_t{5} * 1024 * 1024 * 1024);
    QCOMPARE(savedCaptureSettings(Watch::Sky).historySeconds, 2.0);
    QCOMPARE(savedCaptureSettings(Watch::SkySlow).historySeconds, 25.0);
    QVERIFY(savedSkyExtras(Watch::Sky).follow);
    QCOMPARE(savedSkyExtras(Watch::Sky).maxExposureMs, 2000.0);
    QCOMPARE(savedSkyExtras(Watch::SkySlow).maxGain, 250.0);
    QCOMPARE(savedSkyExtras(Watch::SkySlow).maxExposureMs, defaultLongestExposureMs(Watch::SkySlow));
    QCOMPARE(savedWatchTimelapse(), (WatchTimelapse{true, 45}));
    QCOMPARE(savedLongestExposureMs(Watch::Sky), 30.0);
    QCOMPARE(savedLongestExposureMs(Watch::SkySlow), 1800.0);
    QCOMPARE(savedWatch(), Watch::Sky);
    QSettings().setValue("sky/watch", "slow");
    QCOMPARE(savedWatch(), Watch::SkySlow);
    QSettings().setValue("target", 0);
    QSettings().setValue("moon/watch", "shadows");
    QCOMPARE(savedWatch(), Watch::MoonShadows);
}

// A named profile stores the same fields as the watch's settings, and reads back as it was saved.
void WatchSettingsTest::namedProfilesSaveTheSameFields()
{
    for (const Watch watch : kWatches)
    {
        SavedDetectionProfile profile;
        profile.name = "Night one";
        profile.watch = watch;
        profile.settings = distinctDetection(watch, 9);
        profile.hasMask = watchTarget(watch) == Target::Sky;
        profile.mask = profile.settings.mask;
        saveNamedDetectionProfile(profile);

        const auto profiles = loadSavedDetectionProfiles(watch);
        QCOMPARE(profiles.size(), size_t{1});
        QCOMPARE(detectionFields(profiles.front().settings, watch), detectionFields(profile.settings, watch));
        QCOMPARE(profiles.front().mask.text(), profile.mask.text());

        QSettings saved;
        const QJsonObject stored = QJsonDocument::fromJson(
            saved.value(detectionProfilesGroup(watch) + "/Night_one/json").toByteArray()).object();
        QCOMPARE(stored["settings"].toObject().keys(), detectionFields(profile.settings, watch).keys());
        removeNamedDetectionProfile(watch, profile.name);
        QVERIFY(loadSavedDetectionProfiles(watch).empty());
    }
    // The profiles saved before the Moon had two watches are the flashes'.
    QCOMPARE(detectionProfilesGroup(Watch::MoonFlashes), QString("detection/profiles/moon"));
}

void WatchSettingsTest::invalidValuesReadAsTheWatchsDefaults()
{
    QSettings().setValue("skySlow/detection/sigma", -3.0);
    QSettings().setValue("sky/capture/history", -1.0);
    QSettings().setValue("skySlow/timelapse/intervalSeconds", 99999);
    QCOMPARE(detectionFields(savedDetectionSettings(Watch::SkySlow), Watch::SkySlow),
        detectionFields(defaultDetectionSettings(Watch::SkySlow), Watch::SkySlow));
    QCOMPARE(savedCaptureSettings(Watch::Sky).historySeconds, defaultCaptureSettings(Watch::Sky).historySeconds);
    QCOMPARE(savedWatchTimelapse().intervalSeconds, 3600);
    // Darkening is the shadows' alone, whatever was saved.
    QSettings().setValue("detection/darkening", true);
    QVERIFY(!savedDetectionSettings(Watch::MoonFlashes).darkening);
    QVERIFY(savedDetectionSettings(Watch::MoonShadows).darkening);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("SeleneWatchTests");
    QCoreApplication::setApplicationName("WatchSettings-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    WatchSettingsTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "watch_settings_tests.moc"
