#include "detection_panel.h"
#include "display_panel.h"
#include "camera_panel.h"
#include "elided_label.h"
#include "guiding_panel.h"
#include "tests/fake_zwo_api.h"
#include "tests/fake_qhy_api.h"
#include "qhy_camera_panel.h"
#include "mainwindow.h"
#include "preview_widget.h"
#include "recording/ser_reader.h"
#include "recording/ser_writer.h"
#include "sources/synthetic_scene.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QUuid>
#include <QtGui/QAction>
#include <QtGui/QFontDatabase>
#include <QtGui/QHelpEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace {
QString text(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}
}

// Drives the offscreen window to check controls, saved preferences and capture workflows.
class UiTest : public QObject
{
    Q_OBJECT
private slots:
    void init() { QSettings saved; saved.clear(); saved.sync(); }
    void controlsPreserveValidSizeLimits();
    void detectionPanelExplainsPatchRejections();
    void readoutsKeepTheControlsBelowInPlace();
    void eachTargetKeepsItsOwnSettings();
    void statusMessagesHaveTheirOwnLine();
    void captureLabelChangesWithoutWaitingForANewExposure()
    {
        DetectionPanel panel;
        DetectionView view; view.summary.ready=true; view.summary.passed=true; view.summary.firing=true;
        panel.setView(view);
        auto* state=panel.findChild<QLabel*>("detectionState");
        QVERIFY(state->text().contains("Would trigger — capture OFF"));
        panel.setMonitoring(true); QVERIFY(state->text().contains("Candidate — capture ON"));
        panel.setMonitoring(false); QVERIFY(state->text().contains("Would trigger — capture OFF"));
    }
    void previewShowsWouldTriggerWithoutSaving();
    void dragLocksOnAFeatureAndClickStillSelects();
    void livePanelInjectionEvidenceAndSavedSettings();
    void stretchARecordingWhileCapturingThenRestoreSettings();
    void saveARecordingManuallyThroughTheWindow();
    void cameraReadbacksRestartAndDisconnectKeepSeparateEvidence();
    void qhyRuntimeOverridePersistsAndRecovers();
    void qhyReadbacksBayerEvidenceAndUnknownDrops();
};

void UiTest::previewShowsWouldTriggerWithoutSaving()
{
    QTemporaryDir dir;
    {
        MainWindow window(nullptr,dir.path()); window.show();
        window.findChild<QDockWidget*>("detectionDock")->raise();
        auto* histogram = window.findChild<QDockWidget*>("histogramDock");
        auto* samples = histogram->findChild<QLabel*>("histogramSampleRange");
        QVERIFY(histogram->isHidden());
        window.findChild<QPushButton*>("startButton")->click();
        auto* preview=window.findChild<PreviewWidget*>();
        QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(),3000);
        QCOMPARE(samples->text(), QString("Waiting for frames"));
        histogram->toggleViewAction()->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(samples->text().contains("At max code (255)"), 3000);
        histogram->findChild<QCheckBox*>("histogramFitRange")->setChecked(true);
        histogram->findChild<QCheckBox*>("histogramLogCounts")->setChecked(false);
        QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(),3000);
        QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,preview->imageRect().center());
        window.findChild<QDoubleSpinBox*>("blipAmplitudeSpinBox")->setValue(100);
        window.findChild<QSpinBox*>("blipFramesSpinBox")->setValue(10);
        auto* inject=window.findChild<QPushButton*>("injectBlipButton");
        QTRY_VERIFY_WITH_TIMEOUT(inject->isEnabled(),3000); inject->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("detectionState")->text().contains("Would trigger"),3000);
        QVERIFY(window.findChild<QLabel*>("sigmaReading")->text().contains("% of threshold"));
        QVERIFY(window.findChild<QLabel*>("patchReading")->text().contains("allowed"));
        QVERIFY(!window.findChild<QPushButton*>("saveNowButton")->isEnabled());
        QDir().mkpath(SELENEWATCH_CAMERA_QA_DIR);
        QVERIFY(window.grab().save(QString(SELENEWATCH_CAMERA_QA_DIR)+"/preview-would-trigger.png"));
        window.findChild<QPushButton*>("stopButton")->click();
    }
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot).size(),0);
    MainWindow restored(nullptr, dir.path());
    auto* histogram = restored.findChild<QDockWidget*>("histogramDock");
    QVERIFY(!histogram->isHidden());
    QVERIFY(histogram->findChild<QCheckBox*>("histogramFitRange")->isChecked());
    QVERIFY(!histogram->findChild<QCheckBox*>("histogramLogCounts")->isChecked());
}

// A message has the status bar's first line to itself, above the readouts,
// so the readouts cannot squeeze it. A text too long for its label is
// shortened on screen only; the whole text stays the label's text and is its
// tool tip.
void UiTest::statusMessagesHaveTheirOwnLine()
{
    QTemporaryDir dir;
    MainWindow window(nullptr, dir.path());
    window.show();
    auto* source = window.findChild<QComboBox*>("sourceComboBox");
    source->setCurrentIndex(source->findData("recording"));
    window.findChild<QPushButton*>("startButton")->click();
    auto* message = window.findChild<QLabel*>("statusMessage");
    QVERIFY(message);
    QCOMPARE(message->text(), QString("Choose a recording file first"));
    const auto* frames = window.findChild<QLabel*>("framesReadout");
    QVERIFY(message->mapTo(&window, QPoint()).y() + message->height() <= frames->mapTo(&window, QPoint()).y());
    QVERIFY(message->width() > frames->mapTo(&window, QPoint()).x() + frames->width() - message->mapTo(&window, QPoint()).x());

    const QString longText = "Clip saved: " + QString(40, 'x') + "/2026-11-16T21-04-05Z-candidate";
    ElidedLabel label(longText, Qt::ElideMiddle);
    label.resize(label.minimumSizeHint().width() * 2, label.sizeHint().height());
    label.show();
    QVERIFY(label.minimumSizeHint().width() < label.fontMetrics().horizontalAdvance(longText));
    QCOMPARE(label.text(), longText);
    QHelpEvent help(QEvent::ToolTip, QPoint(5, 5), label.mapToGlobal(QPoint(5, 5)));
    QVERIFY(QApplication::sendEvent(&label, &help));
    QTRY_COMPARE(QToolTip::text(), longText);
}

// Feature-8 Part A: a drag locks, the readout follows, a click still selects a
// pixel, and Preview writes no guiding record.
void UiTest::dragLocksOnAFeatureAndClickStillSelects()
{
    QTemporaryDir dir;
    {
        MainWindow window(nullptr, dir.path()); window.show();
        window.findChild<QDockWidget*>("guidingDock")->raise();
        window.findChild<QPushButton*>("startButton")->click();
        auto* preview = window.findChild<PreviewWidget*>();
        QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(), 3000);
        auto* state = window.findChild<QLabel*>("lockStateLabel");
        auto* readout = window.findChild<QLabel*>("lockReadout");
        auto* message = window.findChild<QLabel*>("lockMessageLabel");
        QVERIFY(state->text().contains("Off"));

        const QPoint from = preview->imageRect().center() - QPoint(30, 25);
        const QPoint to = preview->imageRect().center() + QPoint(30, 25);
        QSignalSpy regions(preview, &PreviewWidget::regionSelected);
        QSignalSpy pixels(preview, &PreviewWidget::positionSelected);
        QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(preview, from + QPoint(12, 9));
        QTest::mouseMove(preview, to);
        QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, to);
        QCOMPARE(regions.size(), 1);
        QCOMPARE(pixels.size(), 0);
        QVERIFY(!preview->selectedPosition());
        QTRY_VERIFY2_WITH_TIMEOUT(state->text().contains("Locked"), qPrintable(message->text()), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(readout->text().startsWith("Lock: dx"), 5000);
        QVERIFY(window.findChild<QLabel*>("lockDriftLabel")->text().contains("median of"));

        window.findChild<QPushButton*>("unlockButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("Off"), 3000);
        QCOMPARE(readout->text(), QString("Lock: off"));

        auto* lockAtSelection = window.findChild<QPushButton*>("lockAtSelectionButton");
        QVERIFY(!lockAtSelection->isEnabled());
        QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier, preview->imageRect().center());
        QCOMPARE(pixels.size(), 1);
        QVERIFY(preview->selectedPosition());
        QVERIFY(lockAtSelection->isEnabled());
        lockAtSelection->click();
        QTRY_VERIFY2_WITH_TIMEOUT(state->text().contains("Locked"), qPrintable(message->text()), 5000);

        // Part B controls: the simulated mount connects, calibration starts, and a disconnect ends it.
        auto* mountState = window.findChild<QLabel*>("mountStateLabel");
        auto* calibrate = window.findChild<QPushButton*>("calibrateButton");
        auto* guide = window.findChild<QPushButton*>("guideButton");
        QVERIFY(!calibrate->isEnabled());
        QVERIFY(!guide->isEnabled());
        window.findChild<QPushButton*>("connectMountButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(mountState->text().contains("Simulated mount"), 3000);
        QVERIFY(mountState->text().contains("not slewing"));
        QTRY_VERIFY_WITH_TIMEOUT(calibrate->isEnabled(), 3000);
        QVERIFY(!guide->isEnabled()); // not calibrated yet
        calibrate->click();
        auto* calibration = window.findChild<QLabel*>("calibrationLabel");
        QTRY_VERIFY_WITH_TIMEOUT(calibration->text().contains("Calibrating"), 3000);
        QDir().mkpath(SELENEWATCH_CAMERA_QA_DIR);
        QVERIFY(window.grab().save(QString(SELENEWATCH_CAMERA_QA_DIR) + "/preview-feature-lock.png"));
        window.findChild<QPushButton*>("disconnectMountButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(mountState->text().contains("Not connected"), 3000);
        QVERIFY(calibration->text().contains("Not calibrated"));
        QVERIFY(window.findChild<QLabel*>("guideMessageLabel")->text().contains("disconnected"));
        window.findChild<QPushButton*>("stopButton")->click();
    }
    // Preview writes nothing, guiding included.
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);
}

void UiTest::qhyRuntimeOverridePersistsAndRecovers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString savedPath=dir.filePath("QHY SDK/qhyccd.dll");
    QSettings().setValue("qhy/runtimePath",savedPath);
    auto api=std::make_shared<FakeQhyApi>();
    {
        MainWindow window(nullptr,dir.path(),{},api);
        QVERIFY(api->runtimeOverride.isEmpty()); // No runtime discovery during application startup.
        auto* source=window.findChild<QComboBox*>("sourceComboBox");
        api->failOperation="devices";
        source->setCurrentIndex(source->findData("qhy"));
        QCOMPARE(api->runtimeOverride,savedPath);
        QVERIFY(window.findChild<QLabel*>("qhyReadback")->text().contains("TEST QHY devices failed"));
        auto* start=window.findChild<QPushButton*>("startButton");
        QVERIFY(!start->isEnabled());
        api->failOperation.clear();
        api->connected=false;
        auto* automatic=window.findChild<QPushButton*>("automaticQhyRuntime");
        QVERIFY(automatic->isEnabled());
        automatic->click();
        QVERIFY(api->runtimeOverride.isEmpty());
        QVERIFY(!QSettings().contains("qhy/runtimePath"));
        QVERIFY(!start->isEnabled());
        QVERIFY(window.findChild<QLabel*>("qhyReadback")->text().contains("check the QHY driver installation"));
        api->connected=true;
        window.findChild<QPushButton*>("refreshQhyCameras")->click();
        QVERIFY(start->isEnabled());
    }
    MainWindow reopened(nullptr,dir.path(),{},api);
    auto* source=reopened.findChild<QComboBox*>("sourceComboBox");
    source->setCurrentIndex(source->findData("qhy"));
    QVERIFY(!reopened.findChild<QPushButton*>("automaticQhyRuntime")->isEnabled());
    QVERIFY(reopened.findChild<QPushButton*>("startButton")->isEnabled());
    source->setCurrentIndex(source->findData("recording"));
    QVERIFY(reopened.findChild<QDockWidget*>("qhyCameraDock")->isHidden());
    QCOMPARE(reopened.findChild<QLabel*>("fileLabel")->text(),QString("No recording chosen"));
    source->setCurrentIndex(source->findData("synthetic"));
    QVERIFY(reopened.findChild<QLabel*>("fileLabel")->text().isEmpty());
}

void UiTest::qhyReadbacksBayerEvidenceAndUnknownDrops()
{
    QTemporaryDir dir;
    auto api=std::make_shared<FakeQhyApi>();
    for (auto& c:api->state.controls)
        if (c.kind==QhyControl::Gain) { c.minimum=.25;c.step=.5;c.value=12.25; }
    MainWindow window(nullptr,dir.path(),{},api);
    window.show();
    QCOMPARE(api->opened.load(),0);
    auto* source=window.findChild<QComboBox*>("sourceComboBox");
    const int qhyIndex=source->findData("qhy");
    QVERIFY(qhyIndex>=2);
    source->setCurrentIndex(qhyIndex);
    auto* panel=window.findChild<QhyCameraPanel*>();
    QVERIFY(panel);
    QVERIFY(panel->hasCamera());
    QVERIFY(!window.findChild<QSpinBox*>("targetFpsSpinBox")->isEnabled());
    QCOMPARE(panel->findChild<QComboBox*>("cameraBin")->count(),1);
    auto* start=window.findChild<QPushButton*>("startButton");
    auto* stop=window.findChild<QPushButton*>("stopButton");
    auto* save=window.findChild<QPushButton*>("saveNowButton");
    auto* monitoring=window.findChild<QPushButton*>("monitoringButton");
    auto* apply=panel->findChild<QPushButton*>("applyQhyCamera");
    auto* preview=window.findChild<PreviewWidget*>();
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(),3000);
    auto* gain=panel->findChild<QDoubleSpinBox*>("cameraControl1");
    QCOMPARE(gain->value(),12.25);
    QVERIFY(!save->isEnabled());
    const int previewOpens=api->opened.load();
    gain->setValue(13.37);
    panel->findChild<QDoubleSpinBox*>("cameraControl0")->setValue(2);
    panel->findChild<QDoubleSpinBox*>("cameraControl2")->setValue(17);
    panel->findChild<QDoubleSpinBox*>("cameraControl3")->setValue(20);
    panel->findChild<QDoubleSpinBox*>("cameraControl4")->setValue(1);
    QTRY_COMPARE_WITH_TIMEOUT(panel->profile().current.controls.at(QhyControl::Gain),13.25,3000);
    QCOMPARE(panel->profile().current.controls.at(QhyControl::Exposure),2000.0);
    QCOMPARE(panel->profile().current.controls.at(QhyControl::Offset),17.0);
    QCOMPARE(panel->profile().current.controls.at(QhyControl::UsbTraffic),20.0);
    QCOMPARE(panel->profile().current.controls.at(QhyControl::TransferSpeed),1.0);
    QCOMPARE(api->opened.load(),previewOpens);
    QCOMPARE(api->starts.load(),1);
    QCOMPARE(api->stops.load(),0);
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("sigmaReading")->text().contains("Settling"),3000);
    QVERIFY(!QFile::exists(dir.path()+"/candidates.txt"));

    monitoring->click();
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),2,3000);
    QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(),3000);
    QVERIFY(!apply->isEnabled());
    QVERIFY(!panel->findChild<QDoubleSpinBox*>("cameraControl1")->isEnabled());
    QVERIFY(!panel->findChild<QPushButton*>("chooseQhyRuntime")->isEnabled());
    QVERIFY(save->isEnabled());
    save->click();
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    api->notReady=true;
    monitoring->click();
    QTRY_COMPARE_WITH_TIMEOUT(monitoring->text(),QString("Start monitoring"),3000);
    QVERIFY(!save->isEnabled());
    QCOMPARE(api->starts.load(),2);
    QCOMPARE(api->stops.load(),1);
    api->notReady=false;

    gain=panel->findChild<QDoubleSpinBox*>("cameraControl1");
    QVERIFY(gain->isEnabled());
    gain->setValue(20.25);
    auto* format=panel->findChild<QComboBox*>("cameraFormat");
    format->setCurrentIndex(format->findData(static_cast<int>(PixelFormat::Mono16)));
    panel->findChild<QSpinBox*>("cameraWidth")->setValue(64);
    panel->findChild<QSpinBox*>("cameraHeight")->setValue(32);
    panel->findChild<QSpinBox*>("cameraX")->setValue(1);
    panel->findChild<QSpinBox*>("cameraY")->setValue(1);
    apply->click();
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),3,3000);
    QCOMPARE(panel->profile().current.format,PixelFormat::Mono16);
    QCOMPARE(panel->profile().current.controls.at(QhyControl::Gain),20.25);
    QVERIFY(!save->isEnabled());
    monitoring->click();
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),4,3000);
    QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(),3000);
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("framesReadout")->text().contains("SDK drops: unknown"),3000);
    save->click();
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    QVERIFY(window.findChild<QDockWidget*>("detectionDock")->isVisible());
    QVERIFY(window.findChild<QDockWidget*>("qhyCameraDock")->isVisible());
    window.findChild<DisplayPanel*>()->findChild<QComboBox*>("displayMode")->setCurrentIndex(2);
    QDir().mkpath(SELENEWATCH_CAMERA_QA_DIR);
    QVERIFY(window.grab().save(QString(SELENEWATCH_CAMERA_QA_DIR)+"/qhy-window.png"));
    api->connected=false;
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(),3000);
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        int complete=0;
        for (const auto& folder:QDir(dir.path()).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
            complete+=text(dir.path()+"/"+folder+"/event.txt").contains("complete: yes");
        return complete>=2;
    })(),5000);
    int before=0,after=0;
    for (const auto& folder:QDir(dir.path()).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
    {
        const auto note=text(dir.path()+"/"+folder+"/event.txt");
        if (note.contains("pixel_format: Mono8"))
        {
            QVERIFY(note.contains("gain: 13.25\n"));
            QVERIFY(note.contains("collection_end: monitoring stopped"));
            ++before;
        }
        if (note.contains("pixel_format: Mono16"))
        {
            QVERIFY(note.contains("gain: 20.25\n"));
            QVERIFY(note.contains("source failure: TEST QHY disconnected"));
            QVERIFY(note.contains("bayer_pattern: BGGR\n"));
            ++after;
        }
        QVERIFY(note.contains("sdk_dropped_frames=unknown"));
        QVERIFY(note.contains("kind=manual"));
        QVERIFY(note.contains("camera_serial: unknown"));
    }
    QCOMPARE(before,1);
    QCOMPARE(after,1);
    QCOMPARE(api->opened.load(),api->closed.load());
}

void UiTest::cameraReadbacksRestartAndDisconnectKeepSeparateEvidence()
{
    QTemporaryDir dir;
    auto api=std::make_shared<FakeZwoApi>(); api->dropCounts={0,0,3,3};
    MainWindow window(nullptr,dir.path(),api); window.show();
    auto* source=window.findChild<QComboBox*>("sourceComboBox");
    auto* panel=window.findChild<CameraPanel*>(); QVERIFY(panel);
    QCOMPARE(api->opened.load(),0); // Other source types never inspect hardware.
    source->setCurrentIndex(2); QVERIFY(panel->hasCamera());
    auto* fps=window.findChild<QSpinBox*>("targetFpsSpinBox"); QVERIFY(!fps->isEnabled());
    QVERIFY(panel->findChild<QDoubleSpinBox*>("cameraControl0"));
    QCOMPARE(panel->findChild<QDoubleSpinBox*>("cameraControl0")->decimals(),3);
    QCOMPARE(panel->findChild<QDoubleSpinBox*>("cameraControl0")->singleStep(),0.1);
    QVERIFY(!panel->findChild<QDoubleSpinBox*>("cameraControl4")); // Unsupported high-speed control is absent.
    auto* start=window.findChild<QPushButton*>("startButton");
    auto* stop=window.findChild<QPushButton*>("stopButton");
    auto* save=window.findChild<QPushButton*>("saveNowButton");
    start->click(); QTRY_VERIFY_WITH_TIMEOUT(api->delivered>=2,3000);
    auto* monitoring=window.findChild<QPushButton*>("monitoringButton");
    QVERIFY(!save->isEnabled());
    QVERIFY(window.findChild<QLabel*>("captureModeLabel")->text().contains("PREVIEW"));
    const int previewOpens=api->opened.load();
    panel->findChild<QDoubleSpinBox*>("cameraControl1")->setValue(13);
    panel->findChild<QDoubleSpinBox*>("cameraControl0")->setValue(2);
    panel->findChild<QDoubleSpinBox*>("cameraControl2")->setValue(11);
    panel->findChild<QDoubleSpinBox*>("cameraControl3")->setValue(60);
    QTRY_COMPARE_WITH_TIMEOUT(panel->profile().current.controls.at(ZwoControl::Gain),int64_t{12},3000);
    QCOMPARE(panel->profile().current.controls.at(ZwoControl::Exposure),int64_t{2000});
    QCOMPARE(panel->profile().current.controls.at(ZwoControl::Offset),int64_t{11});
    QCOMPARE(panel->profile().current.controls.at(ZwoControl::Bandwidth),int64_t{60});
    QCOMPARE(api->opened.load(),previewOpens); QCOMPARE(api->starts.load(),1); QCOMPARE(api->stops.load(),0);
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("sigmaReading")->text().contains("Settling"),3000);
    QVERIFY(!QFile::exists(dir.path()+"/candidates.txt"));
    QVERIFY(window.findChild<QDockWidget*>("detectionDock")->isVisible());
    QVERIFY(window.findChild<QDockWidget*>("cameraDock")->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("framesReadout")->text().contains("SDK drops: 3"),3000);
    panel->findChild<QSpinBox*>("cameraWidth")->setValue(127);
    panel->findChild<QPushButton*>("applyCamera")->click();
    QVERIFY(stop->isEnabled()); QCOMPARE(api->starts.load(),1); QCOMPARE(api->stops.load(),0);
    panel->findChild<QDoubleSpinBox*>("cameraControl1")->setValue(13);
    auto* format=panel->findChild<QComboBox*>("cameraFormat");
    format->setCurrentIndex(format->findData(static_cast<int>(PixelFormat::Mono16)));
    panel->findChild<QSpinBox*>("cameraWidth")->setValue(64);
    panel->findChild<QPushButton*>("applyCamera")->click();
    QTRY_VERIFY_WITH_TIMEOUT(api->starts>=2,3000);
    QCOMPARE(panel->profile().current.format,PixelFormat::Mono16);
    QCOMPARE(panel->profile().current.width,64);
    QCOMPARE(panel->findChild<QDoubleSpinBox*>("cameraControl1")->value(),12.0);
    auto* histogram = window.findChild<QDockWidget*>("histogramDock");
    histogram->toggleViewAction()->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(histogram->findChild<QLabel*>("histogramSampleRange")->text().contains("At max code (65535)"), 3000);
    QVERIFY(!save->isEnabled());
    monitoring->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),3,3000);
    QVERIFY(!panel->findChild<QDoubleSpinBox*>("cameraControl1")->isEnabled());
    QVERIFY(window.findChild<QDoubleSpinBox*>("detectionSigma")->isEnabled());
    save->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    monitoring->click(); QTRY_COMPARE_WITH_TIMEOUT(monitoring->text(),QString("Start monitoring"),3000);
    QVERIFY(!save->isEnabled()); QCOMPARE(api->starts.load(),3); QCOMPARE(api->stops.load(),2);
    QVERIFY(panel->findChild<QDoubleSpinBox*>("cameraControl1")->isEnabled());
    panel->findChild<QDoubleSpinBox*>("cameraControl1")->setValue(20);
    QTRY_COMPARE_WITH_TIMEOUT(panel->profile().current.controls.at(ZwoControl::Gain),int64_t{20},3000);
    format->setCurrentIndex(format->findData(static_cast<int>(PixelFormat::Mono8)));
    panel->findChild<QSpinBox*>("cameraWidth")->setValue(128);
    panel->findChild<QPushButton*>("applyCamera")->click();
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),4,3000);
    monitoring->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),5,3000);
    save->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    window.findChild<QDockWidget*>("cameraDock")->raise();
    window.findChild<DisplayPanel*>()->findChild<QComboBox*>("displayMode")->setCurrentIndex(2);
    QDir().mkpath(SELENEWATCH_CAMERA_QA_DIR);
    QVERIFY(window.grab().save(QString(SELENEWATCH_CAMERA_QA_DIR)+"/window-simulated.png"));
    api->connected=false;
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(),3000);
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        int complete=0;
        for (const auto& folder:QDir(dir.path()).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
            complete+=text(dir.path()+"/"+folder+"/event.txt").contains("complete: yes");
        return complete>=2;
    })(),5000);
    int old=0,changed=0;
    for (const auto& folder:QDir(dir.path()).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
    {
        const auto note=text(dir.path()+"/"+folder+"/event.txt");
        if (note.contains("pixel_format: Mono16")) { QVERIFY(note.contains("collection_end: monitoring stopped")); QVERIFY(note.contains("gain: 12\n")); ++old; }
        if (note.contains("pixel_format: Mono8")) { QVERIFY(note.contains("source failure: TEST camera disconnected")); QVERIFY(note.contains("gain: 20\n")); ++changed; }
        QVERIFY(note.contains("kind=manual")); QVERIFY(note.contains("contains_test_signal: yes"));
    }
    QCOMPARE(old,1); QCOMPARE(changed,1);
}

void UiTest::saveARecordingManuallyThroughTheWindow()
{
    QTemporaryDir directory;
    const QString input=directory.path()+"/native.ser";
    QFile file(input); QVERIFY(file.open(QIODevice::ReadWrite));
    Frame f; f.width=64; f.height=48; f.format=PixelFormat::Mono16;
    f.significantBits=12; f.alignment=BitAlignment::High; f.testSource=true;
    for (int i=0;i<64*48;++i)
    {
        const int word=(1000+i%8)<<4;
        f.data.push_back(static_cast<uint8_t>(word&255)); f.data.push_back(static_cast<uint8_t>(word>>8));
    }
    const auto native=f.data;
    f.timestamp=std::chrono::system_clock::time_point{}+std::chrono::seconds{1700000000};
    SerWriter writer(file); QString error;
    QVERIFY(writer.start(f,"TEST: manual window",error));
    for (int i=0;i<48;++i)
    {
        f.frameIndex=i;
        f.timestamp=std::chrono::system_clock::time_point{}+std::chrono::seconds{1700000000}+std::chrono::milliseconds{i*50};
        QVERIFY(writer.append(f,error));
    }
    QVERIFY(writer.finish(error)); file.seek(0); const auto inputBytes=file.readAll(); file.close();
    const QString output=directory.path()+"/events";
    MainWindow window(nullptr,output); window.selectRecording(input); window.show();
    auto* save=window.findChild<QPushButton*>("saveNowButton");
    QVERIFY(save); QVERIFY(!save->isEnabled());
    window.findChild<DisplayPanel*>()->findChild<QDoubleSpinBox*>("postSeconds")->setValue(.3);
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<PreviewWidget*>()->imageRect().isEmpty(),3000);
    window.findChild<QPushButton*>("monitoringButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    QVERIFY(save->isEnabled()); save->click();
    QString folder;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        for (const auto& name:QDir(output).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
        {
            const QString candidate=output+"/"+name;
            if (text(candidate+"/event.txt").contains("complete: yes")) { folder=candidate; return true; }
        }
        return false;
    })(),4000);
    const auto metadata=text(folder+"/event.txt");
    QVERIFY(metadata.contains("kind=manual")); QVERIFY(metadata.contains("collection_end: post-event interval complete"));
    QVERIFY(!metadata.contains("kind=automatic"));
    QDir().mkpath(SELENEWATCH_MANUAL_QA_DIR);
    QVERIFY(window.grab().save(QString(SELENEWATCH_MANUAL_QA_DIR)+"/window.png"));
    window.findChild<QPushButton*>("stopButton")->click(); QVERIFY(!save->isEnabled());
    SerReader reader; std::string reason;
    QVERIFY(reader.open(std::filesystem::path((folder+"/event.ser").toStdWString()),20,reason));
    QVERIFY(reader.frameCount()>5);
    for (int i=0;i<reader.frameCount();++i)
    {
        Frame actual; actual.data.resize(reader.frameBytes()); QVERIFY(reader.readFrame(i,actual,reason));
        QCOMPARE(actual.data,native);
    }
    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),inputBytes);
}

void UiTest::controlsPreserveValidSizeLimits()
{
    DetectionPanel panel;
    QSignalSpy changed(&panel, &DetectionPanel::settingsChanged);
    panel.findChild<QSpinBox*>("detectionMinPixels")->setValue(500);
    QCOMPARE(panel.settings().minPixels, 500);
    QCOMPARE(panel.settings().maxPixels, 500);
    panel.findChild<QSpinBox*>("detectionMaxPixels")->setValue(9);
    QCOMPARE(panel.settings().minPixels, 9);
    QCOMPARE(panel.settings().maxPixels, 9);
    QVERIFY(changed.count() >= 2);
    panel.findChild<QSlider*>("sigmaSlider")->setValue(600);
    QVERIFY(panel.settings().sigma > 6);
    panel.findChild<QSlider*>("compactnessSlider")->setValue(600);
    QVERIFY(panel.settings().maxPatchBoxRatio > 4);
    panel.findChild<QToolButton*>("backgroundSettings")->click();
    panel.findChild<QSlider*>("referenceTimeSlider")->setValue(600);
    panel.findChild<QSlider*>("minimumNoiseSlider")->setValue(600);
    QVERIFY(panel.settings().referenceTimeConstantSeconds > 2);
    QVERIFY(panel.settings().minNoiseAdu > 0.5);
    QVERIFY(panel.settings().valid());
}

void UiTest::detectionPanelExplainsPatchRejections()
{
    DetectionPanel panel;
    DetectionView view;
    view.summary.ready = true;
    view.summary.peakSigma = 5.0;
    const auto* label = panel.findChild<QLabel*>("detectionState");
    panel.setView(view);
    QVERIFY(label->text().contains("Below K"));
    view.summary.peakSigma = 10.0;
    view.summary.brightPixels = 5;
    view.summary.boxWidth = 3;
    view.summary.boxHeight = 2;
    panel.setView(view);
    QVERIFY(label->text().contains("5 px, needs 8"));
    view.summary.brightPixels = 9;
    view.summary.boxWidth = view.summary.boxHeight = 9;
    panel.setView(view);
    QVERIFY(label->text().contains("Box / patch 9.00 exceeds 4.00"));
    view.summary.brightPixels = 5000;
    view.summary.patchSearchLimited = true;
    panel.setView(view);
    QVERIFY(label->text().contains("Too many pixels at K"));
    QVERIFY(panel.findChild<QLabel*>("patchReading")->text().contains("across the frame"));
}

// Feature-11: Moon and Sky keep their own detection and capture settings.
// The Moon's are saved under the keys used before Sky mode existed, and
// guiding is off while the sky is watched.
void UiTest::eachTargetKeepsItsOwnSettings()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    auto* panel = window.findChild<DetectionPanel*>();
    auto* display = window.findChild<DisplayPanel*>();
    auto* guiding = window.findChild<GuidingPanel*>();
    auto* choice = panel->findChild<QComboBox*>("targetChoice");
    auto* sigma = panel->findChild<QDoubleSpinBox*>("detectionSigma");
    QCOMPARE(panel->target(), Target::Moon);
    QVERIFY(choice->isEnabled());
    QVERIFY(guiding->isEnabled());
    QCOMPARE(panel->settings().brighteningShare, 0.0);
    QCOMPARE(display->captureSettings().diskReserveBytes, uint64_t{10} * 1024 * 1024 * 1024);
    sigma->setValue(7);

    choice->setCurrentIndex(1);
    QCOMPARE(panel->target(), Target::Sky);
    QVERIFY(panel->findChild<QWidget*>("skySettings")->isVisibleTo(panel));
    QCOMPARE(panel->settings().sigma, 6.0);
    QCOMPARE(panel->settings().maxPixels, 3000);
    QCOMPARE(panel->settings().maxPatchBoxRatio, 30.0);
    QCOMPARE(panel->settings().brighteningShare, 0.02);
    QCOMPARE(panel->settings().releaseSeconds, 20.0);
    QCOMPARE(display->captureSettings().target, Target::Sky);
    QCOMPARE(display->captureSettings().postSeconds, 5.0);
    QCOMPARE(display->captureSettings().maxPostSeconds, 20.0);
    QVERIFY(!guiding->isEnabled());
    // The Moon's settings were saved under their old keys on the way out.
    QCOMPARE(QSettings().value("detection/sigma").toDouble(), 7.0);
    QCOMPARE(QSettings().value("target").toInt(), 1);

    // A mask drawn on the sky, and a Sky K.
    panel->findChild<QToolButton*>("drawMask")->setChecked(true);
    QVERIFY(panel->addMaskRect(QRect(0, 400, 640, 80)));
    QCOMPARE(panel->settings().mask.count, 1);
    sigma->setValue(9);

    choice->setCurrentIndex(0);
    QCOMPARE(panel->settings().sigma, 7.0);
    QCOMPARE(panel->settings().maxPixels, 400);
    QCOMPARE(panel->settings().brighteningShare, 0.0);
    QVERIFY(panel->settings().mask.empty());
    QVERIFY(!panel->drawingMask());
    QCOMPARE(display->captureSettings().target, Target::Moon);
    QCOMPARE(display->captureSettings().maxPostSeconds, 10.0);
    QVERIFY(guiding->isEnabled());
    QCOMPARE(QSettings().value("sky/detection/sigma").toDouble(), 9.0);
    QCOMPARE(QSettings().value("sky/detection/mask").toString(), QString("0,400,640,80"));

    choice->setCurrentIndex(1);
    QCOMPARE(panel->settings().sigma, 9.0);
    QCOMPARE(panel->settings().mask.count, 1);
    QCOMPARE(panel->settings().mask.rects[0], (MaskRect{0, 400, 640, 80}));
}

// The readouts keep room for their longest texts, so the controls below them
// stay where they are while the texts change, at the narrowest panel and a
// wide one.
void UiTest::readoutsKeepTheControlsBelowInPlace()
{
    const auto positions = [](QWidget* panel, const QList<QWidget*>& widgets) {
        for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
        QList<int> y;
        for (const QWidget* widget : widgets) y << widget->mapTo(panel, QPoint()).y();
        return y;
    };
    for (const int width : {350, 700})
    {
        DetectionPanel detection;
        detection.resize(width, 1600);
        detection.show();
        const QList<QWidget*> belowReadouts{detection.findChild<QSpinBox*>("detectionMinPixels"),
            detection.findChild<QCheckBox*>("oneFrameTrigger"), detection.findChild<QComboBox*>("detectionOverlay"),
            detection.findChild<QToolButton*>("backgroundSettings")};
        const QList<int> start = positions(&detection, belowReadouts);
        DetectionView view;
        view.settling = true;
        detection.setView(view);
        QCOMPARE(positions(&detection, belowReadouts), start);
        view.settling = false;
        view.summary.learningFrames = 12;
        detection.setView(view);
        QCOMPARE(positions(&detection, belowReadouts), start);
        view.summary.ready = true;
        view.summary.peakSigma = 5.2;
        detection.setView(view);
        QCOMPARE(positions(&detection, belowReadouts), start);
        view.summary.peakSigma = 41.7;
        view.summary.brightPixels = 9;
        view.summary.boxWidth = view.summary.boxHeight = 9;
        view.summary.overlaySkipped = true;
        detection.setView(view);
        QVERIFY(detection.findChild<QLabel*>("patchReading")->text().contains("Box / patch"));
        QCOMPARE(positions(&detection, belowReadouts), start);
        view.summary.brightPixels = 5000;
        view.summary.patchSearchLimited = true;
        detection.setView(view);
        QCOMPARE(positions(&detection, belowReadouts), start);

        GuidingPanel guiding;
        guiding.resize(width, 1600);
        guiding.show();
        const QList<QWidget*> belowLock{guiding.findChild<QPushButton*>("lockAtSelectionButton"),
            guiding.findChild<QPushButton*>("connectMountButton"), guiding.findChild<QPushButton*>("calibrateButton"),
            guiding.findChild<QDoubleSpinBox*>("toleranceSpinBox"), guiding.findChild<QPushButton*>("guideButton")};
        const QList<int> idle = positions(&guiding, belowLock);
        GuiderStatus status;
        status.lock.state = LockState::Locked;
        status.lock.region = {600, 420, 64, 64};
        status.lock.quality.contrast = 48;
        status.lock.quality.structureRatio = 0.62;
        status.lock.lastMatch = LockMatch{1.25, -0.5, 0.973, true, {}};
        status.lock.estimateValid = true;
        status.lock.estimateDx = 1.2;
        status.lock.estimateDy = -0.4;
        status.lock.estimateSamples = 6;
        guiding.setStatus(status);
        QCOMPARE(positions(&guiding, belowLock), idle);
        status.lock.state = LockState::Searching;
        status.lock.lastMatch = LockMatch{14.5, -22.0, 0.31, false, "match score 0.31 below 0.50"};
        status.lock.message = "No valid match for 3 s (match score 0.31 below 0.50). Searching within 60 pixels.";
        guiding.setStatus(status);
        QCOMPARE(positions(&guiding, belowLock), idle);
        status.lock.state = LockState::Locked;
        status.lock.message.clear();
        status.guide.mountConnected = true;
        status.guide.mountName = "Simulated mount";
        status.guide.mount.connected = status.guide.mount.tracking = status.guide.mount.canPulseGuide = true;
        status.guide.mount.guideRateRa = status.guide.mount.guideRateDec = 0.0021;
        status.guide.mode = GuideMode::Calibrating;
        status.guide.calibrationProgress = "taking up declination backlash (2.5 s)";
        guiding.setStatus(status);
        QCOMPARE(positions(&guiding, belowLock), idle);
        GuideCalibration calibration;
        calibration.west = {0.004, 0.001};
        calibration.north = {-0.001, 0.004};
        calibration.arcsecPerPixel = 1.85;
        status.guide.calibration = calibration;
        status.guide.guideSpeedPixelsPerFrame = 0.12;
        status.guide.mode = GuideMode::Guiding;
        status.guide.pulses = 12;
        status.guide.lastPulse = GuidePulse{GuideDirection::West, 340};
        guiding.setStatus(status);
        QVERIFY(guiding.findChild<QLabel*>("guidingLabel")->text().contains("12 pulses"));
        QCOMPARE(positions(&guiding, belowLock), idle);
    }
}

void UiTest::livePanelInjectionEvidenceAndSavedSettings()
{
    QTemporaryDir output;
    QVERIFY(output.isValid());
    DetectionSettings chosen{7.5, 9, 500, true, 5.25, 3.5, 0.75};
    {
        MainWindow window(nullptr, output.path());
        window.show();
        auto* panel = window.findChild<DetectionPanel*>();
        auto* dock = window.findChild<QDockWidget*>("detectionDock");
        dock->raise();
        auto* preview = window.findChild<PreviewWidget*>("previewWidget");
        auto* start = window.findChild<QPushButton*>("startButton");
        auto* stop = window.findChild<QPushButton*>("stopButton");
        auto* inject = window.findChild<QPushButton*>("injectBlipButton");
        QVERIFY(panel && dock && preview && start && stop && inject);
        QCOMPARE(panel->settings().sigma, 6.0);
        panel->findChild<QDoubleSpinBox*>("detectionSigma")->setValue(chosen.sigma);
        panel->findChild<QSpinBox*>("detectionMinPixels")->setValue(chosen.minPixels);
        panel->findChild<QSpinBox*>("detectionMaxPixels")->setValue(chosen.maxPixels);
        panel->findChild<QDoubleSpinBox*>("detectionMaxBoxRatio")->setValue(chosen.maxPatchBoxRatio);
        panel->findChild<QDoubleSpinBox*>("detectionReferenceTime")->setValue(chosen.referenceTimeConstantSeconds);
        panel->findChild<QDoubleSpinBox*>("detectionMinNoise")->setValue(chosen.minNoiseAdu);
        start->click();
        window.findChild<QPushButton*>("monitoringButton")->click();
        QVERIFY(stop->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(), 5000);
        panel->findChild<QComboBox*>("detectionOverlay")->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!panel->overlayImage().isNull(), 5000);
        QCOMPARE(panel->settings(), chosen);
        QDir().mkpath(SELENEWATCH_UI_QA_DIR);
        QVERIFY(window.grab().save(QString(SELENEWATCH_UI_QA_DIR) + "/window-sigma.png"));
        panel->findChild<QComboBox*>("detectionOverlay")->setCurrentIndex(2);
        QTest::qWait(150);
        QVERIFY(window.grab().save(QString(SELENEWATCH_UI_QA_DIR) + "/window-noise.png"));
        QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier, preview->imageRect().center());
        QTRY_VERIFY_WITH_TIMEOUT(inject->isEnabled(), 3000);
        auto* readout = window.findChild<QLabel*>("blipStatusLabel");
        QTRY_VERIFY_WITH_TIMEOUT(readout->text().contains("sigma at centre"), 3000);
        // Closing the panel must leave the same trigger running.
        dock->hide();
        window.findChild<QDoubleSpinBox*>("blipFwhmSpinBox")->setValue(4);
        window.findChild<QDoubleSpinBox*>("blipAmplitudeSpinBox")->setValue(80);
        window.findChild<QSpinBox*>("blipFramesSpinBox")->setValue(1);
        inject->click();
        QString clipFolder;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto dirs = QDir(output.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const auto& dir : dirs)
            {
                const QString path = QDir(output.path()).filePath(dir);
                if (text(QDir(path).filePath("event.txt")).contains("complete: yes")) { clipFolder = path; return true; }
            }
            return false;
        })(), 10000);
        const QString metadata = text(QDir(clipFolder).filePath("event.txt"));
        QVERIFY(metadata.contains("sigma=7.5 min_pixels=9 max_pixels=500 one_frame=yes"));
        QVERIFY(metadata.contains("max_patch_box_ratio=5.25 reference_time_constant_seconds=3.5 min_noise_adu=0.75"));
        QCOMPARE(metadata.count(" test_blip="), 1);
        SerReader reader;
        std::string error;
        QVERIFY2(reader.open(std::filesystem::path(QDir(clipFolder).filePath("event.ser").toStdWString()), 30, error), error.c_str());
        int checked = 0;
        for (const QString& line : metadata.split('\n'))
        {
            bool isFrame = false;
            const int position = line.section(' ', 0, 0).toInt(&isFrame);
            if (!isFrame || line.contains(" test_blip=")) continue;
            const uint64_t index = line.section(' ', 1, 1).toULongLong();
            Frame stored;
            stored.data.resize(reader.frameBytes());
            QVERIFY(reader.readFrame(position, stored, error));
            Frame expected;
            paintSyntheticFrame(expected, stored.width, stored.height, index, false);
            QCOMPARE(stored.data, expected.data);
            ++checked;
        }
        QVERIFY(checked > 1);
        dock->show();
        window.resize(1550, 900);
        QTest::qWait(100);
        QVERIFY(window.grab().save(QString(SELENEWATCH_UI_QA_DIR) + "/window-resized.png"));
        stop->click();
        QVERIFY(!inject->isEnabled());
    }
    MainWindow restored(nullptr, output.path());
    QCOMPARE(restored.findChild<DetectionPanel*>()->settings(), chosen);
}

void UiTest::stretchARecordingWhileCapturingThenRestoreSettings()
{
    QTemporaryDir output;
    const QString input = QDir(output.path()).filePath("faint-test.ser");
    QFile file(input);
    QVERIFY(file.open(QIODevice::WriteOnly));
    SerWriter writer(file);
    QString reason;
    for (int i = 0; i < 100; ++i)
    {
        Frame frame; frame.width = 64; frame.height = 48; frame.format = PixelFormat::Mono16;
        frame.significantBits = 12; frame.alignment = BitAlignment::High; frame.testSource = true;
        frame.frameIndex = static_cast<uint64_t>(i);
        frame.timestamp = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{i * 100};
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x)
        {
            const int value = (x == 0 && y == 0 ? 4000 : 8 + (x + y) % 48) << 4;
            frame.data.push_back(static_cast<uint8_t>(value & 255)); frame.data.push_back(static_cast<uint8_t>(value >> 8));
        }
        if (i == 0) QVERIFY(writer.start(frame, "TEST faint recording", reason));
        QVERIFY(writer.append(frame, reason));
    }
    QVERIFY(writer.finish(reason)); file.close();
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray inputBefore = file.readAll(); file.close();
    DisplaySettings savedDisplay;
    const CaptureSettings chosen{0.3, 0.25, 0.6, size_t{128} * 1024 * 1024};
    {
        MainWindow window(nullptr, QDir(output.path()).filePath("events"));
        window.selectRecording(input); window.show();
        auto* panel = window.findChild<DisplayPanel*>();
        window.findChild<QDockWidget*>("displayDock")->raise();
        panel->setCaptureSettings(chosen);
        auto* start = window.findChild<QPushButton*>("startButton");
        auto* stop = window.findChild<QPushButton*>("stopButton");
        auto* preview = window.findChild<PreviewWidget*>();
        start->click();
        window.findChild<QPushButton*>("monitoringButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(panel->findChild<QDoubleSpinBox*>("displayWhite")->maximum(), 4095.0, 3000);
        QVERIFY(!panel->findChild<QDoubleSpinBox*>("historySeconds")->isEnabled());
        const auto brightness = [&] {
            const QRect rect = preview->imageRect();
            const QPoint point(rect.x() + rect.width() * 47 / 64, rect.y() + rect.height() * 24 / 48);
            return qGray(preview->grab().toImage().pixel(point));
        };
        const int linear = brightness();
        auto* mode = panel->findChild<QComboBox*>("displayMode");
        mode->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(brightness() > linear + 30, 3000);
        QDir().mkpath(SELENEWATCH_DISPLAY_QA_DIR);
        QVERIFY(window.grab().save(QString(SELENEWATCH_DISPLAY_QA_DIR) + "/window-auto.png"));
        mode->setCurrentIndex(1);
        panel->findChild<QDoubleSpinBox*>("displayBlack")->setValue(8);
        panel->findChild<QDoubleSpinBox*>("displayWhite")->setValue(64);
        const int normal = brightness();
        panel->findChild<QCheckBox*>("displayInvert")->setChecked(true);
        QCOMPARE(brightness() + normal, 255);
        panel->findChild<QCheckBox*>("displayInvert")->setChecked(false);
        QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier, preview->imageRect().center());
        window.findChild<QDoubleSpinBox*>("blipFwhmSpinBox")->setValue(4);
        window.findChild<QDoubleSpinBox*>("blipAmplitudeSpinBox")->setValue(50);
        window.findChild<QSpinBox*>("blipFramesSpinBox")->setValue(1);
        auto* inject = window.findChild<QPushButton*>("injectBlipButton");
        QTRY_VERIFY_WITH_TIMEOUT(inject->isEnabled(), 3000);
        inject->click();
        QString folder;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            QDir events(QDir(output.path()).filePath("events"));
            for (const auto& entry : events.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                if (text(events.filePath(entry + "/event.txt")).contains("complete: yes")) { folder = events.filePath(entry); return true; }
            return false;
        })(), 5000);
        const QString metadata = text(QDir(folder).filePath("event.txt"));
        QVERIFY(metadata.contains("capture_history_seconds: 0.3"));
        QVERIFY(metadata.contains("capture_post_seconds: 0.25"));
        QCOMPARE(metadata.count(" test_blip="), 1);
        SerReader source, saved;
        std::string error;
        QVERIFY(source.open(std::filesystem::path(input.toStdWString()), 10, error));
        QVERIFY(saved.open(std::filesystem::path(QDir(folder).filePath("event.ser").toStdWString()), 10, error));
        int checked = 0;
        for (const auto& line : metadata.split('\n'))
        {
            bool isFrame = false;
            const int position = line.section(' ', 0, 0).toInt(&isFrame);
            if (!isFrame || line.contains(" test_blip=")) continue;
            Frame original, stored;
            original.data.resize(source.frameBytes()); stored.data.resize(saved.frameBytes());
            QVERIFY(source.readFrame(line.section(' ', 1, 1).toInt(), original, error));
            QVERIFY(saved.readFrame(position, stored, error));
            QCOMPARE(stored.data, original.data);
            ++checked;
        }
        QVERIFY(checked > 1);
        stop->click();
        QVERIFY(panel->findChild<QDoubleSpinBox*>("historySeconds")->isEnabled());
        // A stopped recording can still be restretched from the retained raw frame.
        const int before = brightness();
        panel->findChild<QSlider*>("blackSlider")->setValue(12);
        QVERIFY(brightness() != before);
        savedDisplay = panel->displaySettings();
        QVERIFY(window.grab().save(QString(SELENEWATCH_DISPLAY_QA_DIR) + "/window-levels-stopped.png"));
    }
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), inputBefore);
    MainWindow restored(nullptr, QDir(output.path()).filePath("restored-events"));
    auto* panel = restored.findChild<DisplayPanel*>();
    QCOMPARE(panel->displaySettings().mode, savedDisplay.mode);
    QCOMPARE(panel->displaySettings().black, savedDisplay.black);
    QCOMPARE(panel->captureSettings().historySeconds, chosen.historySeconds);
    QCOMPARE(panel->captureSettings().memoryBytes, chosen.memoryBytes);
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("SeleneWatchTests");
    QCoreApplication::setApplicationName("Feature9-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 9));
    UiTest tests;
    const int result = QTest::qExec(&tests, argc, argv);
    QSettings saved; saved.clear(); saved.sync();
    return result;
}
#include "ui_tests.moc"
