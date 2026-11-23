#include "watch_folders.h"
#include "ui/detection_panel.h"
#include "ui/display_panel.h"
#include "ui/cameras/camera_panel.h"
#include "ui/auto_tune_panel.h"
#include "ui/cameras/camera_settings_editor.h"
#include "ui/widgets/elided_label.h"
#include "ui/guiding_panel.h"
#include "mounts/simulated_mount.h"
#include "ui/session_end_dialog.h"
#include "fake_zwo_api.h"
#include "fake_qhy_api.h"
#include "fake_playerone_api.h"
#include "fake_touptek_api.h"
#include "ui/cameras/qhy_camera_panel.h"
#include "ui/cameras/playerone_camera_panel.h"
#include "ui/cameras/touptek_camera_panel.h"
#include "ui/cameras/camera_support_dialog.h"
#include "ui/cameras/runtime_chooser.h"
#include "ui/mainwindow.h"
#include "ui/manual_dialog.h"
#include "ui/preview_widget.h"
#include "ui/site_dialog.h"
#include "sky/site_presets.h"
#include "ui/startup_tips.h"
#include "ui/tips_dialog.h"
#include "detection/target_defaults.h"
#include "ui/release_checker.h"
#include "recording/output_paths.h"
#include "recording/ser_reader.h"
#include "recording/ser_writer.h"
#include "sources/synthetic_scene.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QTimeZone>
#include <QtCore/QUuid>
#include <QtGui/QAction>
#include <QtGui/QDesktopServices>
#include <QtGui/QFontDatabase>
#include <QtGui/QHelpEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTimeEdit>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QWhatsThis>
#include <QtWidgets/QStyleFactory>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace {
QString text(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}
// Chooses a Source entry by its key ("synthetic", "recording", "zwo", ...).
void chooseSource(MainWindow& window, const QString& key)
{
    auto* source = window.findChild<QComboBox*>("sourceComboBox");
    const int index = source->findData(key);
    QVERIFY2(index > 0, qPrintable(key));
    source->setCurrentIndex(index);
}
// Feature-31: chooses what is watched, by its value in the Watching list.
void chooseWatch(QWidget& root, Watch watch)
{
    auto* choice = root.findChild<QComboBox*>("targetChoice");
    choice->setCurrentIndex(choice->findData(static_cast<int>(watch)));
}
// Answers the next modal message box with this object name: its accept-role
// button, or Cancel. Retries until the box is up (it opens inside exec()).
void answerMessageBox(const QString& objectName, bool accept, int attempts = 200)
{
    QTimer::singleShot(10, [objectName, accept, attempts] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box || box->objectName() != objectName)
        {
            if (attempts > 0) answerMessageBox(objectName, accept, attempts - 1);
            return;
        }
        const auto role = accept ? QMessageBox::AcceptRole : QMessageBox::RejectRole;
        for (QAbstractButton* button : box->buttons())
        {
            if (box->buttonRole(button) == role)
            {
                button->click();
                return;
            }
        }
    });
}
// Most window tests run on the synthetic source, which the testing tools offer.
void useSynthetic(MainWindow& window)
{
    window.setTestingTools(true);
    chooseSource(window, "synthetic");
}
}

#include "recording/output_paths.h"
#include "ui/resharpen_dialog.h"
#include "timelapse/timelapse_settings.h"
#include "ui/timelapse_panel.h"

class ReleaseUrlReceiver : public QObject
{
    Q_OBJECT
public:
    QUrl opened;
public slots:
    void open(const QUrl& url) { opened = url; }
};

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
    void watchesCanChangeInPreviewAndTransitControlsFollowTheMoon();
    void diskReserveStartsAtTenGiBAndIsRemembered();
    void theSlowSkyKeepsATimelapseWhileMonitoring();
    void everyControlFitsOnASmallLaptopScreen();
    void darkeningIsForTheMoonOnly();
    void shadowsUseTheLimitsAtTheTop();
    void everyControlHasHelp();
    void numberBoxesFitTheirWidestValue();
    void cameraSlidersSetTheBoxAndReachTheCameraOnce();
    void autoTuneTunesTheCameraLiveThenWatchesK();
    void autoTuneOutlinesTheEarthshineBox();
    void autoTuneSuggestsTheMinimumPatchForFlashes();
    void aDragDrawsOnlyWhatWasChosen();
    void sitePresetsSaveLoadAndDelete();
    void siteAndRecordingControls();
    void exposureFollower();
    void aSessionEndsByItselfAndStopsTheMountsTracking();
    void theDiskReserveEndsMonitoring();
    void lostGuidingEndsTheSession();
    void theEndsDialogShowsWhenEachConditionIsMet();
    void theReleaseIsShown();
    void aNewReleaseAppearsAtTheTopRight();
    void aMaskShapeIsDrawnOnThePreview();
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
    void playerOneAndToupTekInTheWindow();
    void cameraSupportShowsEachFamily();
    void testingToolsAndConnectAtStartup();
    void outputFolderSafetyAndMigration();
    void timelapsePanelAndResharpenWorkflow();
    void rawSerNeedsConfirmationAndIsNotRemembered();
    void closingDuringTimelapseWaitsForTheVideo();
    void userManualAndStartupTipsWorkflow();
    void recreateFolderWhilePreviewRunningAndWheelGuard();
    void detectionProfilesSaveLoadAndOptionalMask();
    void previewZoomControlsSyncWithWidget();
    void cameraRoiDrawingPresetsAndAutoRounding();
    void guiLayoutThemesFocusViewAndPersistence();
    void moonAndSkyEndToEndInjectionsRecordOnDisk();
};

// No source at start; View → Testing tools offers the synthetic source and the
// test flash, recorded on session_start; Connect at startup chooses a camera
// family when the window opens.
void UiTest::testingToolsAndConnectAtStartup()
{
    QTemporaryDir dir;
    auto zwo = std::make_shared<FakeZwoApi>();
    {
        MainWindow window(nullptr, dir.path(), zwo);
        window.show();
        auto* source = window.findChild<QComboBox*>("sourceComboBox");
        auto* start = window.findChild<QPushButton*>("startButton");
        auto* stop = window.findChild<QPushButton*>("stopButton");
        auto* connect = window.findChild<QCheckBox*>("connectAtStartup");
        auto* tools = window.findChild<QAction*>("testingToolsAction");
        // Nothing chosen, no testing tools, no camera opened.
        QVERIFY(source->currentData().toString().isEmpty());
        QCOMPARE(source->findData("synthetic"), -1);
        QVERIFY(source->findData("recording") > 0);
        QVERIFY(!start->isEnabled());
        QVERIFY(!tools->isChecked());
        QVERIFY(window.findChild<QGroupBox*>("blipGroupBox")->isHidden());
        QVERIFY(window.findChild<QPushButton*>("injectFlashButton")->isHidden());
        QVERIFY(window.findChild<QLabel*>("testingToolsBadge")->isHidden());
        QVERIFY(!connect->isEnabled());
        QCOMPARE(zwo->opened.load(), 0);

        // Testing tools on: the synthetic source and the test flash, remembered and recorded.
        tools->trigger();
        QVERIFY(QSettings().value("testing/tools").toBool());
        QVERIFY(!window.findChild<QGroupBox*>("blipGroupBox")->isHidden());
        QVERIFY(!window.findChild<QLabel*>("testingToolsBadge")->isHidden());
        chooseSource(window, "synthetic");
        QVERIFY(start->isEnabled());
        QVERIFY(!connect->isEnabled()); // only camera families connect at startup
        start->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("monitoringButton")->isEnabled(), 3000);
        QVERIFY(!tools->isEnabled());
        window.setTestingTools(false); // refused while running
        QVERIFY(tools->isChecked());
        window.findChild<QPushButton*>("monitoringButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(text(latestWatch(eventsFolder(dir.path(), Target::Moon)) + "/candidates.txt").contains(" testing_tools=\"on\" "), 3000);
        stop->click();
        QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 3000);
        // Off again: the synthetic source goes, and nothing is chosen.
        tools->trigger();
        QVERIFY(source->currentData().toString().isEmpty());
        QCOMPARE(source->findData("synthetic"), -1);
        QVERIFY(!start->isEnabled());

        // Connect at startup, for the chosen camera family.
        chooseSource(window, "zwo");
        QVERIFY(connect->isEnabled() && !connect->isChecked());
        connect->setChecked(true);
        QCOMPARE(QSettings().value("startup/camera").toString(), QString("zwo"));
    }
    {
        // The next start chooses the camera family and finds its camera, ready for Start.
        MainWindow window(nullptr, dir.path(), zwo);
        auto* source = window.findChild<QComboBox*>("sourceComboBox");
        QCOMPARE(source->currentData().toString(), QString("zwo"));
        QVERIFY(window.findChild<CameraPanel*>()->hasCamera());
        QVERIFY(window.findChild<QPushButton*>("startButton")->isEnabled());
        auto* connect = window.findChild<QCheckBox*>("connectAtStartup");
        QVERIFY(connect->isChecked());
        connect->setChecked(false);
        QVERIFY(!QSettings().contains("startup/camera"));
        chooseSource(window, "recording");
        QVERIFY(!connect->isEnabled());
    }
    MainWindow window(nullptr, dir.path(), zwo);
    QVERIFY(window.findChild<QComboBox*>("sourceComboBox")->currentData().toString().isEmpty());
}

// Session → Camera support: each family's state, SDK library, version and
// cameras, without opening a camera; a family missing from the build; the
// DLL choice; and nothing checked while a stream runs.
void UiTest::cameraSupportShowsEachFamily()
{
    auto zwo = std::make_shared<FakeZwoApi>();
    auto qhy = std::make_shared<FakeQhyApi>();
    qhy->failOperation = "devices";
    auto playerOne = std::make_shared<FakePlayerOneApi>();
    playerOne->connected = false;
    CameraPanel zwoPanel(zwo);
    QhyCameraPanel qhyPanel(qhy, nullptr);
    PlayerOneCameraPanel playerOnePanel(playerOne, nullptr);
    {
        CameraSupportDialog dialog({&zwoPanel, &qhyPanel, &playerOnePanel}, false);
        auto* table = dialog.findChild<QTableWidget*>("cameraSupportTable");
        auto* details = dialog.findChild<QLabel*>("cameraSupportDetails");
        QCOMPARE(table->rowCount(), 4);
        QCOMPARE(table->item(0, 1)->text(), QString("Not checked"));
        dialog.checkAll();
        // ZWO: ready, with its camera and SDK version; nothing opened.
        QCOMPARE(table->item(0, 1)->text(), QString("Ready"));
        QCOMPARE(table->item(0, 3)->text(), QString("TEST SDK 1"));
        QCOMPARE(table->item(0, 4)->text(), QString("TEST ASI120 Mono"));
        QCOMPARE(zwo->opened.load(), 0);
        // QHY: the SDK's failure, with what to install.
        QCOMPARE(table->item(1, 1)->text(), QString("SDK library not found"));
        table->selectRow(1);
        QVERIFY2(details->text().contains("TEST QHY devices failed"), qPrintable(details->text()));
        QVERIFY(details->text().contains("All-In-One"));
        // Player One: ready, no camera; a chosen DLL is used and shown at once, and can be forgotten.
        QCOMPARE(table->item(2, 1)->text(), QString("Ready; no camera connected"));
        QCOMPARE(table->item(2, 4)->text(), QString("none"));
        table->selectRow(2);
        auto* automatic = dialog.findChild<QPushButton*>("automaticCameraRuntime");
        QVERIFY(dialog.findChild<QPushButton*>("chooseCameraRuntime")->isEnabled());
        QVERIFY(!automatic->isEnabled());
        playerOnePanel.runtimeChooser()->setPath("C:/Vendor/PlayerOneCamera.dll");
        QCOMPARE(playerOne->runtimeOverride, QString("C:/Vendor/PlayerOneCamera.dll"));
        QCOMPARE(table->item(2, 2)->text(), QDir::toNativeSeparators("C:/Vendor/PlayerOneCamera.dll"));
        QVERIFY(details->text().contains("SDK library chosen"));
        QVERIFY(automatic->isEnabled());
        automatic->click();
        QVERIFY(playerOne->runtimeOverride.isEmpty());
        QVERIFY(!QSettings().contains("playerone/runtimePath"));
        QCOMPARE(playerOne->opened.load(), 0);
        // ToupTek: missing from this build, so listed but not offered.
        QCOMPARE(table->item(3, 1)->text(), QString("Not in this version"));
        table->selectRow(3);
        QVERIFY(details->text().contains("built without"));
        QVERIFY(!dialog.findChild<QPushButton*>("chooseCameraRuntime")->isEnabled());
    }
    // While a stream runs, nothing is checked or changed.
    const size_t calls = zwo->calls.size();
    CameraSupportDialog running({&zwoPanel}, true);
    running.checkAll();
    QCOMPARE(running.findChild<QTableWidget*>("cameraSupportTable")->item(0, 1)->text(), QString("Stop the stream to check"));
    QVERIFY(!running.findChild<QPushButton*>("checkCameraSupport")->isEnabled());
    QCOMPARE(zwo->calls.size(), calls);

    QTemporaryDir dir;
    MainWindow window(nullptr, dir.path(), zwo);
    QVERIFY(window.findChild<QAction*>("cameraSupportAction"));
}

// Feature-16: both new camera families through the window, with test cameras.
void UiTest::playerOneAndToupTekInTheWindow()
{
    QTemporaryDir dir;
    const QString savedRuntime = dir.filePath("Player One/PlayerOneCamera.dll");
    QSettings().setValue("playerone/runtimePath", savedRuntime);
    auto playerOne = std::make_shared<FakePlayerOneApi>();
    auto toupTek = std::make_shared<FakeToupTekApi>();
    MainWindow window(nullptr, dir.path(), {}, {}, playerOne, toupTek);
    window.show();
    QCOMPARE(playerOne->opened.load(), 0); // nothing is opened before its source is chosen
    auto* source = window.findChild<QComboBox*>("sourceComboBox");
    auto* start = window.findChild<QPushButton*>("startButton");
    auto* stop = window.findChild<QPushButton*>("stopButton");
    auto* monitoring = window.findChild<QPushButton*>("monitoringButton");
    auto* frames = window.findChild<QLabel*>("framesReadout");
    QVERIFY(source->findData("playerone") >= 2 && source->findData("touptek") > source->findData("playerone"));

    // Player One: the saved DLL choice reaches the SDK boundary, discovery fills the panel.
    source->setCurrentIndex(source->findData("playerone"));
    auto* poa = window.findChild<PlayerOneCameraPanel*>();
    QCOMPARE(playerOne->runtimeOverride, savedRuntime);
    QVERIFY(poa->hasCamera());
    QVERIFY(window.findChild<QDockWidget*>("playerOneCameraDock")->isVisible());
    QVERIFY(window.findChild<QLabel*>("playerOneReadback")->text().contains("TEST-POA-0001"));
    QVERIFY(start->isEnabled());
    QVERIFY(!window.findChild<QSpinBox*>("targetFpsSpinBox")->isEnabled());
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(playerOne->delivered >= 2, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(frames->text().contains("SDK drops: 0"), 3000);
    // A control changed in Preview applies live on the same connection and reads back.
    poa->findChild<QDoubleSpinBox*>("cameraControl1")->setValue(250);
    QTRY_COMPARE_WITH_TIMEOUT(poa->profile().current.controls.at(PlayerOneControl::Gain), int64_t{250}, 3000);
    QCOMPARE(playerOne->starts.load(), 1);
    // Monitoring reopens the camera with the requested settings.
    monitoring->click();
    QTRY_COMPARE_WITH_TIMEOUT(playerOne->starts.load(), 2, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("captureModeLabel")->text().contains("MONITORING"), 3000);
    QVERIFY(!poa->findChild<QPushButton*>("applyPlayerOneCamera")->isEnabled());
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 3000);
    QCOMPARE(playerOne->opened.load(), playerOne->closed.load());
    // Back to automatic discovery.
    window.findChild<QPushButton*>("automaticPlayerOneRuntime")->click();
    QVERIFY(playerOne->runtimeOverride.isEmpty());
    QVERIFY(!QSettings().contains("playerone/runtimePath"));

    // ToupTek: losses are counted from the camera's sequence numbers.
    toupTek->sequences = {7, 8, 10};
    source->setCurrentIndex(source->findData("touptek"));
    auto* toupTekPanel = window.findChild<ToupTekCameraPanel*>();
    QVERIFY(toupTekPanel->hasCamera());
    QVERIFY(window.findChild<QDockWidget*>("toupTekCameraDock")->isVisible());
    QVERIFY(window.findChild<QDockWidget*>("playerOneCameraDock")->isHidden());
    QVERIFY(window.findChild<QLabel*>("toupTekReadback")->text().contains("TEST-TT-0001"));
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(toupTek->delivered >= 4, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(frames->text().contains("SDK drops: 1"), 3000);
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 3000);
    QCOMPARE(toupTek->opened.load(), toupTek->closed.load());
    source->setCurrentIndex(source->findData("synthetic"));
    QVERIFY(window.findChild<QDockWidget*>("toupTekCameraDock")->isHidden());
}

void UiTest::previewShowsWouldTriggerWithoutSaving()
{
    QTemporaryDir dir;
    {
        MainWindow window(nullptr,dir.path()); window.show(); useSynthetic(window);
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
        // Feature-31: the synthetic Moon is all sunlit, above the flashes' ceiling:
        // the readout says a flash there is not tested. With the ceiling off it is.
        auto* blipStatus = window.findChild<QLabel*>("blipStatusLabel");
        QTRY_VERIFY2_WITH_TIMEOUT(blipStatus->text().contains("does not test it"), qPrintable(blipStatus->text()), 3000);
        window.findChild<QDoubleSpinBox*>("detectionIgnoreAbove")->setValue(0);
        QTest::mouseClick(preview,Qt::LeftButton,Qt::NoModifier,preview->imageRect().center());
        QVERIFY(!blipStatus->text().contains("does not test it"));
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
        MainWindow window(nullptr, dir.path()); window.show(); useSynthetic(window);
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
        const auto drag = [&] {
            QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, from);
            QTest::mouseMove(preview, from + QPoint(12, 9));
            QTest::mouseMove(preview, to);
            QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, to);
        };
        // Feature-28 + default pan: dragging without Draw lock box does not draw or lock;
        // Draw lock box enables one-shot region drawing, then returns left-drag to pan.
        QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Pan);
        drag();
        QCOMPARE(regions.size(), 0);
        QTest::qWait(300);
        QVERIFY(state->text().contains("Off"));
        auto* drawLock = window.findChild<QPushButton*>("drawLockBox");
        QVERIFY(drawLock->isEnabled());
        drawLock->click();
        QVERIFY(drawLock->isChecked());
        QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Region);
        drag();
        QVERIFY(!drawLock->isChecked());
        QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Pan);
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
    QVERIFY(!QFile::exists(eventsFolder(dir.path(), Target::Moon)+"/candidates.txt"));

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
    const QString moonDir=eventsFolder(dir.path(),Target::Moon);
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        int complete=0;
        for (const auto& folder:watchClips(moonDir))
            complete+=text(moonDir+"/"+folder+"/event.txt").contains("complete: yes");
        return complete>=2;
    })(),5000);
    int before=0,after=0;
    for (const auto& folder:watchClips(moonDir))
    {
        const auto note=text(moonDir+"/"+folder+"/event.txt");
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
    auto* panel=window.findChild<CameraPanel*>(); QVERIFY(panel);
    QCOMPARE(api->opened.load(),0); // Other source types never inspect hardware.
    chooseSource(window,"zwo"); QVERIFY(panel->hasCamera());
    auto* fps=window.findChild<QSpinBox*>("targetFpsSpinBox"); QVERIFY(!fps->isEnabled());
    QVERIFY(panel->findChild<QDoubleSpinBox*>("cameraControl0"));
    QCOMPARE(panel->findChild<QDoubleSpinBox*>("cameraControl0")->decimals(),3);
    QCOMPARE(panel->findChild<QDoubleSpinBox*>("cameraControl0")->singleStep(),1.0);
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
    QVERIFY(!QFile::exists(eventsFolder(dir.path(), Target::Moon)+"/candidates.txt"));
    QVERIFY(window.findChild<QDockWidget*>("detectionDock")->isVisible());
    QVERIFY(window.findChild<QDockWidget*>("cameraDock")->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("framesReadout")->text().contains("SDK drops: 3"),3000);
    // Unaligned ROI width auto-rounds to camera alignment rules (127 -> 128) on Apply.
    panel->findChild<QSpinBox*>("cameraWidth")->setValue(127);
    panel->findChild<QPushButton*>("applyCamera")->click();
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),2,3000);
    QVERIFY(stop->isEnabled()); QCOMPARE(api->stops.load(),1);
    QCOMPARE(panel->profile().current.width,128);
    panel->findChild<QDoubleSpinBox*>("cameraControl1")->setValue(13);
    auto* format=panel->findChild<QComboBox*>("cameraFormat");
    format->setCurrentIndex(format->findData(static_cast<int>(PixelFormat::Mono16)));
    panel->findChild<QSpinBox*>("cameraWidth")->setValue(64);
    panel->findChild<QPushButton*>("applyCamera")->click();
    QTRY_VERIFY_WITH_TIMEOUT(api->starts>=3,3000);
    QCOMPARE(panel->profile().current.format,PixelFormat::Mono16);
    QCOMPARE(panel->profile().current.width,64);
    QCOMPARE(panel->findChild<QDoubleSpinBox*>("cameraControl1")->value(),12.0);
    auto* histogram = window.findChild<QDockWidget*>("histogramDock");
    histogram->toggleViewAction()->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(histogram->findChild<QLabel*>("histogramSampleRange")->text().contains("At max code (65535)"), 3000);
    QVERIFY(!save->isEnabled());
    monitoring->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),4,3000);
    QVERIFY(!panel->findChild<QDoubleSpinBox*>("cameraControl1")->isEnabled());
    QVERIFY(window.findChild<QDoubleSpinBox*>("detectionSigma")->isEnabled());
    save->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    monitoring->click(); QTRY_COMPARE_WITH_TIMEOUT(monitoring->text(),QString("Start monitoring"),3000);
    QVERIFY(!save->isEnabled()); QCOMPARE(api->starts.load(),4); QCOMPARE(api->stops.load(),3);
    QVERIFY(panel->findChild<QDoubleSpinBox*>("cameraControl1")->isEnabled());
    panel->findChild<QDoubleSpinBox*>("cameraControl1")->setValue(20);
    QTRY_COMPARE_WITH_TIMEOUT(panel->profile().current.controls.at(ZwoControl::Gain),int64_t{20},3000);
    format->setCurrentIndex(format->findData(static_cast<int>(PixelFormat::Mono8)));
    panel->findChild<QSpinBox*>("cameraWidth")->setValue(128);
    panel->findChild<QPushButton*>("applyCamera")->click();
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),5,3000);
    monitoring->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(),6,3000);
    save->click(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    window.findChild<QDockWidget*>("cameraDock")->raise();
    window.findChild<DisplayPanel*>()->findChild<QComboBox*>("displayMode")->setCurrentIndex(2);
    QDir().mkpath(SELENEWATCH_CAMERA_QA_DIR);
    QVERIFY(window.grab().save(QString(SELENEWATCH_CAMERA_QA_DIR)+"/window-simulated.png"));
    api->connected=false;
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(),3000);
    const QString moonDir=eventsFolder(dir.path(),Target::Moon);
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        int complete=0;
        for (const auto& folder:watchClips(moonDir))
            complete+=text(moonDir+"/"+folder+"/event.txt").contains("complete: yes");
        return complete>=2;
    })(),5000);
    int old=0,changed=0;
    for (const auto& folder:watchClips(moonDir))
    {
        const auto note=text(moonDir+"/"+folder+"/event.txt");
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
    // The file is read off the window's thread, and its length shown when read.
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("fileLabel")->text().contains("48 frames, "),3000);
    auto* save=window.findChild<QPushButton*>("saveNowButton");
    QVERIFY(save); QVERIFY(!save->isEnabled());
    window.findChild<DisplayPanel*>()->findChild<QDoubleSpinBox*>("postSeconds")->setValue(.3);
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<PreviewWidget*>()->imageRect().isEmpty(),3000);
    window.findChild<QPushButton*>("monitoringButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(),3000);
    QVERIFY(save->isEnabled()); save->click();
    const QString moonOutput=latestWatch(eventsFolder(output,Target::Moon))+"/clips";
    QString folder;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        for (const auto& name:QDir(moonOutput).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
        {
            const QString candidate=moonOutput+"/"+name;
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
    QCOMPARE(panel->watch(), Watch::MoonFlashes);
    QCOMPARE(choice->count(), 4);
    QVERIFY(choice->isEnabled());
    QVERIFY(guiding->isEnabled());
    QCOMPARE(panel->settings().brighteningShare, 0.0);
    QCOMPARE(display->captureSettings().diskReserveBytes, uint64_t{10} * 1024 * 1024 * 1024);
    // Feature-31: the flashes leave out the sunlit side and never watch for darkening.
    QVERIFY(!panel->settings().darkening);
    QCOMPARE(panel->settings().ignoreAboveShare, 0.3);
    sigma->setValue(7);
    display->findChild<QDoubleSpinBox*>("postSeconds")->setValue(4);

    // The Moon's shadows: their own detection settings, the Moon's capture settings.
    chooseWatch(window, Watch::MoonShadows);
    QCOMPARE(panel->watch(), Watch::MoonShadows);
    QCOMPARE(panel->target(), Target::Moon);
    QVERIFY(panel->settings().darkening);
    QCOMPARE(panel->settings().sigma, 12.0);
    QCOMPARE(panel->settings().minPixels, 20);
    QVERIFY(!panel->settings().oneFrame);
    QCOMPARE(panel->settings().ignoreAboveShare, 0.0);
    QCOMPARE(display->captureSettings().target, Target::Moon);
    QCOMPARE(display->captureSettings().postSeconds, 4.0);
    QVERIFY(guiding->isEnabled());
    QCOMPARE(QSettings().value("detection/sigma").toDouble(), 7.0); // The flashes keep the Moon's keys.
    QCOMPARE(QSettings().value("moon/watch").toString(), QString("shadows"));
    sigma->setValue(14);

    chooseWatch(window, Watch::Sky);
    QVERIFY(!panel->settings().darkening);
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
    QCOMPARE(QSettings().value("moonShadows/detection/sigma").toDouble(), 14.0);
    QCOMPARE(QSettings().value("target").toInt(), 1);

    // A mask drawn on the sky, and a Sky K.
    panel->findChild<QToolButton*>("drawMask")->setChecked(true);
    QVERIFY(panel->addMaskRect(QRect(0, 400, 640, 80)));
    QCOMPARE(panel->settings().mask.count(), 1);
    sigma->setValue(9);

    chooseWatch(window, Watch::MoonFlashes);
    QCOMPARE(panel->settings().sigma, 7.0);
    QVERIFY(!panel->settings().darkening);
    QCOMPARE(panel->settings().maxPixels, 400);
    QCOMPARE(panel->settings().brighteningShare, 0.0);
    QVERIFY(panel->settings().mask.empty());
    QVERIFY(!panel->drawingMask());
    QCOMPARE(display->captureSettings().target, Target::Moon);
    QCOMPARE(display->captureSettings().maxPostSeconds, 10.0);
    QVERIFY(guiding->isEnabled());
    QCOMPARE(QSettings().value("sky/detection/sigma").toDouble(), 9.0);
    QCOMPARE(QSettings().value("sky/detection/mask").toString(), QString("0,400 640,400 640,480 0,480"));
    chooseWatch(window, Watch::MoonShadows);
    QCOMPARE(panel->settings().sigma, 14.0);

    chooseWatch(window, Watch::Sky);
    QCOMPARE(panel->settings().sigma, 9.0);
    QCOMPARE(panel->settings().mask.count(), 1);
    QVERIFY(panel->settings().mask.shapes()[0] == DetectionMask::rectangle(0, 400, 640, 80));
    QCOMPARE(QSettings().value("sky/watch").toString(), QString("fast"));
    auto* follower = panel->findChild<QDoubleSpinBox*>("followMaxExposure");
    QCOMPARE(follower->value(), 40.0); // Feature-35: the fast sky's follower stops at 40 ms.
    display->findChild<QDoubleSpinBox*>("postSeconds")->setValue(6);

    // Feature-35: the slow sky keeps its own detection, capture and follower
    // settings, and the sky's mask is its own too.
    chooseWatch(window, Watch::SkySlow);
    QCOMPARE(panel->watch(), Watch::SkySlow);
    QCOMPARE(panel->target(), Target::Sky);
    QVERIFY(panel->findChild<QWidget*>("skySettings")->isVisibleTo(panel));
    QCOMPARE(panel->settings().sigma, 6.0);
    QCOMPARE(panel->settings().referenceTimeConstantSeconds, 10.0);
    QCOMPARE(panel->settings().releaseSeconds, 60.0);
    QVERIFY(panel->settings().mask.empty());
    QCOMPARE(display->captureSettings().target, Target::Sky);
    QCOMPARE(display->captureSettings().historySeconds, 20.0);
    QCOMPARE(display->captureSettings().maxPostSeconds, 60.0);
    QCOMPARE(follower->value(), 2000.0);
    QVERIFY(!guiding->isEnabled());
    QCOMPARE(QSettings().value("sky/watch").toString(), QString("slow"));
    QCOMPARE(QSettings().value("sky/capture/post").toDouble(), 6.0);
    sigma->setValue(8);
    follower->setValue(1500);

    chooseWatch(window, Watch::Sky);
    QCOMPARE(panel->settings().sigma, 9.0);
    QCOMPARE(display->captureSettings().postSeconds, 6.0);
    QCOMPARE(follower->value(), 40.0);
    chooseWatch(window, Watch::SkySlow);
    QCOMPARE(panel->settings().sigma, 8.0);
    QCOMPARE(follower->value(), 1500.0);
    QCOMPARE(QSettings().value("skySlow/detection/sigma").toDouble(), 8.0);
}
// Feature-36: with the slow sky watched and its time-lapse on, Monitoring
// keeps a frame every interval in the watch's timelapse folder and builds the
// video as it goes; stopping finishes it and records it in session.txt. The
// fast sky has no time-lapse.
void UiTest::theSlowSkyKeepsATimelapseWhileMonitoring()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    useSynthetic(window);
    auto* panel = window.findChild<DetectionPanel*>();
    auto* timelapse = panel->findChild<QCheckBox*>("watchTimelapse");
    auto* interval = panel->findChild<QSpinBox*>("watchTimelapseInterval");
    chooseWatch(window, Watch::Sky);
    QVERIFY(!timelapse->isVisibleTo(panel));
    chooseWatch(window, Watch::SkySlow);
    QVERIFY(timelapse->isVisibleTo(panel));
    QCOMPARE(interval->value(), 30);
    timelapse->setChecked(true);
    interval->setValue(1);
    QCOMPARE(QSettings().value("skySlow/timelapse/intervalSeconds").toInt(), 1);

    window.findChild<QPushButton*>("startButton")->click();
    auto* monitoring = window.findChild<QPushButton*>("monitoringButton");
    QTRY_VERIFY_WITH_TIMEOUT(monitoring->isEnabled(), 5000);
    monitoring->click();
    auto* mode = window.findChild<QLabel*>("captureModeLabel");
    // Set before Monitoring starts.
    QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING") && !timelapse->isEnabled(), 5000);
    QTRY_VERIFY2_WITH_TIMEOUT(mode->text().contains("Time-lapse: 3 frames kept, one every 1 s."), qPrintable(mode->text()), 10000);
    monitoring->click();
    QTRY_VERIFY_WITH_TIMEOUT(!mode->text().contains("MONITORING") && timelapse->isEnabled(), 5000);

    const QString watch = latestWatch(eventsFolder(directory.path(), Target::Sky));
    const QDir folder(watch + "/timelapse");
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(folder.filePath("timelapse.txt")), 15000);
    QVERIFY(folder.entryList({"frame_*.png"}, QDir::Files).size() >= 3);
    QVERIFY(QFileInfo(folder.filePath("timelapse.mp4")).size() > 0);
    QVERIFY(text(folder.filePath("timelapse.txt")).contains("end_reason: monitoring stopped"));
    const QString session = watch + "/session.txt";
    QTRY_VERIFY_WITH_TIMEOUT(text(session).contains(" timelapse_stop frames="), 5000);
    QVERIFY(text(session).contains(" monitoring_start target=sky watch=sky_slow"));
    QVERIFY(text(session).contains(" timelapse_start interval_s=1 folder=timelapse"));
    QVERIFY(text(session).contains("video=timelapse/timelapse.mp4"));
    window.findChild<QPushButton*>("stopButton")->click();
}

// The disk reserve stops clips, recordings and time-lapses before the drive
// fills. It starts at 10 GiB for each target, and a changed value is kept for
// that target the next time the program opens.
void UiTest::diskReserveStartsAtTenGiBAndIsRemembered()
{
    QTemporaryDir directory;
    constexpr uint64_t kGiB = uint64_t{1024} * 1024 * 1024;
    {
        MainWindow window(nullptr, directory.path());
        auto* display = window.findChild<DisplayPanel*>();
        auto* reserve = display->findChild<QDoubleSpinBox*>("diskReserveGiB");
        QCOMPARE(reserve->value(), 10.0);
        reserve->setValue(25);
        QCOMPARE(display->captureSettings().diskReserveBytes, 25 * kGiB);

        chooseWatch(window, Watch::Sky);
        QCOMPARE(reserve->value(), 10.0);
        reserve->setValue(0);
        QCOMPARE(reserve->text(), QString("None"));
        QCOMPARE(display->captureSettings().diskReserveBytes, uint64_t{0});
    }
    MainWindow window(nullptr, directory.path());
    auto* display = window.findChild<DisplayPanel*>();
    QCOMPARE(display->captureSettings().target, Target::Sky);
    QCOMPARE(display->captureSettings().diskReserveBytes, uint64_t{0});
    chooseWatch(window, Watch::MoonFlashes);
    QCOMPARE(display->captureSettings().diskReserveBytes, 25 * kGiB);
}

// The controls whose text is cut off: drawn narrower or lower than their
// minimum size hint. Labels that shorten themselves with "…" and wrapping
// labels report a minimum that fits. A spin box's own text field is checked
// as part of the spin box.
static QStringList squeezedControls(QWidget& root)
{
    QStringList squeezed;
    for (QWidget* widget : root.findChildren<QWidget*>())
    {
        if (!widget->isVisible() || widget->window() != &root) continue;
        if (qobject_cast<QLineEdit*>(widget) && qobject_cast<QAbstractSpinBox*>(widget->parentWidget())) continue;
        const bool control = qobject_cast<QAbstractButton*>(widget) || qobject_cast<QLabel*>(widget)
            || qobject_cast<QComboBox*>(widget) || qobject_cast<QAbstractSpinBox*>(widget)
            || qobject_cast<QLineEdit*>(widget);
        if (!control) continue;
        const QSize need = widget->minimumSizeHint();
        if (widget->width() + 1 >= need.width() && widget->height() + 1 >= need.height()) continue;
        QString text;
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) text = button->text();
        else if (auto* label = qobject_cast<QLabel*>(widget)) text = label->text();
        else if (auto* combo = qobject_cast<QComboBox*>(widget)) text = combo->currentText();
        else if (auto* spin = qobject_cast<QAbstractSpinBox*>(widget)) text = spin->text();
        squeezed << QString("%1 '%2' \"%3\" %4x%5, needs %6x%7")
            .arg(widget->metaObject()->className(), widget->objectName(), text.left(40))
            .arg(widget->width()).arg(widget->height()).arg(need.width()).arg(need.height());
    }
    return squeezed;
}

// Feature-23: a maximised window on a small laptop, 1366 x 768 at 100 %, and
// 1920 x 1200 at 125 % (1536 x 960 logical pixels), less the title bar and
// task bar. A camera is chosen, so its dock is open on the left, and the
// testing tools are on, which adds the most controls. Every dock tab is
// shown in turn.
void UiTest::everyControlFitsOnASmallLaptopScreen()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path(), std::make_shared<FakeZwoApi>());
    window.findChild<QAction*>("testingToolsAction")->setChecked(true);
    chooseSource(window, "zwo");
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.findChild<QDockWidget*>("cameraDock")->isVisible());
    // Feature-26: with the auto-tune panel open too.
    window.findChild<QPushButton*>("autoTuneButton")->click();
    QStringList problems;
    for (const QSize size : {QSize(1366, 700), QSize(1536, 880)})
    {
        window.resize(size);
        for (QDockWidget* dock : window.findChildren<QDockWidget*>())
        {
            if (dock->isHidden()) continue;
            dock->raise();
            QApplication::processEvents();
            const QString where = QString("%1x%2, %3 tab: ").arg(size.width()).arg(size.height()).arg(dock->windowTitle());
            if (window.size() != size)
                problems << where + QString("the window cannot be smaller than %1x%2").arg(window.width()).arg(window.height());
            for (const QString& problem : squeezedControls(window))
                problems << where + problem;
        }
    }
    // The window itself can be made well narrower than a laptop screen, for
    // every watch: it can be resized in width, not only in height.
    for (const Watch watch : {Watch::MoonFlashes, Watch::MoonShadows, Watch::Sky})
    {
        emit window.findChild<DetectionPanel*>()->watchRequested(watch);
        window.resize(400, 300);
        QApplication::processEvents();
        if (window.width() > 1150)
            problems << QString("%1: the window cannot be narrower than %2 px").arg(watchName(watch)).arg(window.width());
    }
    problems.removeDuplicates();
    for (const QString& problem : problems) qWarning().noquote() << problem;
    QVERIFY2(problems.isEmpty(), qPrintable(QString("%1 problems").arg(problems.size())));
}

// Feature-31 (after Feature-22): darkening is the shadows' watch and only
// theirs, the brightness ceiling the flashes' only, whatever was saved.
void UiTest::darkeningIsForTheMoonOnly()
{
    DetectionPanel panel;
    auto* share = panel.findChild<QDoubleSpinBox*>("darkeningShare");
    auto* ceiling = panel.findChild<QDoubleSpinBox*>("detectionIgnoreAbove");
    QVERIFY(!panel.findChild<QCheckBox*>("darkeningTrigger")); // No longer a choice of its own.

    panel.setWatch(Watch::MoonShadows);
    panel.setSettings(defaultDetectionSettings(Watch::MoonShadows));
    QVERIFY(share->isVisibleTo(&panel) && !ceiling->isVisibleTo(&panel));
    QVERIFY(panel.settings().darkening);
    QCOMPARE(panel.settings().sigma, 12.0);
    QCOMPARE(panel.settings().ignoreAboveShare, 0.0);

    // Flashes: an older Moon setting with darkening on is the flashes' now, without it.
    panel.setWatch(Watch::MoonFlashes);
    DetectionSettings old = defaultDetectionSettings(Target::Moon);
    old.darkening = true;
    old.darkeningShare = 0.05;
    panel.setSettings(old);
    QVERIFY(!share->isVisibleTo(&panel) && ceiling->isVisibleTo(&panel));
    QVERIFY(!panel.settings().darkening);
    QCOMPARE(panel.settings().darkeningShare, 0.0);
    ceiling->setValue(40);
    QCOMPARE(panel.settings().ignoreAboveShare, 0.4);

    panel.setWatch(Watch::Sky);
    DetectionSettings saved = defaultDetectionSettings(Target::Sky);
    saved.darkening = true;
    saved.darkeningShare = 0.05;
    panel.setSettings(saved);
    QVERIFY(!share->isVisibleTo(&panel) && !ceiling->isVisibleTo(&panel));
    QVERIFY(!panel.settings().darkening);
    QCOMPARE(panel.settings().darkeningShare, 0.0);
    QCOMPARE(panel.settings().ignoreAboveShare, 0.0);
}

// Feature-45: in the shadows' watch the controls at the top are the dark
// patch limits and the readouts show falls. The bright limits, which have no
// control there, keep their values.
void UiTest::shadowsUseTheLimitsAtTheTop()
{
    DetectionPanel panel;
    auto* minimum = panel.findChild<QSpinBox*>("detectionMinPixels");
    auto* maximum = panel.findChild<QSpinBox*>("detectionMaxPixels");
    auto* box = panel.findChild<QDoubleSpinBox*>("detectionMaxBoxRatio");
    QVERIFY(minimum && maximum && box);
    QVERIFY(!panel.findChild<QSpinBox*>("detectionMaxDarkPixels"));
    QVERIFY(!panel.findChild<QDoubleSpinBox*>("detectionMaxDarkBoxRatio"));
    QVERIFY(!panel.findChild<QLabel*>("darkReading"));

    panel.setWatch(Watch::MoonShadows);
    panel.setSettings(defaultDetectionSettings(Watch::MoonShadows));
    QCOMPARE(maximum->value(), 20000);
    QCOMPARE(box->value(), 10.0);
    QVERIFY(maximum->toolTip().contains("shadow"));
    maximum->setValue(5000);
    box->setValue(8);
    QCOMPARE(panel.settings().maxDarkPixels, 5000);
    QCOMPARE(panel.settings().maxDarkBoxRatio, 8.0);
    QCOMPARE(panel.settings().maxPixels, 400);
    QCOMPARE(panel.settings().maxPatchBoxRatio, 4.0);
    // Only the dark maximum limits the minimum here, not the bright one.
    minimum->setValue(500);
    QCOMPARE(panel.settings().minPixels, 500);
    QVERIFY(panel.settings().valid());
    // A maximum set below the minimum takes the minimum with it.
    maximum->setValue(300);
    QCOMPARE(panel.settings().minPixels, 300);
    QCOMPARE(panel.settings().maxDarkPixels, 300);
    QCOMPARE(panel.settings().maxPixels, 400);

    DetectionView view;
    view.summary.ready = true;
    view.summary.settings = panel.settings();
    view.summary.peakSigma = 15;
    panel.setView(view);
    auto* reading = panel.findChild<QLabel*>("sigmaReading");
    QVERIFY2(reading->text().startsWith("Deepest fall 15.0 / K 12.0"), qPrintable(reading->text()));
    QVERIFY(panel.findChild<QLabel*>("detectionState")->text().contains("Deepest fall ("));

    // The flashes: the same controls are the bright limits again.
    panel.setWatch(Watch::MoonFlashes);
    panel.setSettings(defaultDetectionSettings(Watch::MoonFlashes));
    QCOMPARE(maximum->value(), 400);
    QCOMPARE(box->value(), 4.0);
    QVERIFY(!maximum->toolTip().contains("shadow"));
    view.summary.settings = panel.settings();
    panel.setView(view);
    QVERIFY2(reading->text().startsWith("Peak 15.0 / K 6.0"), qPrintable(reading->text()));
}

// Feature-24: every control in the window and its docks explains itself:
// a tool tip, the same text as What's This, and a ⓘ on the label naming it.
// Checked for both targets with a camera chosen and the testing tools on.
void UiTest::everyControlHasHelp()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path(), std::make_shared<FakeZwoApi>());
    window.findChild<QAction*>("testingToolsAction")->setChecked(true);
    chooseSource(window, "zwo");
    QVERIFY(window.findChild<QAction*>("whatsThisAction"));
    QStringList missing;
    for (const Watch watch : {Watch::MoonFlashes, Watch::MoonShadows, Watch::Sky})
    {
        chooseWatch(window, watch);
        for (QWidget* widget : window.findChildren<QWidget*>())
        {
            // The auto-tune window is part of the window (Feature-28).
            if ((widget->window() != &window && widget->window()->objectName() != "autoTuneWindow")
                || widget->objectName().startsWith("qt_")) continue;
            if (qobject_cast<QScrollBar*>(widget) || qobject_cast<QTabBar*>(widget->parentWidget())) continue;
            if (qobject_cast<QLineEdit*>(widget) && (qobject_cast<QAbstractSpinBox*>(widget->parentWidget())
                    || qobject_cast<QComboBox*>(widget->parentWidget()))) continue;
            const bool control = qobject_cast<QAbstractButton*>(widget) || qobject_cast<QComboBox*>(widget)
                || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QAbstractSlider*>(widget)
                || qobject_cast<QLineEdit*>(widget);
            if (!control) continue;
            if (widget->toolTip().isEmpty() || widget->whatsThis().isEmpty())
            {
                QString text;
                if (auto* button = qobject_cast<QAbstractButton*>(widget)) text = button->text();
                missing << QString("%1 '%2' \"%3\" in %4").arg(widget->metaObject()->className(), widget->objectName(), text,
                    widget->parentWidget() ? widget->parentWidget()->objectName() : QString());
            }
        }
    }
    missing.removeDuplicates();
    for (const QString& line : missing) qWarning().noquote() << "no help:" << line;
    QVERIFY2(missing.isEmpty(), qPrintable(QString("%1 controls without help").arg(missing.size())));

    // The label naming a camera control and a top-row label carry the ⓘ.
    QLabel* gain = nullptr;
    for (QLabel* label : window.findChildren<QLabel*>())
        if (label->text().startsWith("Gain ")) gain = label;
    QVERIFY(gain && gain->text().contains("ⓘ"));
    auto* fps = window.findChild<QLabel*>("fpsLabel");
    QVERIFY(fps->text().contains("ⓘ"));
    QVERIFY(fps->text().startsWith("Target FPS:"));
    emit fps->linkActivated("help");
    QWhatsThis::hideText();

    // The exposure follower's fixed rules are stated where its limits are
    // entered, including on the ⓘ of the Exposure row.
    for (const char* name : {"followSky", "followMinExposure", "followMaxExposure", "followMaxGain"})
    {
        const QString help = window.findChild<QWidget*>(name)->toolTip();
        for (const char* rule : {"between 8% and 30%", "for 60 s", "toward 15%", "at most 2", "at least 120 s apart"})
            QVERIFY2(help.contains(rule), qPrintable(QString("%1: %2").arg(name, rule)));
    }
    bool exposureRowHasHelp = false;
    for (QLabel* label : window.findChildren<QLabel*>())
        exposureRowHasHelp |= label->text().startsWith("Exposure <a") && label->buddy() == nullptr && label->text().contains("ⓘ");
    QVERIFY(exposureRowHasHelp);
}

// A number box is never capped narrower than its widest value with its
// suffix ("40.00 ms" was cut to "40.00 r" in the sky's exposure follower).
void UiTest::numberBoxesFitTheirWidestValue()
{
    // Checked in the tests' Fusion style and in Windows 11's, whose boxes are
    // wider and which the observer sees.
    const QString testsStyle = QApplication::style()->name();
    QStringList clipped;
    for (const QString& style : {QString("fusion"), QString("windows11")})
    {
        if (!QStyleFactory::keys().contains(style, Qt::CaseInsensitive)) continue;
        QApplication::setStyle(style);
        QTemporaryDir directory;
        MainWindow window(nullptr, directory.path(), std::make_shared<FakeZwoApi>());
        window.findChild<QAction*>("testingToolsAction")->setChecked(true);
        chooseSource(window, "zwo");
        for (QAbstractSpinBox* box : window.findChildren<QAbstractSpinBox*>())
            if (box->maximumWidth() < box->sizeHint().width())
                clipped << QString("%1, %2: at most %3 px, needs %4").arg(style, box->objectName())
                               .arg(box->maximumWidth()).arg(box->sizeHint().width());
    }
    QApplication::setStyle(testsStyle);
    for (const QString& line : clipped) qWarning().noquote() << "clipped:" << line;
    QVERIFY2(clipped.isEmpty(), qPrintable(QString("%1 number boxes narrower than their values").arg(clipped.size())));
}

// Feature-26: with a camera in Preview, auto-tune measures the brightness,
// applies the ticked suggestions through the live path and checks them once
// the camera reports back; then it watches what each K would have done, and
// applies the suggested K only while nothing has changed since the watch.
// Every applied change is logged in candidates.txt.
void UiTest::autoTuneTunesTheCameraLiveThenWatchesK()
{
    QTemporaryDir dir;
    auto api = std::make_shared<FakeZwoApi>();
    // Feature-34: a dark sky whose level follows the camera: 5 codes of black
    // level plus 0.1 per millisecond of exposure, doubled every 20 gain units.
    // Its noise: 20 electrons a code at gain 0, and a read noise of 6
    // electrons that drops to 1.5 from gain 70 (a high-conversion-gain mode).
    api->light = [](double exposureUs, double gain) { return 5 + 0.1 * exposureUs / 1000 * std::pow(2.0, gain / 20); };
    api->lightNoise = [](double exposureUs, double gain) {
        const double electrons = 0.1 * exposureUs / 1000 * 20;
        const double read = gain >= 70 ? 1.5 : 6;
        return std::pow(2.0, gain / 20) / 20 * std::sqrt(read * read + electrons);
    };
    MainWindow window(nullptr, dir.path(), api);
    window.findChild<QAction*>("testingToolsAction")->setChecked(true);
    window.show();
    chooseSource(window, "zwo");
    chooseWatch(window, Watch::Sky);
    auto* referenceTime = window.findChild<QDoubleSpinBox*>("detectionReferenceTime");
    referenceTime->setValue(0.5); // The detector learns in 1.5 s, so the K watch starts soon.
    auto* toggle = window.findChild<QPushButton*>("autoTuneButton");
    QVERIFY(toggle && toggle->isVisible());
    toggle->click();
    auto* panel = window.findChild<AutoTunePanel*>();
    QVERIFY(panel->isVisible());
    auto* tune = panel->findChild<QPushButton*>("autoTuneCamera");
    QVERIFY(!tune->isEnabled()); // Not before Preview.
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(tune->isEnabled(), 5000);
    auto* camera = window.findChild<CameraPanel*>();
    auto* exposureBox = camera->findChild<QDoubleSpinBox*>("cameraControl0");
    auto* gainBox = camera->findChild<QDoubleSpinBox*>("cameraControl1");
    const double exposureBefore = exposureBox->value();
    const double gainBefore = gainBox->value();
    auto* restore = panel->findChild<QPushButton*>("autoTuneRestoreCamera");
    QVERIFY(!restore->isEnabled());

    // Step 1: one press. The dark sky lengthens the exposure to the fast
    // sky's longest (40 ms, Feature-35), then raises gain, live, until it is
    // in the aim; then gains are compared by their noise, with dark rounds
    // for the black level, and the chosen one is set at 40 ms.
    tune->click();
    auto* status = panel->findChild<QLabel*>("autoTuneStatus");
    auto* rounds = panel->findChild<QLabel*>("autoTuneCameraRounds");
    QTRY_VERIFY2_WITH_TIMEOUT(status->text().startsWith("Tuned in"), qPrintable(status->text() + " | " + rounds->text()), 120000);
    QVERIFY2(status->text().contains("faintest threshold") && rounds->text().contains(", dark ("), qPrintable(rounds->text()));
    QVERIFY2(rounds->text().startsWith("1. The sky is at") && rounds->text().count('\n') >= 2, qPrintable(rounds->text()));
    auto* verdict = panel->findChild<QLabel*>("autoTuneCameraVerdict");
    QVERIFY2(verdict->text().contains("aim is 8-30 %") && verdict->text().contains("It is in the aim"), qPrintable(verdict->text()));
    QCOMPARE(camera->profile().current.controls.at(ZwoControl::Exposure), int64_t{40'000});
    QVERIFY(gainBox->value() > gainBefore);
    // The loop owns exposure and gain: no rows for them are left to apply.
    QVERIFY(!panel->findChild<QCheckBox*>(QString("autoTuneRow%1").arg(static_cast<int>(TuneRow::Kind::Gain))));
    const QString log = eventsFolder(dir.path(), Target::Sky) + "/candidates.txt";
    QTRY_VERIFY_WITH_TIMEOUT(text(log).contains(" autotune_camera target=sky round=1 measured_level="), 3000);
    QVERIFY(text(log).contains(" autotune_readback exposure="));
    // The choice is logged with what was compared, and is the camera's gain now.
    const QString choice = text(log).section(" autotune_gain_choice ", 1).section('\n', 0, 0);
    QVERIFY2(choice.startsWith("target=sky exposure_ms=40 gain=") && choice.contains(" snr="), qPrintable(choice));
    QCOMPARE(gainBox->value(), choice.section("gain=", 1).section(' ', 0, 0).toDouble());
    QVERIFY(restore->isEnabled());

    // Step 2: the K watch, shortened for the test.
    auto* length = panel->findChild<QComboBox*>("autoTuneLength");
    length->addItem("Test (1 s)", 1.0);
    length->setCurrentIndex(length->count() - 1);
    auto* watch = panel->findChild<QPushButton*>("autoTuneWatch");
    QTRY_VERIFY_WITH_TIMEOUT(watch->isEnabled(), 3000);
    // The follower pauses while auto-tune measures.
    auto* follow = window.findChild<QCheckBox*>("followSky");
    follow->setChecked(true);
    watch->click();
    window.checkSkyExposure(std::chrono::steady_clock::now());
    QVERIFY(window.findChild<QLabel*>("followReading")->text().contains("Paused while auto-tune measures"));
    auto* table = panel->findChild<QTableWidget*>("autoTuneKTable");
    QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() == 4, 15000);
    QCOMPARE(table->item(0, 0)->text().left(3), QString("5.0"));
    auto* applyK = panel->findChild<QPushButton*>("autoTuneApplyK");
    QVERIFY2(applyK->isEnabled(), qPrintable(panel->findChild<QLabel*>("autoTuneKVerdict")->text()));
    QVERIFY(panel->findChild<QLabel*>("autoTuneKVerdict")->text().contains("Suggested K 5.0"));
    // A detection setting changed after the watch: the result is withdrawn.
    auto* oneFrame = window.findChild<QCheckBox*>("oneFrameTrigger");
    oneFrame->setChecked(!oneFrame->isChecked());
    QVERIFY(!applyK->isEnabled());
    QVERIFY(panel->findChild<QLabel*>("autoTuneKVerdict")->text().contains("detection settings changed"));
    oneFrame->setChecked(!oneFrame->isChecked());
    watch->click();
    QTRY_VERIFY_WITH_TIMEOUT(applyK->isEnabled(), 15000);
    applyK->click();
    QCOMPARE(window.findChild<QDoubleSpinBox*>("detectionSigma")->value(), 5.0);
    QVERIFY(text(log).contains(" autotune_k target=sky k_from=6.0 k_to=5.0 k_tested=\""));

    // The flash check, with the testing tools: a 100 ADU flash on a flat sky is caught.
    auto* check = panel->findChild<QPushButton*>("autoTuneCheckFlash");
    QVERIFY(check->isVisible() && check->isEnabled());
    check->click();
    auto* checked = panel->findChild<QLabel*>("autoTuneCheckResult");
    QTRY_VERIFY_WITH_TIMEOUT(checked->text().startsWith("Caught") || checked->text().startsWith("Missed"), 10000);
    QVERIFY2(checked->text().startsWith("Caught"), qPrintable(checked->text()));

    // Restore previous settings: the exposure and gain from before the loop.
    restore->click();
    QCOMPARE(exposureBox->value(), exposureBefore);
    QCOMPARE(gainBox->value(), gainBefore);
    QVERIFY(text(log).contains(" autotune_camera target=sky restore=yes applied=\""));
    QTRY_VERIFY_WITH_TIMEOUT(!restore->isEnabled() && tune->isEnabled(), 5000);

    // During a recording, no camera change and no test flash.
    window.findChild<QPushButton*>("recordButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!check->isEnabled(), 3000);
    window.findChild<QPushButton*>("recordButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(check->isEnabled(), 3000);
    // Monitoring: auto-tune waits for Preview.
    window.findChild<QPushButton*>("monitoringButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("captureModeLabel")->text().contains("MONITORING"), 5000);
    QVERIFY(!watch->isEnabled() && !tune->isEnabled());
    window.findChild<QPushButton*>("stopButton")->click();
}

// Feature-26: on the Moon, auto-tune needs a box on the earthshine. A frame
// with no darker sky beyond the Moon's edge measures the box alone, and says so.
void UiTest::autoTuneOutlinesTheEarthshineBox()
{
    QTemporaryDir dir;
    auto api = std::make_shared<FakeZwoApi>();
    MainWindow window(nullptr, dir.path(), api);
    window.show();
    chooseSource(window, "zwo");
    window.findChild<QPushButton*>("autoTuneButton")->click();
    auto* panel = window.findChild<AutoTunePanel*>();
    auto* tune = panel->findChild<QPushButton*>("autoTuneCamera");
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("captureModeLabel")->text().contains("PREVIEW"), 5000);
    QVERIFY(panel->findChild<QToolButton*>("autoTuneDrawBox")->isVisible());
    QVERIFY(!tune->isEnabled()); // No box yet.
    window.setEarthshineBox(QRect(40, 20, 24, 16));
    QTRY_VERIFY_WITH_TIMEOUT(panel->findChild<QLabel*>("autoTuneArea")->text().contains("box alone"), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(tune->isEnabled(), 3000);
    QCOMPARE(QSettings().value("autotune/earthshineBox").toRect(), QRect(40, 20, 24, 16));
    tune->click();
    // The fake camera's fixed 41 % is far above the earthshine's 3-10 %: the
    // exposure is brought into 10-40 ms first, then the gain goes to its
    // lowest, and the loop stops: the camera cannot get darker.
    auto* status = panel->findChild<QLabel*>("autoTuneStatus");
    auto* rounds = panel->findChild<QLabel*>("autoTuneCameraRounds");
    QTRY_VERIFY2_WITH_TIMEOUT(status->text().startsWith("Stopped after"), qPrintable(status->text() + " | " + rounds->text()), 30000);
    QVERIFY2(rounds->text().contains("→ 10 ms, into this watch's range") && rounds->text().contains("cannot reach"),
        qPrintable(rounds->text()));
    QVERIFY(panel->findChild<QLabel*>("autoTuneCameraVerdict")->text().contains("The earthshine is at"));
    window.findChild<QPushButton*>("stopButton")->click();
}
// Feature-30: for flashes, auto-tune measures the footprint of a flash just
// above K, of the width assumed from the seeing (or the Test flash FWHM
// without an image scale), watches smaller minimum patches at the current K,
// suggests the smallest quiet one within the footprint, applies it and logs it.
void UiTest::autoTuneSuggestsTheMinimumPatchForFlashes()
{
    QTemporaryDir dir;
    QSettings().remove("autotune");
    auto api = std::make_shared<FakeZwoApi>();
    MainWindow window(nullptr, dir.path(), api);
    window.show();
    chooseSource(window, "zwo");
    chooseWatch(window, Watch::MoonFlashes);
    // The fake camera's frames are at 41 % of full scale: the ceiling is off.
    window.findChild<QDoubleSpinBox*>("detectionIgnoreAbove")->setValue(0);
    window.findChild<QDoubleSpinBox*>("detectionReferenceTime")->setValue(0.5);
    window.findChild<QPushButton*>("autoTuneButton")->click();
    auto* panel = window.findChild<AutoTunePanel*>();
    panel->setMinPatchSeconds(1);
    auto* width = panel->findChild<QLabel*>("autoTuneFlashWidth");
    QVERIFY2(width->text().contains("assumed 3.0 px"), qPrintable(width->text())); // No image scale.
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("captureModeLabel")->text().contains("PREVIEW"), 5000);
    window.setEarthshineBox(QRect(40, 20, 24, 16));
    auto* measure = panel->findChild<QPushButton*>("autoTuneMinPatchWatch");
    QTRY_VERIFY_WITH_TIMEOUT(measure->isEnabled(), 5000);
    measure->click();
    auto* verdict = panel->findChild<QLabel*>("autoTuneMinPatchVerdict");
    QTRY_VERIFY2_WITH_TIMEOUT(verdict->text().startsWith("Suggested minimum patch"),
        qPrintable(verdict->text() + " | " + panel->findChild<QLabel*>("autoTuneFootprint")->text()), 20000);
    QVERIFY(panel->findChild<QLabel*>("autoTuneFootprint")->text().contains(" covers "));
    QCOMPARE(panel->findChild<QTableWidget*>("autoTuneMinPatchTable")->rowCount(), 4);
    // The fake camera has no noise: the smallest value, 3 px, is quiet and within the footprint.
    QVERIFY2(verdict->text().startsWith("Suggested minimum patch 3 px"), qPrintable(verdict->text()));
    auto* apply = panel->findChild<QPushButton*>("autoTuneApplyMinPatch");
    QVERIFY(apply->isEnabled());
    apply->click();
    QCOMPARE(window.findChild<QSpinBox*>("detectionMinPixels")->value(), 3);
    QTRY_VERIFY_WITH_TIMEOUT(text(eventsFolder(dir.path(), Target::Moon) + "/candidates.txt").contains(
        " autotune_min_patch target=moon k=6.0 min_from=8 min_to=3 "), 3000);
    // With only the focal length, the camera's pixel size gives the scale:
    // 3.75 µm at 1000 mm is 0.77″ per pixel, so 2.5″ seeing is 3.2 px.
    ObservingSite focalOnly;
    focalOnly.focalLengthMm = 1000;
    window.applySite(focalOnly, false, "pool.ntp.org");
    window.findChild<QPushButton*>("autoTuneButton")->click();
    QVERIFY2(width->text().contains("about 3.2 px wide (2.5″ seeing at 0.77″ per pixel, with the pixel size the camera reports)"),
        qPrintable(width->text()));
    // With an image scale the width comes from the seeing: 2.5″ at 0.5″ per pixel is 5 px.
    ObservingSite site;
    site.focalLengthMm = 1196;
    site.pixelSizeUm = 2.9;
    window.applySite(site, false, "pool.ntp.org");
    window.findChild<QPushButton*>("autoTuneButton")->click();
    QVERIFY2(width->text().contains("about 5.0 px wide (2.5″ seeing at 0.50″ per pixel)"), qPrintable(width->text()));
    window.findChild<QPushButton*>("stopButton")->click();
}
// Feature-28: auto-tune opens in its own window, leaving the Detection dock as
// it is. A drag on the preview draws only what the observer chose: a guiding
// lock box (Guiding) or an earthshine box (Auto-tune), one at a time, each in
// its own colour; with nothing chosen it draws nothing and says how to.
void UiTest::aDragDrawsOnlyWhatWasChosen()
{
    QTemporaryDir dir;
    QSettings().remove("autotune");
    MainWindow window(nullptr, dir.path());
    window.show();
    useSynthetic(window);
    window.findChild<QPushButton*>("startButton")->click();
    auto* preview = window.findChild<PreviewWidget*>();
    QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(), 3000);

    auto* autoTuneWindow = window.findChild<QDialog*>("autoTuneWindow");
    QVERIFY(autoTuneWindow && !autoTuneWindow->isVisible());
    auto* dock = window.findChild<QDockWidget*>("detectionDock");
    const QSize dockBefore = dock->size();
    window.findChild<QPushButton*>("autoTuneButton")->click();
    QVERIFY(autoTuneWindow->isVisible());
    QCOMPARE(window.findChild<AutoTunePanel*>()->window(), static_cast<QWidget*>(autoTuneWindow));
    QCOMPARE(dock->size(), dockBefore);

    const QPoint from = preview->imageRect().center() - QPoint(20, 15);
    const QPoint to = preview->imageRect().center() + QPoint(20, 15);
    const auto drag = [&] {
        QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(preview, from + QPoint(10, 8));
        QTest::mouseMove(preview, to);
        QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, to);
    };
    auto* lockState = window.findChild<QLabel*>("lockStateLabel");
    // With nothing chosen, dragging while zoomed pans instead of drawing a box.
    preview->setZoom(2.0);
    drag();
    QVERIFY(preview->panOffset() != QPointF(0.0, 0.0));
    QVERIFY(!QSettings().contains("autotune/earthshineBox"));
    QVERIFY(lockState->text().contains("Off"));
    preview->setFitToWindow();

    // Choosing one drawing turns the other off and switches left-drag to Region mode.
    auto* drawLock = window.findChild<QPushButton*>("drawLockBox");
    auto* drawEarthshine = window.findChild<QToolButton*>("autoTuneDrawBox");
    drawLock->click();
    QVERIFY(drawLock->isChecked());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Region);
    drawEarthshine->click();
    QVERIFY(drawEarthshine->isChecked());
    QVERIFY(!drawLock->isChecked());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Region);
    drag();
    QVERIFY(!drawEarthshine->isChecked());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Pan);
    QVERIFY(QSettings().value("autotune/earthshineBox").toRect().isValid());
    QTest::qWait(300);
    QVERIFY(lockState->text().contains("Off")); // The earthshine box is not a lock.
    drawEarthshine->click();
    drawLock->click();
    QVERIFY(!drawEarthshine->isChecked());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Region);
    drag();
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Pan);
    QTRY_VERIFY_WITH_TIMEOUT(lockState->text().contains("Locked"), 5000);

    // Closing the window ends a box being drawn and returns left-drag to Pan mode.
    drawEarthshine->click();
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Region);
    autoTuneWindow->close();
    QVERIFY(!drawEarthshine->isChecked());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Pan);
    window.findChild<QPushButton*>("stopButton")->click();
}

// Feature-29: the site and instrument can be saved under a name and loaded back.
void UiTest::sitePresetsSaveLoadAndDelete()
{
    QSettings().remove("site/presets");
    SiteDialog dialog;
    auto* combo = dialog.findChild<QComboBox*>("sitePresetCombo");
    auto* name = dialog.findChild<QLineEdit*>("sitePresetName");
    auto* save = dialog.findChild<QPushButton*>("sitePresetSave");
    auto* remove = dialog.findChild<QPushButton*>("sitePresetDelete");
    QCOMPARE(combo->count(), 1);
    QVERIFY(!save->isEnabled() && !remove->isEnabled());

    ObservingSite home;
    home.observer = "Observer A";
    home.site = "Home";
    home.latitudeDeg = 37.98; home.longitudeDeg = 23.72; home.heightM = 120;
    home.telescope = "8-inch SCT";
    home.apertureMm = 203; home.focalLengthMm = 2032; home.pixelSizeUm = 2.9;
    ObservingSite club = home;
    club.site = "Club field";
    club.latitudeDeg.reset(); club.longitudeDeg.reset(); club.heightM.reset();
    club.telescope = "80 mm refractor";
    club.apertureMm = 80; club.focalLengthMm = 480;

    dialog.setSite(home);
    name->setText("Home, 8-inch");
    QVERIFY(save->isEnabled());
    save->click();
    dialog.setSite(club);
    name->setText("Club");
    save->click();
    QCOMPARE(combo->count(), 3);
    QCOMPARE(loadSitePresets().size(), size_t{2});

    // Choosing a preset fills the form.
    const int homeIndex = combo->findData("Home, 8-inch");
    QVERIFY(homeIndex > 0);
    combo->setCurrentIndex(homeIndex);
    emit combo->activated(homeIndex);
    QCOMPARE(dialog.site(), home);
    QVERIFY(remove->isEnabled());
    const int clubIndex = combo->findData("Club");
    combo->setCurrentIndex(clubIndex);
    emit combo->activated(clubIndex);
    QCOMPARE(dialog.site(), club);
    QVERIFY(!dialog.site().hasPosition());

    // Saving under the same name replaces; deleting removes.
    club.telescope = "80 mm refractor, reducer";
    dialog.setSite(club);
    save->click();
    QCOMPARE(loadSitePresets().size(), size_t{2});
    remove->click();
    QCOMPARE(loadSitePresets().size(), size_t{1});
    QCOMPARE(loadSitePresets().front().name, QString("Home, 8-inch"));
    QCOMPARE(combo->count(), 2);
    QSettings().remove("site/presets");
}
// Feature-25/39: a ranged camera control has a slider beside its box; Exposure
// steps through fixed presets in three ranges (Planetary, Fast, Sky) while the
// box keeps the camera's full range and fine-tuning arrows. In Preview a drag
// reaches the camera once, after the pause, and Watching selects the range.
void UiTest::cameraSlidersSetTheBoxAndReachTheCameraOnce()
{
    CameraSettingsEditor editor(2, 2, nullptr);
    editor.addControl(1, "Gain", 0, 600, 1, 100, 1, "");
    editor.addControl(2, "Exposure", 32, 2'000'000'000, 100, 10'000, 1000, " ms");
    editor.addControl(3, "High speed mode", 0, 1, 1, 0, 1, "");
    editor.addControl(4, "Temperature", -500, 500, 1, 200, 1, "", false);
    auto* gain = editor.findChild<QSlider*>("cameraControlSlider1");
    auto* exposure = editor.findChild<QSlider*>("cameraControlSlider2");
    auto* exposureBox = editor.findChild<QDoubleSpinBox*>("cameraControl2");
    auto* rangeBox = editor.findChild<QComboBox*>("cameraExposureRange");
    QVERIFY(gain && exposure && exposureBox && rangeBox);
    QVERIFY(!editor.findChild<QSlider*>("cameraControlSlider3"));
    QVERIFY(!editor.findChild<QSlider*>("cameraControlSlider4"));
    QVERIFY(!gain->toolTip().isEmpty());
    QVERIFY(!rangeBox->toolTip().isEmpty());

    // Gain: one slider position per step.
    QCOMPARE(gain->maximum(), 600);
    QCOMPARE(gain->value(), 100);
    gain->setValue(250);
    QCOMPARE(*editor.controlValue(1), 250.0);
    QVERIFY(editor.setControlValue(1, 400));
    QCOMPARE(gain->value(), 400);

    // Exposure defaults to Fast (5-100 ms) with 1 ms arrow step, while the box
    // keeps the full hardware range for auto-tune, follower, and typed values.
    QCOMPARE(editor.exposureRange(), ExposureRange::Fast);
    QCOMPARE(exposureBox->singleStep(), 1.0);
    const auto fullRange = editor.controlRange(2);
    QVERIFY(fullRange.has_value());
    QCOMPARE(fullRange->first, 32.0);
    QCOMPARE(fullRange->second, 2e9);
    exposure->setValue(0);
    QCOMPARE(*editor.controlValue(2), 5'000.0);
    exposure->setValue(exposure->maximum());
    QCOMPARE(*editor.controlValue(2), 100'000.0);
    // Fine-tuning via spin-box arrows keeps the exact value without snapping back.
    exposureBox->setValue(16.0);
    exposureBox->stepUp();
    QCOMPARE(*editor.controlValue(2), 17'000.0);

    // Switching range updates preset stops and arrow step without changing the
    // live exposure value or firing a live camera request.
    editor.setLiveEditing(true);
    QSignalSpy requested(&editor, &CameraSettingsEditor::liveControlsRequested);
    rangeBox->setCurrentIndex(rangeBox->findData(static_cast<int>(ExposureRange::Planetary)));
    QCOMPARE(editor.exposureRange(), ExposureRange::Planetary);
    QCOMPARE(exposureBox->singleStep(), 0.1);
    QCOMPARE(*editor.controlValue(2), 17'000.0);
    QTest::qWait(250);
    QCOMPARE(requested.count(), 0);
    exposure->setValue(0);
    QCOMPARE(*editor.controlValue(2), 50.0);
    exposure->setValue(exposure->maximum());
    QCOMPARE(*editor.controlValue(2), 20'000.0);

    rangeBox->setCurrentIndex(rangeBox->findData(static_cast<int>(ExposureRange::Sky)));
    QCOMPARE(editor.exposureRange(), ExposureRange::Sky);
    QCOMPARE(exposureBox->singleStep(), 50.0);
    // Current value (20 ms) stays untouched until the slider moves.
    QCOMPARE(*editor.controlValue(2), 20'000.0);
    exposure->setValue(exposure->maximum());
    QCOMPARE(*editor.controlValue(2), 5'000'000.0);
    exposure->setValue(0);
    QCOMPARE(*editor.controlValue(2), 50'000.0);
    // Typed values above 5 s still work in the box.
    exposureBox->setValue(10'000.0);
    QCOMPARE(*editor.controlValue(2), 10'000'000.0);

    // Coalescing: a slider drag reaches the camera once, after the pause.
    requested.clear();
    QTest::qWait(250);
    requested.clear();
    for (int value = 300; value <= 340; value += 4) gain->setValue(value);
    QTRY_COMPARE(requested.count(), 1);
    QTest::qWait(400);
    QCOMPARE(requested.count(), 1);
    QCOMPARE(*editor.controlValue(1), 340.0);

    // Narrow hardware exposure limits filter the preset tables cleanly.
    editor.clearControls();
    editor.setExposureRange(ExposureRange::Planetary);
    editor.addControl(2, "Exposure", 1'000, 2'000'000, 100, 5'000, 1000, " ms");
    exposure = editor.findChild<QSlider*>("cameraControlSlider2");
    QVERIFY(exposure);
    exposure->setValue(0);
    QCOMPARE(*editor.controlValue(2), 1'000.0);
    editor.setExposureRange(ExposureRange::Sky);
    exposure->setValue(exposure->maximum());
    QCOMPARE(*editor.controlValue(2), 2'000'000.0);

    // MainWindow automatically matches the camera Exposure range to Watching,
    // both while stopped and during live Preview, without altering exposure.
    QTemporaryDir dir;
    auto api = std::make_shared<FakeZwoApi>();
    MainWindow window(nullptr, dir.path(), api);
    chooseSource(window, "zwo");
    auto* camPanel = window.findChild<CameraPanel*>();
    QVERIFY(camPanel && camPanel->controls());
    QCOMPARE(camPanel->controls()->exposureRange(), ExposureRange::Fast);
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(api->delivered >= 2, 3000);
    const double initialExposure = *camPanel->controls()->controlValue(0);
    chooseWatch(window, Watch::MoonShadows);
    QTRY_VERIFY(window.findChild<QComboBox*>("targetChoice")->isEnabled());
    QCOMPARE(camPanel->controls()->exposureRange(), ExposureRange::Planetary);
    QCOMPARE(*camPanel->controls()->controlValue(0), initialExposure);
    chooseWatch(window, Watch::SkySlow);
    QTRY_VERIFY(window.findChild<QComboBox*>("targetChoice")->isEnabled());
    QCOMPARE(camPanel->controls()->exposureRange(), ExposureRange::Sky);
    QCOMPARE(*camPanel->controls()->controlValue(0), initialExposure);
    chooseWatch(window, Watch::Sky);
    QTRY_VERIFY(window.findChild<QComboBox*>("targetChoice")->isEnabled());
    QCOMPARE(camPanel->controls()->exposureRange(), ExposureRange::Fast);
    window.findChild<QPushButton*>("stopButton")->click();
}

// Feature-14: the site dialog keeps what is entered, the window saves it,
// and Record is offered only while Monitoring.
void UiTest::siteAndRecordingControls()
{
    SiteDialog dialog;
    ObservingSite site;
    site.observer = "A. Observer";
    site.latitudeDeg = 37.98; site.longitudeDeg = 23.73; site.heightM = 100;
    site.telescope = "C9.25"; site.focalLengthMm = 2350; site.pixelSizeUm = 3.75;
    dialog.setSite(site);
    dialog.setClockCheck(true, "time.example.org");
    QCOMPARE(dialog.site(), site);
    QVERIFY(dialog.ntpCheck());
    QCOMPARE(dialog.ntpServer(), QString("time.example.org"));
    QVERIFY(dialog.findChild<QLabel*>("siteScale")->text().startsWith("0.329"));
    dialog.findChild<QCheckBox*>("sitePositionKnown")->setChecked(false);
    QVERIFY(!dialog.site().hasPosition());

    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    useSynthetic(window);
    window.applySite(site, false, "pool.ntp.org");
    QCOMPARE(QSettings().value("site/telescope").toString(), QString("C9.25"));
    QCOMPARE(QSettings().value("site/latitude").toDouble(), 37.98);
    QCOMPARE(QSettings().value("time/ntp").toBool(), false);
    auto* record = window.findChild<QPushButton*>("recordButton");
    auto* recordFrames = window.findChild<QSpinBox*>("recordFramesSpinBox");
    QVERIFY(record && recordFrames);
    QVERIFY(!record->isEnabled());
    // An unwritable output folder is refused; the existing root stays.
    QFile blocker(QDir(directory.path()).filePath("blocker"));
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    QString error;
    QVERIFY(!window.applyOutputFolder(QDir(directory.path()).filePath("blocker/out"), error));
    QVERIFY(error.contains("could not create") || error.contains("cannot write") || error.contains("not a folder"));

    // Preview manual recording works with an optional frame limit and leaves no candidates.txt.
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(record->isEnabled(), 3000);
    recordFrames->setValue(5);
    record->click();
    QTRY_VERIFY_WITH_TIMEOUT(record->text() == QString("Record") &&
        QDir(recordingsFolder(directory.path())).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() == 1, 5000);
    window.findChild<QPushButton*>("stopButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!record->isEnabled(), 3000);
    QVERIFY(!QFile::exists(QDir(eventsFolder(directory.path(), Target::Moon)).filePath("candidates.txt")));

    recordFrames->setValue(0);
    const auto recordInMonitoring = [&] {
        window.findChild<QPushButton*>("startButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPushButton*>("monitoringButton")->isEnabled(), 3000);
        window.findChild<QPushButton*>("monitoringButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("captureModeLabel")->text().contains("MONITORING"), 3000);
        record->click();
        // Feature-33: while recording, the button is Stop with the recording's length.
        QTRY_VERIFY_WITH_TIMEOUT(record->text().startsWith("Stop ") && record->text().contains(':'), 3000);
        QVERIFY(!record->styleSheet().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("captureModeLabel")->text().contains("RECORDING"), 3000);
        record->click();
        QTRY_COMPARE_WITH_TIMEOUT(record->text(), QString("Record"), 3000);
        QVERIFY(record->styleSheet().isEmpty());
        window.findChild<QPushButton*>("stopButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!record->isEnabled(), 3000);
    };
    recordInMonitoring();
    QCOMPARE(QDir(recordingsFolder(directory.path())).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), qsizetype{1});
    QCOMPARE(QDir(latestWatch(eventsFolder(directory.path(), Target::Moon)) + "/recordings").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), qsizetype{1});

    // A chosen output folder routes recordings, events/moon, and events/sky underneath it.
    QTemporaryDir elsewhere;
    const QString chosen = QDir(elsewhere.path()).filePath("long nights");
    QVERIFY(window.applyOutputFolder(chosen, error));
    recordInMonitoring();
    const QStringList made = QDir((latestWatch(eventsFolder(chosen, Target::Moon)) + "/recordings")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(made.size(), qsizetype{1});
    QVERIFY(QFile::exists(QDir(QDir((latestWatch(eventsFolder(chosen, Target::Moon)) + "/recordings")).filePath(made.front())).filePath("recording.ser")));
    const QString moonLog = latestWatch(eventsFolder(chosen, Target::Moon)) + "/candidates.txt";
    QTRY_VERIFY_WITH_TIMEOUT(text(moonLog).contains(" recording_start name=" + made.front() + " "), 3000);
    QVERIFY(text(moonLog).contains("folder=\"" + QDir::toNativeSeparators(QDir::cleanPath((latestWatch(eventsFolder(chosen, Target::Moon)) + "/recordings"))) + "\""));
}

// The release (year.month-number) is in the title and the About text, with
// the source revision and the licenses.
void UiTest::theReleaseIsShown()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    const QRegularExpression release(R"(SeleneWatch \d{4}\.\d{2}-\d{2} )");
    QVERIFY2(release.match(window.windowTitle()).hasMatch(), qPrintable(window.windowTitle()));
    const QString about = MainWindow::aboutText();
    QVERIFY2(about.contains(QRegularExpression(R"(<b>SeleneWatch \d{4}\.\d{2}-\d{2}</b>)")), qPrintable(about));
    QVERIFY(about.contains("Source revision "));
    QVERIFY(about.contains("BSD 2-Clause") && about.contains("LGPL"));
    QVERIFY(window.findChild<QAction*>("userManualAction"));
    QVERIFY(window.findChild<QAction*>("tipsAction"));
    QVERIFY(window.findChild<QAction*>("aboutAction"));
    QVERIFY(window.findChild<QAction*>("openEventsAction"));
    // Help → Acknowledgements shows ACKNOWLEDGEMENTS.md, built in.
    QVERIFY(window.findChild<QAction*>("acknowledgementsAction"));
    QVERIFY(about.contains("Acknowledgements"));
    const QString acknowledgements = MainWindow::acknowledgementsMarkdown();
    QVERIFY(acknowledgements.startsWith("# Acknowledgements"));
    QVERIFY(acknowledgements.contains("## Scientific References & Algorithms"));
    QVERIFY(acknowledgements.contains("**Welford (1962)**"));
    QVERIFY(acknowledgements.contains("### AI Coding Assistants"));
    QVERIFY(acknowledgements.contains("**Claude** by Anthropic") && acknowledgements.contains("**Codex** by OpenAI"));
    QFile source(QString(SELENEWATCH_SOURCE_DIR) + "/ACKNOWLEDGEMENTS.md");
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(acknowledgements, QString::fromUtf8(source.readAll()));
}

void UiTest::aNewReleaseAppearsAtTheTopRight()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    useSynthetic(window);
    window.resize(1280, 900);
    window.show();
    auto* notice = window.findChild<QToolButton*>("updateNoticeButton");
    auto* checker = window.findChild<ReleaseChecker*>();
    QVERIFY(notice && checker);
    QVERIFY(notice->isHidden());
    QCOMPARE(window.menuBar()->cornerWidget(Qt::TopRightCorner), notice);

    window.findChild<QPushButton*>("startButton")->click();
    auto* frames = window.findChild<QLabel*>("framesReadout");
    QTRY_VERIFY(frames->text() != "Frames: 0");
    const QString beforeNotice = frames->text();

    checker->newerReleaseAvailable("v2026.11-02");
    QTRY_VERIFY(notice->isVisible());
    QCOMPARE(notice->text(), QString("New version v2026.11-02 available"));
    QVERIFY(notice->isEnabled());
    QVERIFY(!QApplication::activeModalWidget());
    QTRY_VERIFY(frames->text() != beforeNotice);
    QVERIFY(window.findChild<QPushButton*>("stopButton")->isEnabled());

    ReleaseUrlReceiver receiver;
    struct ResetUrlHandler { ~ResetUrlHandler() { QDesktopServices::unsetUrlHandler("https"); } } reset;
    QDesktopServices::setUrlHandler("https", &receiver, "open");
    notice->click();
    QCOMPARE(receiver.opened, ReleaseChecker::releasesUrl());

    const QString screenshots = QString(SELENEWATCH_SOURCE_DIR) + "/out/qa-update";
    QVERIFY(QDir().mkpath(screenshots));
    QVERIFY(window.grab().save(screenshots + "/new-release.png"));
    window.resize(800, 600);
    QTest::qWait(10);
    QVERIFY(window.menuBar()->rect().contains(notice->geometry()));
    QVERIFY(window.grab().save(screenshots + "/new-release-narrow.png"));
    window.findChild<QPushButton*>("stopButton")->click();
}

// Feature-15, Done-when 2 in the window: the exposure follower.
void UiTest::exposureFollower()
{
    {
        // The fake camera's sky is 42% of full scale: too bright.
        QTemporaryDir directory;
        auto api = std::make_shared<FakeZwoApi>();
        MainWindow window(nullptr, directory.path(), api);
        window.show();
        auto* panel = window.findChild<DetectionPanel*>();
        auto* camera = window.findChild<CameraPanel*>();
        auto* mode = window.findChild<QLabel*>("captureModeLabel");
        chooseWatch(*panel, Watch::Sky);
        chooseSource(window, "zwo");
        SkyExtras extras;
        extras.follow = true;
        extras.minExposureMs = 0.1;
        extras.maxExposureMs = 100;
        panel->setSkyExtras(extras);
        window.findChild<QPushButton*>("startButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(api->delivered >= 2, 3000);
        window.findChild<QPushButton*>("monitoringButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);
        QTest::qWait(300);
        const auto t0 = std::chrono::steady_clock::now();
        window.checkSkyExposure(t0);
        QVERIFY2(panel->findChild<QLabel*>("followReading")->text().startsWith("Sky at 4"),
            qPrintable(panel->findChild<QLabel*>("followReading")->text()));
        QCOMPARE(camera->profile().current.controls.at(ZwoControl::Exposure), int64_t{1000});
        window.checkSkyExposure(t0 + std::chrono::seconds{61});
        // Half the exposure, applied by starting Monitoring again.
        QTRY_COMPARE_WITH_TIMEOUT(camera->profile().current.controls.at(ZwoControl::Exposure), int64_t{500}, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);
        window.findChild<QPushButton*>("stopButton")->click();
        const QDir targetDir(eventsFolder(directory.path(), Target::Sky));
        QCOMPARE(targetDir.entryList({"session-*"}, QDir::Dirs).size(), qsizetype{1});
        const QString watch = latestWatch(targetDir.path());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(watch + "/report.html"), 5000);
        QVERIFY(text(watch + "/candidates.txt").contains(" exposure_change exposure_ms=1 to 0.5 "));
        QCOMPARE(text(watch + "/session.txt").count(" monitoring_start "), 2);
        QCOMPARE(text(watch + "/session.txt").count(" monitoring_stop "), 2);
    }
}

// Feature-15, Done-when 4 in the window: clicked corners, closed by a
// double-click, make a mask shape that is saved.
void UiTest::aMaskShapeIsDrawnOnThePreview()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    auto* panel = window.findChild<DetectionPanel*>();
    auto* preview = window.findChild<PreviewWidget*>();
    chooseWatch(*panel, Watch::Sky);
    panel->findChild<QToolButton*>("drawMask")->setChecked(true);
    emit preview->positionSelected(QPoint(0, 479));
    emit preview->positionSelected(QPoint(639, 479));
    emit preview->positionSelected(QPoint(320, 300));
    emit preview->positionSelected(QPoint(320, 300)); // The release after a double-click.
    window.closeMaskShape();
    QCOMPARE(panel->settings().mask.count(), 1);
    QVERIFY(panel->settings().mask.shapes()[0] == (MaskShape{{0, 479}, {639, 479}, {320, 300}}));
    // Two corners are not a shape.
    emit preview->positionSelected(QPoint(10, 10));
    emit preview->positionSelected(QPoint(20, 10));
    window.closeMaskShape();
    QCOMPARE(panel->settings().mask.count(), 1);
    // Switching away saves the sky's mask.
    chooseWatch(*panel, Watch::MoonFlashes);
    QCOMPARE(QSettings().value("sky/detection/mask").toString(), QString("0,479 639,479 320,300"));
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
        useSynthetic(window);
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
        // Feature-31: the flash goes on the all-sunlit synthetic Moon, so the
        // flashes' ceiling is off (chosen has it at 0).
        panel->findChild<QDoubleSpinBox*>("detectionIgnoreAbove")->setValue(chosen.ignoreAboveShare);
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
        const QString moonOut = latestWatch(eventsFolder(output.path(), Target::Moon))+"/clips";
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto dirs = QDir(moonOut).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const auto& dir : dirs)
            {
                const QString path = QDir(moonOut).filePath(dir);
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
        window.setTestingTools(true); // the test flash on a recording
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
            QDir events(latestWatch(eventsFolder(QDir(output.path()).filePath("events"), Target::Moon))+"/clips");
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

void UiTest::outputFolderSafetyAndMigration()
{
    // Non-cloud local default and cloud-sync heuristic.
    const QString defRoot = defaultOutputFolder();
    QVERIFY(!defRoot.isEmpty());
    QVERIFY(!isCloudSyncedPath(defRoot));
    QVERIFY(isCloudSyncedPath("C:/Users/Alice/OneDrive/SeleneWatch"));
    QVERIFY(isCloudSyncedPath("D:/Dropbox/Astro"));
    QVERIFY(!isCloudSyncedPath("D:/Astro/SeleneWatch"));

    // Unplugged / missing drive letter check never silently falls back.
    QString missingDrive;
    for (char letter = 'Z'; letter >= 'G'; --letter)
    {
        const QString cand = QString("%1:/").arg(letter);
        if (!QDir(cand).exists())
        {
            missingDrive = QString("%1:/MissingSeleneWatch").arg(letter);
            break;
        }
    }
    if (!missingDrive.isEmpty())
    {
        const OutputFolderCheck chk = checkOutputFolder(missingDrive, 1024ULL * 1024, false);
        QVERIFY(!chk.ok);
        QVERIFY(chk.mountMissing);
    }

    // Legacy events/ files are left untouched during migration.
    QTemporaryDir legacyTmp;
    QVERIFY(legacyTmp.isValid());
    const QString legacyClip = legacyTmp.filePath("events/20260101-000000-000/clip.txt");
    QDir().mkpath(QFileInfo(legacyClip).absolutePath());
    {
        QFile f(legacyClip);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("legacy evidence untouched");
    }
    QSettings().setValue("recording/folder", legacyTmp.filePath("old_rec"));
    migrateOutputSettings();
    QCOMPARE(QSettings().value("settingsVersion").toInt(), kCurrentSettingsVersion);
    QVERIFY(!QSettings().contains("recording/folder"));
    QVERIFY(QFileInfo::exists(legacyClip));

    // Persistence across MainWindow restart + refusal when custom folder vanishes.
    QTemporaryDir rootTmp;
    QVERIFY(rootTmp.isValid());
    const QString customRoot = rootTmp.filePath("AstroOut");
    QDir().mkpath(customRoot);

    QSettings().setValue("output/folder", customRoot);
    {
        MainWindow w1;
        QCOMPARE(QDir::cleanPath(QSettings().value("output/folder").toString()), QDir::cleanPath(customRoot));
    }
    QCOMPARE(QDir::cleanPath(QSettings().value("output/folder").toString()), QDir::cleanPath(customRoot));

    MainWindow window;
    useSynthetic(window);
    auto* startBtn = window.findChild<QPushButton*>("startButton");
    auto* stopBtn = window.findChild<QPushButton*>("stopButton");
    QPushButton* monitorBtn = nullptr;
    for (auto* b : window.findChildren<QPushButton*>())
    {
        if (b->objectName() == "monitorButton" || b->objectName() == "startMonitoringButton"
            || b->text().contains("Start monitoring", Qt::CaseInsensitive))
        {
            monitorBtn = b;
            break;
        }
    }
    auto* recordBtn = window.findChild<QPushButton*>("recordButton");
    auto* modeLabel = window.findChild<QLabel*>("captureModeLabel");
    auto* tlPanel = window.findChild<TimelapsePanel*>("timelapsePanel");
    QVERIFY(startBtn);
    QVERIFY(stopBtn);
    QVERIFY(monitorBtn);
    QVERIFY(recordBtn);
    QVERIFY(modeLabel);
    QVERIFY(tlPanel);
    auto* tlStartBtn = tlPanel->findChild<QPushButton*>("timelapseStartButton");
    QVERIFY(tlStartBtn);

    startBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(recordBtn->isEnabled(), 5000);

    // Delete the remembered custom output folder while Preview is active;
    // Monitoring, Manual Record, and Time-lapse must all refuse without fallback.
    QVERIFY(QDir(customRoot).removeRecursively());

    monitorBtn->click();
    QCoreApplication::processEvents();
    QVERIFY(!modeLabel->text().contains("MONITORING"));

    recordBtn->click();
    QCoreApplication::processEvents();
    QVERIFY(!recordBtn->text().contains("Stop"));

    tlStartBtn->click();
    QCoreApplication::processEvents();
    QVERIFY(!tlStartBtn->text().contains("Stop"));

    stopBtn->click();
    QVERIFY(!QDir(customRoot).exists());
}

void UiTest::watchesCanChangeInPreviewAndTransitControlsFollowTheMoon()
{
    QTemporaryDir dir;
    MainWindow window(nullptr, dir.path());
    useSynthetic(window);
    auto* panel = window.findChild<DetectionPanel*>();
    auto* watch = panel->findChild<QComboBox*>("targetChoice");
    auto* timeLapse = window.findChild<TimelapsePanel*>();
    auto* transits = timeLapse->findChild<QCheckBox*>("timelapseWatchTransits");
    auto* configure = timeLapse->findChild<QPushButton*>("timelapseConfigureTransits");
    auto* start = window.findChild<QPushButton*>("startButton");
    auto* monitoring = window.findChild<QPushButton*>("monitoringButton");
    auto* record = window.findChild<QPushButton*>("recordButton");
    auto* startLapse = timeLapse->findChild<QPushButton*>("timelapseStartButton");
    QVERIFY(!transits->isChecked());
    start->click();
    QTRY_VERIFY(watch->isEnabled() && monitoring->isEnabled() && !start->isEnabled());

    chooseWatch(window, Watch::SkySlow);
    QTRY_VERIFY(watch->isEnabled());
    QCOMPARE(panel->watch(), Watch::SkySlow);
    QCOMPARE(panel->settings().referenceTimeConstantSeconds, 10.0);
    QVERIFY(transits->isHidden());
    QVERIFY(!window.findChild<GuidingPanel*>()->isEnabled());
    chooseWatch(window, Watch::MoonFlashes);
    QTRY_VERIFY(watch->isEnabled());
    QVERIFY(!transits->isHidden());
    QVERIFY(window.findChild<GuidingPanel*>()->isEnabled());

    // The configure button opens the existing watch without stopping Preview.
    transits->setChecked(true);
    QVERIFY(configure->isEnabled());
    configure->click();
    QTRY_VERIFY(watch->isEnabled());
    QCOMPARE(panel->watch(), Watch::MoonShadows);
    QCOMPARE(panel->settings().sigma, 12.0);
    QVERIFY(!start->isEnabled());
    panel->findChild<QDoubleSpinBox*>("detectionSigma")->setValue(15.0);
    chooseWatch(window, Watch::Sky);
    QTRY_VERIFY(watch->isEnabled());
    chooseWatch(window, Watch::MoonShadows);
    QTRY_VERIFY(watch->isEnabled());
    QCOMPARE(panel->settings().sigma, 15.0);

    record->click();
    QTRY_VERIFY(!watch->isEnabled() && record->text().contains("Stop"));
    record->click();
    QTRY_VERIFY(watch->isEnabled());
    monitoring->click();
    QTRY_VERIFY(!watch->isEnabled() && monitoring->text().contains("Stop"));
    QVERIFY(!startLapse->isEnabled());
    monitoring->click();
    QTRY_VERIFY(watch->isEnabled());

    auto settings = timeLapse->settings();
    settings.intervalSeconds = 2;
    settings.captureWindowSeconds = 0.3;
    settings.video.writeMp4 = false;
    timeLapse->setSettings(settings);
    startLapse->click();
    QTRY_VERIFY(!watch->isEnabled() && !transits->isEnabled() && startLapse->text().contains("Stop"));
    auto* transitStatus = timeLapse->findChild<QLabel*>("timelapseTransitStatus");
    QTRY_VERIFY(!transitStatus->text().isEmpty());
    // Feature-44: Detection controls and Auto-tune lock during time-lapse, and
    // a banner states that active shadow/transit monitoring is taking place.
    auto* autoTuneBtn = panel->findChild<QPushButton*>("autoTuneButton");
    auto* sigmaBox = panel->findChild<QDoubleSpinBox*>("detectionSigma");
    auto* tlBanner = panel->findChild<QLabel*>("detectionTimeLapseBanner");
    QVERIFY(autoTuneBtn && !autoTuneBtn->isEnabled());
    QVERIFY(sigmaBox && !sigmaBox->isEnabled());
    QVERIFY(tlBanner && !tlBanner->isHidden() && tlBanner->text().contains("shadows / transits"));
    startLapse->click();
    QTRY_VERIFY(watch->isEnabled());
    QVERIFY(autoTuneBtn->isEnabled() && sigmaBox->isEnabled() && tlBanner->isHidden());
    QVERIFY(!timeLapse->findChild<QPushButton*>("timelapseOpenTransits")->isHidden());

    auto* preset = timeLapse->findChild<QComboBox*>("timelapsePreset");
    preset->setCurrentIndex(1);
    QVERIFY(transits->isHidden());
    QVERIFY(!timeLapse->settings().watchTransits);
}

void UiTest::timelapsePanelAndResharpenWorkflow()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MainWindow window(nullptr, dir.path());
    useSynthetic(window);

    auto* tl = window.findChild<TimelapsePanel*>("timelapsePanel");
    QVERIFY(tl);
    auto* preset = tl->findChild<QComboBox*>("timelapsePreset");
    auto* interval = tl->findChild<QDoubleSpinBox*>("timelapseInterval");
    auto* winSpin = tl->findChild<QDoubleSpinBox*>("timelapseWindow");
    auto* keepPct = tl->findChild<QDoubleSpinBox*>("timelapseKeepPercent");
    auto* maxFrames = tl->findChild<QSpinBox*>("timelapseMaxFrames");
    auto* selectAnchor = tl->findChild<QToolButton*>("timelapseSelectAnchor");
    auto* anchorLabel = tl->findChild<QLabel*>("timelapseAnchorLabel");
    auto* startTl = tl->findChild<QPushButton*>("timelapseStartButton");
    auto* statsLabel = tl->findChild<QLabel*>("timelapseStats");
    QVERIFY(preset && interval && winSpin && keepPct && maxFrames && selectAnchor && anchorLabel && startTl && statsLabel);

    // Preset switch: Moon <-> Planet.
    preset->setCurrentIndex(1); // Planet
    QCOMPARE(keepPct->value(), 15.0);
    QCOMPARE(maxFrames->value(), 200);
    QVERIFY(!selectAnchor->isEnabled());
    preset->setCurrentIndex(0); // Moon
    QCOMPARE(keepPct->value(), 25.0);
    QCOMPARE(maxFrames->value(), 150);

    // window <= interval invariant in both directions.
    interval->setValue(1.0);
    winSpin->setValue(2.0);
    QCOMPARE(interval->value(), 2.0);
    interval->setValue(0.5);
    QCOMPARE(winSpin->value(), 0.5);

    // Start Preview so we can select a sunlit lunar anchor by dragging on PreviewWidget.
    window.findChild<QPushButton*>("startButton")->click();
    auto* preview = window.findChild<PreviewWidget*>();
    QVERIFY(preview);
    QTRY_VERIFY_WITH_TIMEOUT(preview->imageRect().width() > 40 && selectAnchor->isEnabled(), 5000);

    selectAnchor->setChecked(true);
    const QRect imgRect = preview->imageRect();
    const QPoint p0(imgRect.left() + imgRect.width() / 4, imgRect.top() + imgRect.height() / 4);
    const QPoint p1(imgRect.left() + (imgRect.width() * 3) / 4, imgRect.top() + (imgRect.height() * 3) / 4);
    QTest::mousePress(preview, Qt::LeftButton, Qt::NoModifier, p0);
    QTest::mouseMove(preview, (p0 + p1) / 2);
    QTest::mouseMove(preview, p1);
    QTest::mouseRelease(preview, Qt::LeftButton, Qt::NoModifier, p1);
    QTRY_VERIFY_WITH_TIMEOUT(tl->settings().lunarAnchor.has_value(), 2000);
    QVERIFY(anchorLabel->text().contains("Anchor ("));

    // Configure rapid synthetic session and start / run / stop Time-lapse.
    interval->setValue(0.45);
    winSpin->setValue(0.25);
    tl->findChild<QComboBox*>("timelapseStopKind")->setCurrentIndex(0); // Manual stop
    tl->findChild<QCheckBox*>("timelapseMp4")->setChecked(true);
    tl->findChild<QCheckBox*>("timelapseGif")->setChecked(false);
    tl->findChild<QSpinBox*>("timelapseVideoFps")->setValue(5);

    startTl->click();
    QTRY_VERIFY_WITH_TIMEOUT(startTl->text().contains("Stop"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(statsLabel->text().contains("Stacks: 2"), 15000);

    auto* timingLabel = tl->findChild<QLabel*>("timelapseTiming");
    QVERIFY(timingLabel);

    startTl->click(); // Stop time-lapse manually -> builds MP4
    QTRY_VERIFY_WITH_TIMEOUT(timingLabel->text().startsWith("Finished ("), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(startTl->text().contains("Start"), 5000);

    const QString tlRoot = timelapsesFolder(dir.path());
    const QStringList sessions = QDir(tlRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QCOMPARE(sessions.size(), 1);
    const QString sessionDir = QDir(tlRoot).filePath(sessions.first());
    QVERIFY(QDir(QDir(sessionDir).filePath("stacks")).entryList({"*.fits"}, QDir::Files).size() >= 2);
    QVERIFY(QDir(QDir(sessionDir).filePath("sharpened")).entryList({"*.fits"}, QDir::Files).size() >= 2);
    QVERIFY2(QFileInfo::exists(QDir(sessionDir).filePath("timelapse.mp4")),
        qPrintable(timingLabel->text() + " | " + tl->findChild<QLabel*>("timelapseWarning")->text()));
    // Flash detection stays OFF during Time-lapse: no candidates log created.
    QVERIFY(!QFileInfo::exists(QDir(eventsFolder(dir.path(), Target::Moon)).filePath("candidates.txt")));

    // Test ResharpenDialog: opens session, previews best stack, applies & saves profile.
    SavedSharpeningProfile customProf;
    customProf.name = "UI Crisp";
    customProf.sharpening = sharpeningForPreset(SharpeningPreset::Medium);
    customProf.sharpening.amount = 1.8;
    customProf.sharpening.profileName = "UI Crisp";
    customProf.video.gamma = 1.45;
    customProf.video.autoStretch = false;
    saveNamedSharpeningProfile(customProf);
    tl->refreshProfiles();
    auto* sharpCombo = tl->findChild<QComboBox*>("timelapseSharpening");
    const int profIdx = sharpCombo->findData("profile:UI Crisp");
    QVERIFY(profIdx >= 0);
    sharpCombo->setCurrentIndex(profIdx);
    // Saved profile in Time-lapse panel must carry over its saved video tone settings too.
    QVERIFY(std::abs(tl->settings().video.gamma - 1.45) < 1e-4);
    QVERIFY(!tl->settings().video.autoStretch);

    // Feature-32: a duration and an end time are the session's Ends setting;
    // the time-lapse's own choice is when the session ends, or a number of stacks.
    auto* stopKindCombo = tl->findChild<QComboBox*>("timelapseStopKind");
    QCOMPARE(stopKindCombo->count(), 2);
    QVERIFY(!tl->findChild<QWidget*>("timelapseUtcEnd") && !tl->findChild<QWidget*>("timelapseDurationMin"));
    stopKindCombo->setCurrentIndex(1);
    QCOMPARE(tl->settings().stopKind, StopConditionKind::StackCount);
    stopKindCombo->setCurrentIndex(0);

    ResharpenDialog dlg(tlRoot, sessionDir, &window);
    dlg.show();
    QCoreApplication::processEvents();
    auto* stackCombo = dlg.findChild<QComboBox*>("resharpenStackCombo");
    auto* beforeView = dlg.findChild<PreviewWidget*>("resharpenBefore");
    auto* afterView = dlg.findChild<PreviewWidget*>("resharpenAfter");
    auto* zoomFit = dlg.findChild<QToolButton*>("resharpenZoomFit");
    auto* zoom100 = dlg.findChild<QToolButton*>("resharpenZoom100");
    auto* zoom200 = dlg.findChild<QToolButton*>("resharpenZoom200");
    auto* zoom400 = dlg.findChild<QToolButton*>("resharpenZoom400");
    auto* zoomLbl = dlg.findChild<QLabel*>("resharpenZoomLabel");
    auto* statusLbl = dlg.findChild<QLabel*>("resharpenStatus");
    auto* applyBtn = dlg.findChild<QPushButton*>("resharpenApply");
    QVERIFY(stackCombo && beforeView && afterView && zoomFit && zoom100 && zoom200 && zoom400 && zoomLbl && statusLbl && applyBtn);
    QVERIFY(stackCombo->count() >= 2);
    QVERIFY(!beforeView->image().isNull());
    QVERIFY(!afterView->image().isNull());
    QVERIFY(zoomFit->isChecked());

    // Zoom controls and left-drag pan synchronize Before and After views cleanly.
    zoom200->click();
    QVERIFY(zoom200->isChecked());
    QCOMPARE(zoomLbl->text(), QString("200%"));
    QVERIFY(std::abs(beforeView->zoom() - 2.0) < 1e-6);
    QVERIFY(std::abs(afterView->zoom() - 2.0) < 1e-6);
    zoom400->click();
    QVERIFY(zoom400->isChecked());
    QCOMPARE(zoomLbl->text(), QString("400%"));
    QVERIFY(std::abs(afterView->zoom() - 4.0) < 1e-6);
    {
        const QPointF start(beforeView->width() * 0.5, beforeView->height() * 0.5);
        const QPointF end = start + QPointF(40, -25);
        QMouseEvent press(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(beforeView, &press);
        QApplication::sendEvent(beforeView, &move);
        QApplication::sendEvent(beforeView, &release);
    }
    QVERIFY(beforeView->panOffset() != QPointF(0.0, 0.0));
    QCOMPARE(afterView->panOffset(), beforeView->panOffset());
    const QPointF savedPan = afterView->panOffset();

    auto* radiusSlider = dlg.findChild<QSlider*>("resharpenRadiusSlider");
    auto* amountSlider = dlg.findChild<QSlider*>("resharpenAmountSlider");
    auto* thresholdSlider = dlg.findChild<QSlider*>("resharpenThresholdSlider");
    auto* denoiseSlider = dlg.findChild<QSlider*>("resharpenDenoiseSlider");
    auto* gammaSlider = dlg.findChild<QSlider*>("resharpenGammaSlider");
    auto* blackSlider = dlg.findChild<QSlider*>("resharpenBlackSlider");
    auto* whiteSlider = dlg.findChild<QSlider*>("resharpenWhiteSlider");
    auto* autoStretch = dlg.findChild<QCheckBox*>("resharpenAutoStretch");
    auto* rebuildOnly = dlg.findChild<QCheckBox*>("resharpenRebuildOnly");
    auto* amountSpin = dlg.findChild<QDoubleSpinBox*>("resharpenAmount");
    auto* gammaSpin = dlg.findChild<QDoubleSpinBox*>("resharpenGamma");
    QVERIFY(radiusSlider && amountSlider && thresholdSlider && denoiseSlider
        && gammaSlider && blackSlider && whiteSlider && autoStretch && rebuildOnly && amountSpin && gammaSpin);

    // Sync slider -> spin box and verify coalesced live after-preview refreshes while keeping zoom & pan.
    const QImage afterBefore = afterView->image();
    amountSlider->setValue(260); // 2.60
    QVERIFY(std::abs(amountSpin->value() - 2.60) < 1e-4);
    QTRY_VERIFY_WITH_TIMEOUT(afterView->image() != afterBefore, 2000);
    QVERIFY(std::abs(afterView->zoom() - 4.0) < 1e-6);
    QCOMPARE(afterView->panOffset(), savedPan);
    zoomFit->click();
    QVERIFY(beforeView->isFitToWindow() && afterView->isFitToWindow());

    // Sync spin box -> slider and check enable/disable states.
    gammaSpin->setValue(1.25);
    QCOMPARE(gammaSlider->value(), 125);
    QVERIFY(!blackSlider->isEnabled() && !whiteSlider->isEnabled());
    autoStretch->setChecked(false);
    QVERIFY(blackSlider->isEnabled() && whiteSlider->isEnabled());
    autoStretch->setChecked(true);
    QVERIFY(!blackSlider->isEnabled() && !whiteSlider->isEnabled());

    rebuildOnly->setChecked(true);
    QVERIFY(!amountSlider->isEnabled() && !radiusSlider->isEnabled());
    // Rebuild video only must preview the existing sharpened/ stack, not the unsharpened stacks/ image.
    QTRY_VERIFY_WITH_TIMEOUT(afterView->image() != beforeView->image(), 2000);
    rebuildOnly->setChecked(false);
    QVERIFY(amountSlider->isEnabled() && radiusSlider->isEnabled());

    amountSpin->setValue(2.1);
    QTRY_VERIFY_WITH_TIMEOUT(amountSlider->value() == 210, 1000);
    applyBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(statusLbl->text().startsWith("Done:"), 15000);

    const QStringList reDirs = QDir(sessionDir).entryList({"sharpened_*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(reDirs.size(), 1);
    const QString reOut = QDir(sessionDir).filePath(reDirs.first());
    QVERIFY(QFileInfo::exists(QDir(reOut).filePath("timelapse.mp4")));
    QVERIFY(QDir(QDir(reOut).filePath("sharpened")).entryList({"*.fits"}, QDir::Files).size() >= 2);

    // Verify Crop-to-valid-overlap checkbox updates preview and Rebuild-video-only writes
    // into a non-destructive video_* folder without overwriting the original timelapse.mp4.
    {
        auto* cropCheck = dlg.findChild<QCheckBox*>("resharpenCropOverlap");
        auto* brightCombo = dlg.findChild<QComboBox*>("resharpenBrightnessLevel");
        QVERIFY(cropCheck && brightCombo);
        QCOMPARE(brightCombo->count(), 3);
        QCOMPARE(brightCombo->currentData().toInt(), static_cast<int>(BrightnessLevellingMode::Global));
        brightCombo->setCurrentIndex(2); // GlobalAndPatchy
        QCoreApplication::processEvents();
        brightCombo->setCurrentIndex(0); // Off
        QCoreApplication::processEvents();
        brightCombo->setCurrentIndex(1); // Global
        QCoreApplication::processEvents();

        // Save a distinct cropped overlap in session profile and reopen in dialog.
        SharpeningSettings ls;
        VideoToneSettings lv;
        QString perr;
        const QString profPath = QDir(sessionDir).filePath("sharpening_profile.json");
        QVERIFY(loadProfileJson(profPath, ls, lv, perr));
        QVERIFY(saveProfileJson(profPath, ls, lv, perr, QRect(40, 40, 200, 140)));
        dlg.openSession(sessionDir);
        QCoreApplication::processEvents();

        cropCheck->setChecked(true);
        QCoreApplication::processEvents();
        const QImage croppedImg = afterView->image();
        QCOMPARE(croppedImg.size(), QSize(200, 140));
        cropCheck->setChecked(false);
        QCoreApplication::processEvents();
        const QImage fullImg = afterView->image();
        QVERIFY(croppedImg != fullImg);
        QVERIFY(fullImg.width() > 200);
        cropCheck->setChecked(true);

        const QString origMp4 = QDir(sessionDir).filePath("timelapse.mp4");
        const QDateTime origMtime = QFileInfo(origMp4).lastModified();
        QThread::msleep(20);
        rebuildOnly->setChecked(true);
        applyBtn->click();
        QTRY_VERIFY_WITH_TIMEOUT(statusLbl->text().startsWith("Done:"), 15000);
        const QStringList vidDirs = QDir(sessionDir).entryList({"video_*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(vidDirs.size(), 1);
        QVERIFY(QFileInfo::exists(QDir(QDir(sessionDir).filePath(vidDirs.first())).filePath("timelapse.mp4")));
        QCOMPARE(QFileInfo(origMp4).lastModified(), origMtime);
    }

    window.findChild<QPushButton*>("stopButton")->click();
}

void UiTest::userManualAndStartupTipsWorkflow()
{
    const auto& tips = allStartupTips();
    QVERIFY(tips.size() >= 20);
    QCOMPARE(wrapTipIndex(-1, static_cast<int>(tips.size())), static_cast<int>(tips.size()) - 1);
    QCOMPARE(wrapTipIndex(static_cast<int>(tips.size()), static_cast<int>(tips.size())), 0);

    // Every tip's manual link must resolve cleanly inside the packaged MANUAL.md.
    ManualDialog checkManual(nullptr, QString(SELENEWATCH_SOURCE_DIR) + "/MANUAL.md");
    QCOMPARE(checkManual.loadedManualPath(), QString(SELENEWATCH_SOURCE_DIR) + "/MANUAL.md");
    auto* checkBrowser = checkManual.findChild<QTextBrowser*>("manualTextBrowser");
    QVERIFY(checkBrowser && checkBrowser->document()->toPlainText().contains("SeleneWatch User Manual"));
    for (size_t i = 0; i < tips.size(); ++i)
    {
        QVERIFY2(!tips[i].title.trimmed().isEmpty(),
            qPrintable(QString("Tip %1 missing title").arg(i + 1)));
        QVERIFY2(!tips[i].html.trimmed().isEmpty(),
            qPrintable(QString("Tip %1 missing html").arg(i + 1)));
        QVERIFY2(checkManual.hasAnchor(tips[i].manualSection),
            qPrintable(QString("Tip %1 missing MANUAL.md anchor: %2").arg(i + 1).arg(tips[i].manualSection)));
        QVERIFY(checkManual.scrollToSection(tips[i].manualSection));
        QCOMPARE(checkManual.currentSection(), tips[i].manualSection);
    }

    QTemporaryDir dir;
    MainWindow window(nullptr, dir.path());
    window.show();
    QCoreApplication::processEvents();

    // Help menu places User Manual and Tips at the same top level before Acknowledgements.
    QAction* manualAction = window.findChild<QAction*>("userManualAction");
    QAction* tipsAction = window.findChild<QAction*>("tipsAction");
    QAction* ackAction = window.findChild<QAction*>("acknowledgementsAction");
    QAction* aboutAction = window.findChild<QAction*>("aboutAction");
    QVERIFY(manualAction && tipsAction && ackAction && aboutAction);
    QVERIFY(!manualAction->shortcut().isEmpty());

    // Startup tips appear automatically while wantedAtStartup() is true, and step cleanly.
    QVERIFY(TipsDialog::wantedAtStartup());
    window.maybeShowStartupTips();
    auto* tipsDlg = window.findChild<TipsDialog*>("tipsDialog");
    QVERIFY(tipsDlg && tipsDlg->isVisible());

    auto* headingLbl = tipsDlg->findChild<QLabel*>("tipsHeading");
    auto* posLbl = tipsDlg->findChild<QLabel*>("tipsPositionLabel");
    auto* bodyLbl = tipsDlg->findChild<QLabel*>("tipsBodyLabel");
    auto* prevBtn = tipsDlg->findChild<QPushButton*>("tipsPreviousButton");
    auto* nextBtn = tipsDlg->findChild<QPushButton*>("tipsNextButton");
    auto* linkBtn = tipsDlg->findChild<QPushButton*>("tipsManualLink");
    auto* startupChk = tipsDlg->findChild<QCheckBox*>("showTipsAtStartupCheckBox");
    auto* closeBtn = tipsDlg->findChild<QPushButton*>("tipsCloseButton");
    QVERIFY(headingLbl && posLbl && bodyLbl && prevBtn && nextBtn && linkBtn && startupChk && closeBtn);
    QCOMPARE(headingLbl->text(), tips[0].title);
    QCOMPARE(posLbl->text(), QString("1 of %1").arg(tips.size()));
    QCOMPARE(QSettings().value(TipsDialog::kNextTipKey).toInt(), 1);

    nextBtn->click();
    QCOMPARE(headingLbl->text(), tips[1].title);
    QCOMPARE(posLbl->text(), QString("2 of %1").arg(tips.size()));
    QCOMPARE(QSettings().value(TipsDialog::kNextTipKey).toInt(), 2);
    prevBtn->click();
    QCOMPARE(headingLbl->text(), tips[0].title);
    QCOMPARE(posLbl->text(), QString("1 of %1").arg(tips.size()));
    prevBtn->click();
    QCOMPARE(headingLbl->text(), tips.back().title);
    QCOMPARE(posLbl->text(), QString("%1 of %1").arg(tips.size()));

    // Clicking "Read more in the manual" opens the manual scrolled to that tip's section.
    linkBtn->click();
    QCoreApplication::processEvents();
    auto* manualDlg = window.findChild<ManualDialog*>("manualDialog");
    QVERIFY(manualDlg && manualDlg->isVisible());
    QCOMPARE(manualDlg->currentSection(), tips.back().manualSection);
    manualDlg->findChild<QPushButton*>("manualTopButton")->click();
    QVERIFY(manualDlg->currentSection().isEmpty());

    // Unchecking "Show tips at startup" suppresses startup display, while Help → Tips still opens it.
    startupChk->setChecked(false);
    QVERIFY(!TipsDialog::wantedAtStartup());
    closeBtn->click();
    QVERIFY(!tipsDlg->isVisible());
    window.maybeShowStartupTips();
    QVERIFY(!tipsDlg->isVisible());
    tipsAction->trigger();
    QVERIFY(tipsDlg->isVisible());
    tipsDlg->hide();
    manualDlg->hide();
}

void UiTest::recreateFolderWhilePreviewRunningAndWheelGuard()
{
    QTemporaryDir base;
    QVERIFY(base.isValid());
    const QString outDir = QDir(base.path()).filePath("session_output");
    QVERIFY(QDir().mkpath(outDir));

    MainWindow window(nullptr, outDir);
    window.show();
    useSynthetic(window);

    // WheelGuard: wheel events on focused spinbox, inner lineEdit, slider, and combobox must NOT mutate them.
    auto* sigmaSpin = window.findChild<QDoubleSpinBox*>("detectionSigma");
    auto* sigmaSlider = window.findChild<QSlider*>("sigmaSlider");
    auto* overlayCombo = window.findChild<QComboBox*>("detectionOverlay");
    QVERIFY(sigmaSpin && sigmaSlider && overlayCombo);

    sigmaSpin->setFocus();
    const double initialSigma = sigmaSpin->value();
    const int initialSlider = sigmaSlider->value();
    const int initialOverlay = overlayCombo->currentIndex();

    const auto sendWheel = [](QWidget* target) {
        const QPointF center(target->width() * 0.5, target->height() * 0.5);
        QWheelEvent ev(center, target->mapToGlobal(center), QPoint(), QPoint(0, 120),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &ev);
    };

    sendWheel(sigmaSpin);
    if (auto* innerEdit = sigmaSpin->findChild<QLineEdit*>())
    {
        innerEdit->setFocus();
        sendWheel(innerEdit);
    }
    sigmaSlider->setFocus();
    sendWheel(sigmaSlider);
    overlayCombo->setFocus();
    sendWheel(overlayCombo);

    QCOMPARE(sigmaSpin->value(), initialSigma);
    QCOMPARE(sigmaSlider->value(), initialSlider);
    QCOMPARE(overlayCombo->currentIndex(), initialOverlay);

    // Finding 11: start Preview, delete output folder while running, verify recreateOutputFolder succeeds in place.
    window.findChild<QPushButton*>("startButton")->click();
    auto* stopBtn = window.findChild<QPushButton*>("stopButton");
    QTRY_VERIFY_WITH_TIMEOUT(stopBtn->isEnabled(), 5000);

    QVERIFY(QDir(outDir).removeRecursively());
    QVERIFY(!QDir(outDir).exists());

    // applyOutputFolder still refuses while running, whereas recreateOutputFolder succeeds in place.
    QString err;
    QVERIFY(!window.applyOutputFolder(outDir, err));
    QVERIFY(err.contains("Stop the stream"));

    err.clear();
    QVERIFY2(window.recreateOutputFolder(err), qPrintable(err));
    QVERIFY(QDir(outDir).exists());

    // Monitoring can now start cleanly without having stopped Preview.
    auto* monitorBtn = window.findChild<QPushButton*>("monitoringButton");
    monitorBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(monitorBtn->text().contains("Stop monitoring"), 5000);
    stopBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stopBtn->isEnabled(), 5000);
}

void UiTest::detectionProfilesSaveLoadAndOptionalMask()
{
    QTemporaryDir dir;
    MainWindow window(nullptr, dir.path());
    auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
    auto* combo = window.findChild<QComboBox*>("detectionProfileCombo");
    auto* deleteBtn = window.findChild<QToolButton*>("detectionDeleteProfile");
    auto* targetCombo = window.findChild<QComboBox*>("targetChoice");
    QVERIFY(panel && combo && deleteBtn && targetCombo);

    // The Moon's shadows: save and reload a named profile (Feature-31: their
    // profiles are their own, and darkening is always on for them).
    chooseWatch(window, Watch::MoonShadows);
    DetectionSettings moonCustom = panel->settings();
    moonCustom.sigma = 9.5;
    moonCustom.minPixels = 12;
    panel->setSettings(moonCustom);
    QVERIFY(panel->saveCurrentProfile("Moon Transit", false));
    QCOMPARE(combo->currentData().toString(), QString("Moon Transit"));
    QVERIFY(deleteBtn->isEnabled());

    // Mutate settings -> combo resets to "Custom / current".
    auto* sigmaSpin = window.findChild<QDoubleSpinBox*>("detectionSigma");
    sigmaSpin->setValue(5.0);
    QCOMPARE(combo->currentIndex(), 0);
    QVERIFY(!deleteBtn->isEnabled());

    // Selecting saved profile restores exact values.
    const int moonIdx = combo->findData("Moon Transit");
    QVERIFY(moonIdx > 0);
    combo->setCurrentIndex(moonIdx);
    QCOMPARE(panel->settings().sigma, 9.5);
    QCOMPARE(panel->settings().minPixels, 12);
    QVERIFY(panel->settings().darkening);

    // The flashes do not list the shadows' profiles, nor the sky.
    chooseWatch(window, Watch::MoonFlashes);
    QCOMPARE(combo->findData("Moon Transit"), -1);
    QVERIFY(!panel->settings().darkening);
    chooseWatch(window, Watch::Sky);
    QCOMPARE(panel->target(), Target::Sky);
    QCOMPARE(combo->findData("Moon Transit"), -1);

    // Draw horizon mask A, save profile WITHOUT mask (default).
    QVERIFY(panel->addMaskRect(QRect(0, 400, 640, 80)));
    const DetectionMask maskA = panel->settings().mask;
    QCOMPARE(maskA.count(), 1);
    sigmaSpin->setValue(7.5);
    QVERIFY(panel->saveCurrentProfile("Backyard No Mask", false));
    QVERIFY(!combo->currentText().contains("+mask"));

    // Change mask to mask B and change sigma, then load "Backyard No Mask" -> mask B stays untouched!
    window.findChild<QToolButton*>("clearMask")->click();
    QVERIFY(panel->addMaskRect(QRect(0, 350, 640, 130)));
    const DetectionMask maskB = panel->settings().mask;
    QVERIFY(maskB != maskA);
    sigmaSpin->setValue(11.0);

    QVERIFY(panel->applyProfile("Backyard No Mask"));
    QCOMPARE(panel->settings().sigma, 7.5);
    QCOMPARE(panel->settings().mask, maskB);

    // Now save profile WITH mask B, clear mask, and reload -> mask B is restored!
    QVERIFY(panel->saveCurrentProfile("Backyard With Mask", true));
    QVERIFY(combo->currentText().contains("+mask"));
    window.findChild<QToolButton*>("clearMask")->click();
    QVERIFY(panel->settings().mask.empty());

    QVERIFY(panel->applyProfile("Backyard With Mask"));
    QCOMPARE(panel->settings().mask, maskB);

    // Delete profile removes it cleanly.
    deleteBtn->click();
    QCOMPARE(combo->findData("Backyard With Mask"), -1);
}

void UiTest::previewZoomControlsSyncWithWidget()
{
    QTemporaryDir dir;
    MainWindow window(nullptr, dir.path());
    window.resize(1200, 800);
    window.show();
    QCoreApplication::processEvents();

    auto* preview = window.findChild<PreviewWidget*>("previewWidget");
    auto* fitBtn = window.findChild<QToolButton*>("previewZoomFit");
    auto* btn100 = window.findChild<QToolButton*>("previewZoom100");
    auto* btn200 = window.findChild<QToolButton*>("previewZoom200");
    auto* label = window.findChild<QLabel*>("previewZoomLabel");
    QVERIFY(preview && fitBtn && btn100 && btn200 && label);

    preview->setImage(QImage(640, 480, QImage::Format_Grayscale8));
    QVERIFY(fitBtn->isChecked());
    QVERIFY(!btn100->isChecked());
    QVERIFY(!btn200->isChecked());
    QVERIFY(label->text().startsWith("Fit"));

    btn100->click();
    QVERIFY(!fitBtn->isChecked());
    QVERIFY(btn100->isChecked());
    QVERIFY(!btn200->isChecked());
    QCOMPARE(label->text(), QString("100%"));
    QCOMPARE(preview->imageRect().size(), QSize(640, 480));

    btn200->click();
    QVERIFY(btn200->isChecked());
    QCOMPARE(label->text(), QString("200%"));
    QCOMPARE(preview->imageRect().size(), QSize(1280, 960));

    // Wheel zoom on preview still works (not blocked by WheelGuard) and updates the zoom label.
    const QPointF center(preview->width() * 0.5, preview->height() * 0.5);
    QWheelEvent wheel(center, preview->mapToGlobal(center), QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(preview, &wheel);
    QVERIFY(preview->zoom() > 2.2);
    QVERIFY(!btn200->isChecked());
    QVERIFY(!fitBtn->isChecked());

    fitBtn->click();
    QVERIFY(fitBtn->isChecked());
    QVERIFY(preview->isFitToWindow());
}

void UiTest::moonAndSkyEndToEndInjectionsRecordOnDisk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MainWindow window(nullptr, dir.path());
    window.resize(1280, 820);
    window.show();
    useSynthetic(window);

    // Keep clip history/post short so clips finalize promptly on disk.
    auto* historySpin = window.findChild<QDoubleSpinBox*>("historySeconds");
    auto* postSpin = window.findChild<QDoubleSpinBox*>("postSeconds");
    auto* fpsSpin = window.findChild<QSpinBox*>("targetFpsSpinBox");
    auto* startBtn = window.findChild<QPushButton*>("startButton");
    auto* monitorBtn = window.findChild<QPushButton*>("monitoringButton");
    auto* stopBtn = window.findChild<QPushButton*>("stopButton");
    auto* injectBtn = window.findChild<QPushButton*>("injectBlipButton");
    auto* kindCombo = window.findChild<QComboBox*>("blipKindComboBox");
    auto* preview = window.findChild<PreviewWidget*>("previewWidget");
    auto* panel = window.findChild<DetectionPanel*>("detectionPanel");
    auto* stateLbl = window.findChild<QLabel*>("detectionState");
    QVERIFY(historySpin && postSpin && fpsSpin && startBtn && monitorBtn && stopBtn && injectBtn && kindCombo && preview && panel && stateLbl);

    fpsSpin->setValue(30);
    historySpin->setValue(0.5);
    postSpin->setValue(0.4);

    // Helper: click a frame pixel on the preview widget.
    auto clickFramePixel = [&](int fx, int fy) {
        const QRect r = preview->imageRect();
        QVERIFY(!r.isEmpty());
        const QPoint pt(r.x() + static_cast<int>((fx + 0.5) * r.width() / 640.0),
            r.y() + static_cast<int>((fy + 0.5) * r.height() / 480.0));
        QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier, pt);
    };

    // --- 1. Moon: Preview (no clip written) -> Monitoring Bright flash, then Dark transit ---
    // Feature-31: the synthetic Moon is all sunlit; the flashes' ceiling is
    // turned off so the test flash on its disc is tested.
    window.findChild<QDoubleSpinBox*>("detectionIgnoreAbove")->setValue(0);
    startBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(stateLbl->text().contains("Noise floor"), 5000);

    // Click on lunar disk centre (320, 240) and inject a bright flash in Preview.
    clickFramePixel(320, 240);
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 2000);
    injectBtn->click();
    QTest::qWait(400);
    // Evidence rule: Preview MUST NOT write any event clip folders.
    const QDir moonEventsDir(QDir(dir.path()).filePath("events/moon"));
    QCOMPARE(moonEventsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);

    // Now start Monitoring and inject a bright flash -> clip written on disk.
    monitorBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(monitorBtn->text().contains("Stop monitoring"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(stateLbl->text().contains("Noise floor"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 3000);
    injectBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(latestWatch(QDir(dir.path()).filePath("events/moon")) + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() >= 1,
        12000);

    stopBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stopBtn->isEnabled(), 5000);
    const QString flashWatch = latestWatch(QDir(dir.path()).filePath("events/moon"));

    // Feature-31: shadows on the sunlit side are their own watch, monitored apart.
    chooseWatch(window, Watch::MoonShadows);
    QVERIFY(panel->settings().darkening);
    startBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(), 5000);
    monitorBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(monitorBtn->text().contains("Stop monitoring"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(stateLbl->text().contains("Noise floor"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(latestWatch(QDir(dir.path()).filePath("events/moon")) != flashWatch, 5000);
    const QString shadowWatch = latestWatch(QDir(dir.path()).filePath("events/moon"));
    clickFramePixel(260, 240);
    kindCombo->setCurrentIndex(kindCombo->findData(static_cast<int>(BlipKind::DarkTransit)));
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 3000);
    injectBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(shadowWatch + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() >= 1, 15000);
    stopBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stopBtn->isEnabled(), 5000);

    // Verify the Moon's bright and dark clips on disk with valid evidence & overview/GIF.
    bool sawMoonRise = false, sawMoonDark = false;
    for (const QString& watch : {flashWatch, shadowWatch})
        for (const QString& sub : QDir(watch + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        {
            const QDir clipDir(QDir(watch + "/clips").filePath(sub));
            // EventWriter writes event.txt/ser/PNGs and finally 04_event_preview.gif asynchronously.
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(clipDir.filePath("04_event_preview.gif")), 10000);
            const QString ev = text(clipDir.filePath("event.txt"));
            QVERIFY(ev.contains("contains_test_signal: yes"));
            QVERIFY(QFileInfo::exists(clipDir.filePath("event.ser")));
            QVERIFY(QFileInfo::exists(clipDir.filePath("00_overview.png")));
            if (ev.contains("kind=automatic")) sawMoonRise = true;
            if (ev.contains("kind=darkening")) sawMoonDark = true;
        }
    QVERIFY2(sawMoonRise, "Expected Moon bright flash clip on disk");
    QVERIFY2(sawMoonDark, "Expected Moon dark transit clip on disk");
    // --- 2. Sky Watch: Synthetic Sky + Horizon mask + Meteor streak + Lightning ---
    chooseWatch(window, Watch::Sky);
    QCOMPARE(panel->target(), Target::Sky);
    historySpin->setValue(0.5);
    postSpin->setValue(0.4);
    QVERIFY(panel->addMaskRect(QRect(0, 360, 640, 120)));

    startBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!preview->imageRect().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(stateLbl->text().contains("Noise floor"), 5000);
    monitorBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(monitorBtn->text().contains("Stop monitoring"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(stateLbl->text().contains("Noise floor"), 5000);

    // Flash inside the masked horizon (y = 420) must be ignored.
    kindCombo->setCurrentIndex(kindCombo->findData(static_cast<int>(BlipKind::BrightFlash)));
    clickFramePixel(320, 420);
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 3000);
    injectBtn->click();
    QTest::qWait(500);
    QCOMPARE(QDir(latestWatch(QDir(dir.path()).filePath("events/sky")) + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);

    // Moving streak (meteor) in the upper sky -> recorded on disk.
    clickFramePixel(140, 120);
    kindCombo->setCurrentIndex(kindCombo->findData(static_cast<int>(BlipKind::MovingStreak)));
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 3000);
    injectBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(latestWatch(QDir(dir.path()).filePath("events/sky")) + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() >= 1,
        12000);

    // Lightning (no pixel selection needed) -> recorded on disk.
    kindCombo->setCurrentIndex(kindCombo->findData(static_cast<int>(BlipKind::Lightning)));
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 3000);
    const int skyDirsBeforeLightning = QDir(latestWatch(QDir(dir.path()).filePath("events/sky")) + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
    injectBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(latestWatch(QDir(dir.path()).filePath("events/sky")) + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() > skyDirsBeforeLightning,
        12000);

    stopBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stopBtn->isEnabled(), 5000);

    const QStringList skyClips = QDir(latestWatch(QDir(dir.path()).filePath("events/sky")) + "/clips").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QVERIFY(skyClips.size() >= 2);
    bool sawMeteor = false, sawLightning = false;
    for (const QString& sub : skyClips)
    {
        const QDir clipDir(QDir(latestWatch(QDir(dir.path()).filePath("events/sky")) + "/clips").filePath(sub));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(clipDir.filePath("04_event_preview.gif")), 10000);
        const QString ev = text(clipDir.filePath("event.txt"));
        QVERIFY(ev.contains("mode: sky"));
        QVERIFY(ev.contains("contains_test_signal: yes"));
        QVERIFY(QFileInfo::exists(clipDir.filePath("00_overview.png")));
        if (ev.contains("kind=moving_streak") || ev.contains("suggested_kind: meteor")) sawMeteor = true;
        if (ev.contains("kind=brightening") || ev.contains("suggested_kind: lightning")) sawLightning = true;
    }
    QVERIFY2(sawMeteor, "Expected Sky meteor streak clip on disk");
    QVERIFY2(sawLightning, "Expected Sky lightning clip on disk");
    QVERIFY(text(latestWatch(eventsFolder(dir.path(), Target::Sky)) + "/candidates.txt").contains("mode=sky"));
}

void UiTest::rawSerNeedsConfirmationAndIsNotRemembered()
{
    {
        TimelapsePanel panel;
        auto* raw = panel.findChild<QCheckBox*>("timelapseSaveSer");
        QVERIFY(raw);
        QVERIFY(!raw->isChecked());

        answerMessageBox("timelapseRawSerConfirm", false);
        raw->click();
        QVERIFY(!raw->isChecked());
        QVERIFY(!panel.settings().saveRawSer);

        answerMessageBox("timelapseRawSerConfirm", true);
        raw->click();
        QVERIFY(raw->isChecked());
        QVERIFY(panel.settings().saveRawSer);
        QVERIFY(panel.findChild<QLabel*>("timelapseEstimate")->text().contains("Raw SER bursts are ON"));

        // Turning it off needs no confirmation.
        raw->click();
        QVERIFY(!raw->isChecked());
        answerMessageBox("timelapseRawSerConfirm", true);
        raw->click();
        QVERIFY(raw->isChecked());
    }
    // A new run starts with it off again, even though it was left on.
    TimelapsePanel again;
    QVERIFY(!again.findChild<QCheckBox*>("timelapseSaveSer")->isChecked());
}

void UiTest::closingDuringTimelapseWaitsForTheVideo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MainWindow window(nullptr, dir.path());
    useSynthetic(window);
    window.show();

    auto* tl = window.findChild<TimelapsePanel*>("timelapsePanel");
    QVERIFY(tl);
    tl->findChild<QDoubleSpinBox*>("timelapseInterval")->setValue(0.45);
    tl->findChild<QDoubleSpinBox*>("timelapseWindow")->setValue(0.25);
    tl->findChild<QComboBox*>("timelapseStopKind")->setCurrentIndex(0); // Manual stop
    tl->findChild<QCheckBox*>("timelapseMp4")->setChecked(true);
    tl->findChild<QCheckBox*>("timelapseGif")->setChecked(false);
    auto* startTl = tl->findChild<QPushButton*>("timelapseStartButton");
    auto* statsLabel = tl->findChild<QLabel*>("timelapseStats");
    startTl->click();
    QTRY_VERIFY_WITH_TIMEOUT(statsLabel->text().contains("Stacks: 1"), 15000);

    // Closing stops the time-lapse but keeps the window, with progress, until the video exists.
    window.close();
    QVERIFY(window.isVisible());
    auto* progress = window.findChild<QProgressDialog*>("closingTimelapseProgress");
    QVERIFY(progress);
    QVERIFY(progress->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 20000);

    const QString tlRoot = timelapsesFolder(dir.path());
    const QStringList sessions = QDir(tlRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QCOMPARE(sessions.size(), 1);
    const QString sessionDir = QDir(tlRoot).filePath(sessions.first());
    QVERIFY(QFileInfo::exists(QDir(sessionDir).filePath("timelapse.mp4")));
    QVERIFY(text(QDir(sessionDir).filePath("timelapse.txt")).contains("stop_reason: the program closed"));
}

// Feature-32, Done-when 2, 5 and 6: with the simulated mount, a session
// ends an hour after its start, stops the mount's tracking and records it;
// the first start asks what the mount does; the observer's own Stop leaves
// it tracking; a time-lapse ends the same way; the sky shows no mount choice.
void UiTest::aSessionEndsByItselfAndStopsTheMountsTracking()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    useSynthetic(window);
    auto* stop = window.findChild<QPushButton*>("stopButton");
    auto* monitoring = window.findChild<QPushButton*>("monitoringButton");
    auto* mode = window.findChild<QLabel*>("captureModeLabel");
    auto* summary = window.findChild<QLabel*>("sessionEndSummary");
    auto* warning = window.findChild<QLabel*>("sessionEndWarning");
    QCOMPARE(summary->text(), QString("Ends: when you stop it"));
    QVERIFY(!warning->isVisibleTo(&window));

    SessionEndSettings ends;
    ends.afterDuration = true;
    ends.durationMinutes = 60;
    window.applySessionEnd(ends, MountAtEnd::Unset);
    QVERIFY2(summary->text().startsWith("Ends: ") && summary->text().endsWith("1 h 00 min after the start"),
        qPrintable(summary->text()));

    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && mode->text().contains("PREVIEW"), 5000);
    window.findChild<QPushButton*>("connectMountButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(warning->isVisibleTo(&window) && warning->text().contains("choose"), 3000);
    const auto pointing = window.simulatedPointing();
    QVERIFY(pointing->tracking());

    // The first start with a mount asks. Cancel starts nothing; Stop tracking is remembered.
    answerMessageBox("mountAtEndQuestion", false);
    monitoring->click();
    QVERIFY(mode->text().contains("PREVIEW"));
    QCOMPARE(QSettings().value("sessionEnd/mount").toInt(), 0);
    answerMessageBox("mountAtEndQuestion", true);
    monitoring->click();
    QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);
    QCOMPARE(QSettings().value("sessionEnd/mount").toInt(), 2);
    QVERIFY(!warning->isVisibleTo(&window));
    QVERIFY2(summary->text().startsWith("Ends in ") && summary->text().contains("then the mount stops tracking"),
        qPrintable(summary->text()));

    // An hour later: Monitoring and the stream end, then the mount's tracking.
    window.checkSessionEnd(std::chrono::system_clock::now() + std::chrono::minutes{61});
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!pointing->tracking(), 5000);
    const QString watch = latestWatch(eventsFolder(directory.path(), Target::Moon));
    QTRY_VERIFY_WITH_TIMEOUT(text(watch + "/session.txt").contains(
        " mount_at_end action=stop_tracking result=\"tracking stopped\""), 5000);
    QVERIFY2(text(watch + "/session.txt").contains("reason=session end: 1 h 00 min after the start"),
        qPrintable(text(watch + "/session.txt")));
    QTRY_VERIFY2_WITH_TIMEOUT(text(watch + "/report.html").contains("At the end, the mount: tracking stopped"),
        qPrintable(text(watch + "/report.html").section("<summary>Monitoring and recording timeline", 1).left(800)), 15000);
    // The status line keeps why it ended and what the mount did, also once the report is ready.
    auto* message = window.findChild<QLabel*>("statusMessage");
    QTRY_VERIFY2_WITH_TIMEOUT(message->text().contains("the session ended (1 h 00 min after the start). The mount: tracking stopped."),
        qPrintable(message->text()), 5000);

    // The observer's own Stop leaves the mount tracking, and nothing is asked again.
    pointing->setTracking(true);
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && mode->text().contains("PREVIEW"), 5000);
    monitoring->click();
    QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);
    stop->click();
    QTest::qWait(300);
    QVERIFY(pointing->tracking());

    // A time-lapse ends the same way, its video built and its record kept.
    auto* tl = window.findChild<TimelapsePanel*>();
    tl->findChild<QDoubleSpinBox*>("timelapseInterval")->setValue(0.45);
    tl->findChild<QDoubleSpinBox*>("timelapseWindow")->setValue(0.25);
    tl->findChild<QComboBox*>("timelapseStopKind")->setCurrentIndex(0);
    tl->findChild<QSpinBox*>("timelapseVideoFps")->setValue(5);
    auto* startTl = tl->findChild<QPushButton*>("timelapseStartButton");
    startTl->click();
    QTRY_VERIFY_WITH_TIMEOUT(startTl->text().contains("Stop"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(tl->findChild<QLabel*>("timelapseStats")->text().contains("Stacks: 1"), 15000);
    window.checkSessionEnd(std::chrono::system_clock::now() + std::chrono::minutes{61});
    QTRY_VERIFY_WITH_TIMEOUT(!pointing->tracking(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(startTl->text().contains("Start") && !stop->isEnabled(), 20000);
    const QString tlRoot = timelapsesFolder(directory.path());
    const QStringList runs = QDir(tlRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QCOMPARE(runs.size(), 1);
    const QString record = QDir(tlRoot).filePath(runs.first() + "/timelapse.txt");
    QTRY_VERIFY_WITH_TIMEOUT(text(record).contains("mount_at_end: stop_tracking, tracking stopped"), 20000);
    QVERIFY2(text(record).contains("stop_reason: session end: 1 h 00 min after the start"), qPrintable(text(record)));

    // The sky is watched with a fixed camera: no mount choice there.
    chooseWatch(*window.findChild<DetectionPanel*>(), Watch::Sky);
    window.checkSessionEnd(std::chrono::system_clock::now());
    QVERIFY(!summary->text().contains("mount"));
    QVERIFY(!warning->isVisibleTo(&window));
}

// Feature-32, Done-when 4: a clip refused for the disk reserve ends Monitoring.
void UiTest::theDiskReserveEndsMonitoring()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    useSynthetic(window);
    auto* stop = window.findChild<QPushButton*>("stopButton");
    auto* mode = window.findChild<QLabel*>("captureModeLabel");
    // More than any drive has: Monitoring starts with a warning, and the first clip is refused.
    window.findChild<QDoubleSpinBox*>("diskReserveGiB")->setValue(100000);
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && mode->text().contains("PREVIEW"), 5000);
    window.findChild<QPushButton*>("monitoringButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);
    auto* saveNow = window.findChild<QPushButton*>("saveNowButton");
    QTRY_VERIFY_WITH_TIMEOUT(saveNow->isEnabled(), 3000);
    saveNow->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 30000);
    const QString watch = latestWatch(eventsFolder(directory.path(), Target::Moon));
    QTRY_VERIFY_WITH_TIMEOUT(text(watch + "/session.txt").contains("reason=session end: the disk reserve"), 5000);
}

// Feature-32, Done-when 3: guiding that stops by itself ends a guided session
// at once; the observer's own stop of guiding does not. Calibrates the
// simulated mount, which takes most of a minute.
void UiTest::lostGuidingEndsTheSession()
{
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    useSynthetic(window);
    window.applySessionEnd({}, MountAtEnd::KeepTracking);
    auto* stop = window.findChild<QPushButton*>("stopButton");
    auto* mode = window.findChild<QLabel*>("captureModeLabel");
    auto* preview = window.findChild<PreviewWidget*>();
    auto* state = window.findChild<QLabel*>("lockStateLabel");
    auto* guide = window.findChild<QPushButton*>("guideButton");
    window.findChild<QPushButton*>("startButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && !preview->imageRect().isEmpty(), 5000);
    window.findChild<QPushButton*>("connectMountButton")->click();
    QTest::mouseClick(preview, Qt::LeftButton, Qt::NoModifier, preview->imageRect().center());
    window.findChild<QPushButton*>("lockAtSelectionButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(state->text().contains("Locked"), 5000);
    auto* calibrate = window.findChild<QPushButton*>("calibrateButton");
    QTRY_VERIFY_WITH_TIMEOUT(calibrate->isEnabled(), 3000);
    calibrate->click();
    auto* calibration = window.findChild<QLabel*>("calibrationLabel");
    QTRY_VERIFY2_WITH_TIMEOUT(calibration->text().startsWith("West "), qPrintable(calibration->text()), 120000);
    QTRY_VERIFY_WITH_TIMEOUT(guide->isEnabled(), 3000);
    guide->click();
    QTRY_VERIFY_WITH_TIMEOUT(guide->text() == "Stop guiding", 3000);
    window.findChild<QPushButton*>("monitoringButton")->click();
    QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);

    // The observer stops guiding and starts it again: the session goes on.
    guide->click();
    QTRY_VERIFY_WITH_TIMEOUT(guide->text() == "Start guiding", 3000);
    QTest::qWait(1500);
    QVERIFY(mode->text().contains("MONITORING"));
    guide->click();
    QTRY_VERIFY_WITH_TIMEOUT(guide->text() == "Stop guiding", 3000);

    // The mount stops tracking (a limit in its driver): guiding stops, and the session ends.
    window.simulatedPointing()->setTracking(false);
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 5000);
    const QString watch = latestWatch(eventsFolder(directory.path(), Target::Moon));
    QTRY_VERIFY_WITH_TIMEOUT(text(watch + "/session.txt").contains(
        " mount_at_end action=keep_tracking result=\"kept tracking, as chosen\""), 5000);
    QVERIFY2(text(watch + "/session.txt").contains("reason=session end: guiding stopped:"), qPrintable(text(watch + "/session.txt")));
}

// Feature-32: the dialog shows each condition's time and the first one; the
// Moon's row only for the Moon, the mount's only with a mount; a mount that
// cannot stop its tracking cannot be told to. An old Watch from dusk to dawn
// becomes At dawn.
void UiTest::theEndsDialogShowsWhenEachConditionIsMet()
{
    SessionEndSettings settings;
    settings.atLocalTime = true;
    settings.localTime = QTime(4, 30);
    SessionEndDialog::Context context;
    context.zone = QTimeZone(2 * 3600);
    context.moon = false;
    {
        SessionEndDialog dialog(settings, MountAtEnd::Unset, context);
        QVERIFY(!dialog.findChild<QCheckBox*>("sessionEndMoonBelow")->isVisibleTo(&dialog));
        QVERIFY(!dialog.findChild<QGroupBox*>("sessionEndMount")->isVisibleTo(&dialog));
        QCOMPARE(dialog.findChild<QLabel*>("sessionEndTimeReading")->text(), QString("= 02:30 UTC"));
        QVERIFY(dialog.findChild<QLabel*>("sessionEndDawnReading")->text().contains("needs the site's position"));
        QVERIFY2(dialog.findChild<QLabel*>("sessionEndResult")->text().startsWith("→ Ends at 04:30 local (02:30 UTC): the local time"),
            qPrintable(dialog.findChild<QLabel*>("sessionEndResult")->text()));
        dialog.findChild<QCheckBox*>("sessionEndAtTime")->setChecked(false);
        QCOMPARE(dialog.findChild<QLabel*>("sessionEndResult")->text(), QString("→ Runs until you stop it."));
        dialog.findChild<QCheckBox*>("sessionEndAfter")->setChecked(true);
        dialog.findChild<QTimeEdit*>("sessionEndDuration")->setTime(QTime(2, 15));
        QCOMPARE(dialog.settings().durationMinutes, 135);
        QVERIFY(dialog.settings().afterDuration && !dialog.settings().atLocalTime);
        QCOMPARE(dialog.mountAtEnd(), MountAtEnd::Unset);
    }
    context.moon = true;
    context.mountConnected = true;
    context.canSetTracking = false;
    context.site.latitudeDeg = 37.98;
    context.site.longitudeDeg = 23.73;
    {
        SessionEndDialog dialog(settings, MountAtEnd::StopTracking, context);
        QVERIFY(dialog.findChild<QCheckBox*>("sessionEndMoonBelow")->isVisibleTo(&dialog));
        QVERIFY(dialog.findChild<QGroupBox*>("sessionEndMount")->isVisibleTo(&dialog));
        QVERIFY(dialog.findChild<QLabel*>("sessionEndDawnReading")->text().startsWith("dawn: "));
        QVERIFY(!dialog.findChild<QRadioButton*>("mountStopsTracking")->isEnabled());
        QVERIFY(dialog.findChild<QRadioButton*>("mountKeepsTracking")->isChecked());
        QVERIFY(dialog.findChild<QLabel*>("mountKeepWarning")->isVisibleTo(&dialog));
        QCOMPARE(dialog.mountAtEnd(), MountAtEnd::KeepTracking);
    }

    // Feature-15's Watch from dusk to dawn becomes At dawn, once.
    QSettings saved;
    saved.setValue("sky/night/enabled", true);
    saved.setValue("sky/night/sunLimit", -12.0);
    QTemporaryDir directory;
    MainWindow window(nullptr, directory.path());
    QVERIFY(saved.value("sessionEnd/atDawn").toBool());
    QCOMPARE(saved.value("sessionEnd/sunLimit").toDouble(), -12.0);
    QVERIFY(!saved.contains("sky/night/enabled"));
    QVERIFY(!window.findChild<QCheckBox*>("nightSchedule"));
}

void UiTest::cameraRoiDrawingPresetsAndAutoRounding()
{
    // 1. Pure alignment rules across all four camera families.
    {
        // ZWO USB2 ASI120: width % 8 == 0, height % 2 == 0, (width * height) % 1024 == 0.
        const RoiConstraints asi120{8, 2, 1, 1, 8, 2, 1024};
        const AlignedRoi drawn = alignRoi(1280, 960, asi120, 340, 255, 600, 450, true);
        QVERIFY(drawn.adjusted);
        QCOMPARE(drawn.width % 8, 0);
        QCOMPARE(drawn.height % 2, 0);
        QCOMPARE((static_cast<int64_t>(drawn.width) * drawn.height) % 1024, int64_t{0});
        QCOMPARE(drawn.width, 608);
        QCOMPARE(drawn.height, 448);
        // Box center stays aligned (340 + 300 = 640 -> 336 + 304 = 640; 255 + 225 = 480 -> 256 + 224 = 480).
        QCOMPARE(drawn.x, 336);
        QCOMPARE(drawn.y, 256);

        const AlignedRoi nudged = alignRoi(1280, 960, asi120, 100, 80, 640, 482, false);
        QVERIFY(nudged.adjusted);
        QCOMPARE(nudged.width, 640);
        QCOMPARE(nudged.height, 480);
        QCOMPARE(nudged.x, 100);
        QCOMPARE(nudged.y, 80);

        // Player One: width % 4 == 0, height % 2 == 0.
        const RoiConstraints poa{4, 2, 1, 1, 4, 2, 1};
        const AlignedRoi p = alignRoi(1920, 1080, poa, 15, 21, 501, 301, false);
        QVERIFY(p.adjusted);
        QCOMPARE(p.width % 4, 0);
        QCOMPARE(p.height % 2, 0);
        QCOMPARE(p.x, 15);
        QCOMPARE(p.y, 21);

        // ToupTek: even origin and even size >= 8.
        const RoiConstraints tt{2, 2, 2, 2, 8, 8, 1};
        const AlignedRoi t = alignRoi(1600, 1200, tt, 11, 17, 5, 7, false);
        QVERIFY(t.adjusted);
        QCOMPARE(t.width, 8);
        QCOMPARE(t.height, 8);
        QCOMPARE(t.x % 2, 0);
        QCOMPARE(t.y % 2, 0);

        // QHY: 1x1 step, clamped inside sensor bounds.
        const RoiConstraints qhy{1, 1, 1, 1, 1, 1, 1};
        const AlignedRoi q = alignRoi(640, 480, qhy, 600, 450, 100, 100, false);
        QVERIFY(q.adjusted);
        QCOMPARE(q.width, 100);
        QCOMPARE(q.height, 100);
        QCOMPARE(q.x, 540);
        QCOMPARE(q.y, 380);
    }

    // 2. Shared editor interactive auto-rounding and ToupTek even origin steps.
    {
        CameraSettingsEditor editor(RoiConstraints{2, 2, 2, 2, 8, 8, 1}, nullptr);
        editor.showGeometry(640, 480, {PixelFormat::Mono8}, {1}, {640, 480, 0, 0, 1, PixelFormat::Mono8});
        auto* xBox = editor.findChild<QSpinBox*>("cameraX");
        auto* yBox = editor.findChild<QSpinBox*>("cameraY");
        auto* wBox = editor.findChild<QSpinBox*>("cameraWidth");
        auto* hBox = editor.findChild<QSpinBox*>("cameraHeight");
        QCOMPARE(xBox->singleStep(), 2);
        QCOMPARE(yBox->singleStep(), 2);
        QSignalSpy adjustedSpy(&editor, &CameraSettingsEditor::roiAdjusted);
        wBox->setValue(301);
        hBox->setValue(201);
        xBox->setValue(13);
        yBox->setValue(19);
        emit wBox->editingFinished();
        QCOMPARE(adjustedSpy.count(), 1);
        const CameraGeometry g = editor.requestedGeometry();
        QCOMPARE(g.width % 2, 0);
        QCOMPARE(g.height % 2, 0);
        QCOMPARE(g.x % 2, 0);
        QCOMPARE(g.y % 2, 0);
        QVERIFY(g.width >= 8 && g.height >= 8);
    }

    // 3. Full MainWindow workflow with USB2 ASI120 (1280x960):
    //    Draw ROI (including nested sub-ROI), centered presets, Center, Full, and mutual exclusion.
    QTemporaryDir dir;
    auto api = std::make_shared<FakeZwoApi>();
    api->camera.width = 1280;
    api->camera.height = 960;
    api->camera.usb3 = false; // USB2 ASI120 -> 1024-pixel multiple rule active
    api->settings.width = 1280;
    api->settings.height = 960;
    MainWindow window(nullptr, dir.path(), api);
    window.resize(1120, 760);
    window.show();
    chooseSource(window, "zwo");

    auto* panel = window.findChild<CameraPanel*>();
    QVERIFY(panel && panel->hasCamera());
    auto* drawRoi = panel->findChild<QToolButton*>("cameraDrawRoi");
    auto* centerRoi = panel->findChild<QToolButton*>("cameraRoiCenter");
    auto* fullRoi = panel->findChild<QToolButton*>("cameraRoiFull");
    auto* presetBox = panel->findChild<QComboBox*>("cameraRoiPreset");
    auto* preview = window.findChild<PreviewWidget*>();
    auto* start = window.findChild<QPushButton*>("startButton");
    auto* stop = window.findChild<QPushButton*>("stopButton");
    QVERIFY(drawRoi && centerRoi && fullRoi && presetBox && preview && start && stop);

    // Draw ROI is disabled while stopped; presets fill spin boxes without opening the camera.
    QVERIFY(!drawRoi->isEnabled());
    QVERIFY(presetBox->count() > 3);
    const int opensBeforePreset = api->opened.load();
    // Pick "50% (640 × 480)" while stopped.
    int preset50 = -1;
    for (int i = 1; i < presetBox->count(); ++i)
        if (presetBox->itemText(i).contains("50% (")) { preset50 = i; break; }
    QVERIFY(preset50 > 0);
    emit presetBox->activated(preset50);
    QCOMPARE(presetBox->currentIndex(), 0);
    QCOMPARE(api->opened.load(), opensBeforePreset);
    QCOMPARE(panel->controls()->requestedGeometry().width, 640);
    QCOMPARE(panel->controls()->requestedGeometry().height, 480);
    QCOMPARE(panel->controls()->requestedGeometry().x, 320);
    QCOMPARE(panel->controls()->requestedGeometry().y, 240);

    // Reset to Full while stopped and start Preview.
    fullRoi->click();
    QCOMPARE(panel->controls()->requestedGeometry().width, 1280);
    QCOMPARE(panel->controls()->requestedGeometry().height, 960);
    start->click();
    QTRY_VERIFY_WITH_TIMEOUT(api->delivered >= 2 && !preview->imageRect().isEmpty(), 3000);
    QVERIFY(drawRoi->isEnabled());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Pan);

    // Draw ROI switches Preview to Region mode and turns off Draw lock box (mutual exclusion).
    auto* drawLock = window.findChild<QPushButton*>("drawLockBox");
    QVERIFY(drawLock);
    drawLock->click();
    QVERIFY(drawLock->isChecked());
    drawRoi->click();
    QVERIFY(drawRoi->isChecked());
    QVERIFY(!drawLock->isChecked());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Region);

    // Drag an unaligned 600x450 box at (340, 255): auto-rounds to 608x448 at (336, 256)
    // and immediately restarts Preview with that ROI.
    const int startsBeforeDraw = api->starts.load();
    emit preview->regionSelected(QRect(340, 255, 600, 450));
    QVERIFY(!drawRoi->isChecked());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Pan);
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(), startsBeforeDraw + 1, 3000);
    QCOMPARE(panel->profile().current.width, 608);
    QCOMPARE(panel->profile().current.height, 448);
    QCOMPARE(panel->profile().current.x, 336);
    QCOMPARE(panel->profile().current.y, 256);
    QCOMPARE((static_cast<int64_t>(panel->profile().current.width) * panel->profile().current.height) % 1024, int64_t{0});

    // Nested sub-ROI drag inside the already-cropped 608x448 frame:
    // dragging (100, 50, 256, 256) in preview pixels must land at sensor origin (336+100, 256+50) = (436, 306).
    drawRoi->click();
    QVERIFY(drawRoi->isChecked());
    emit preview->regionSelected(QRect(100, 50, 256, 256));
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(), startsBeforeDraw + 2, 3000);
    QCOMPARE(panel->profile().current.width, 256);
    QCOMPARE(panel->profile().current.height, 256);
    QCOMPARE(panel->profile().current.x, 436);
    QCOMPARE(panel->profile().current.y, 306);

    // Center button centers the current 256x256 ROI on the 1280x960 sensor -> (512, 352) immediately.
    centerRoi->click();
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(), startsBeforeDraw + 3, 3000);
    QCOMPARE(panel->profile().current.width, 256);
    QCOMPARE(panel->profile().current.height, 256);
    QCOMPARE(panel->profile().current.x, 512);
    QCOMPARE(panel->profile().current.y, 352);

    // Full button immediately returns to full sensor 1280x960 at (0, 0).
    fullRoi->click();
    QTRY_COMPARE_WITH_TIMEOUT(api->starts.load(), startsBeforeDraw + 4, 3000);
    QCOMPARE(panel->profile().current.width, 1280);
    QCOMPARE(panel->profile().current.height, 960);
    QCOMPARE(panel->profile().current.x, 0);
    QCOMPARE(panel->profile().current.y, 0);

    // Clicking Full again while already at full sensor does not needlessly restart Preview.
    fullRoi->click();
    QTest::qWait(100);
    QCOMPARE(api->starts.load(), startsBeforeDraw + 4);

    // Stopping Preview disables Draw ROI and returns preview to Pan mode.
    drawRoi->click();
    QVERIFY(drawRoi->isChecked());
    stop->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stop->isEnabled(), 3000);
    QVERIFY(!drawRoi->isChecked());
    QVERIFY(!drawRoi->isEnabled());
    QCOMPARE(preview->dragMode(), PreviewWidget::DragMode::Pan);
}

void UiTest::guiLayoutThemesFocusViewAndPersistence()
{
    QTemporaryDir dir;
    auto api = std::make_shared<FakeZwoApi>();
    {
        MainWindow window(nullptr, dir.path(), api);
        window.resize(1280, 760);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // Option B: all docks live in the single right-hand sidebar (no left or bottom dock).
        auto* cameraDock = window.findChild<QDockWidget*>("cameraDock");
        auto* detectionDock = window.findChild<QDockWidget*>("detectionDock");
        auto* histogramDock = window.findChild<QDockWidget*>("histogramDock");
        QVERIFY(cameraDock && detectionDock && histogramDock);
        QCOMPARE(window.dockWidgetArea(cameraDock), Qt::RightDockWidgetArea);
        QCOMPARE(window.dockWidgetArea(detectionDock), Qt::RightDockWidgetArea);
        QCOMPARE(window.dockWidgetArea(histogramDock), Qt::RightDockWidgetArea);

        // Item 2: source-irrelevant controls hide for a live camera, show for Recording.
        chooseSource(window, "zwo");
        auto* openFile = window.findChild<QPushButton*>("openFileButton");
        auto* connectStartup = window.findChild<QCheckBox*>("connectAtStartup");
        auto* playbackBar = window.findChild<QWidget*>("playbackControls");
        auto* startBtn = window.findChild<QPushButton*>("startButton");
        auto* stopBtn = window.findChild<QPushButton*>("stopButton");
        auto* monitorBtn = window.findChild<QPushButton*>("monitoringButton");
        auto* saveNowBtn = window.findChild<QPushButton*>("saveNowButton");
        QVERIFY(openFile && connectStartup && playbackBar && startBtn && stopBtn && monitorBtn && saveNowBtn);
        QVERIFY(!openFile->isVisibleTo(&window));
        QVERIFY(!playbackBar->isVisibleTo(&window));
        QVERIFY(connectStartup->isVisibleTo(&window));

        // Item 6: streamlined Start / Stop / Monitor / Save now visibility by state.
        QVERIFY(startBtn->isVisibleTo(&window));
        QVERIFY(!stopBtn->isVisibleTo(&window));
        QVERIFY(!monitorBtn->isVisibleTo(&window));
        QVERIFY(!saveNowBtn->isVisibleTo(&window));
        QCOMPARE(window.findChild<QLabel*>("captureModeLabel")->text(), QString("STOPPED"));

        // Item 2: smart Apply highlight when ROI/format has unapplied edits.
        auto* panel = window.findChild<CameraPanel*>();
        auto* applyBtn = panel->findChild<QPushButton*>("applyCamera");
        QVERIFY(!panel->controls()->geometryDirty());
        QVERIFY(applyBtn->styleSheet().isEmpty());
        panel->findChild<QSpinBox*>("cameraWidth")->setValue(64);
        QVERIFY(panel->controls()->geometryDirty());
        QVERIFY(!applyBtn->styleSheet().isEmpty());

        startBtn->click();
        QTRY_VERIFY_WITH_TIMEOUT(api->delivered >= 2, 3000);
        QVERIFY(!startBtn->isVisibleTo(&window));
        QVERIFY(stopBtn->isVisibleTo(&window));
        QVERIFY(monitorBtn->isVisibleTo(&window));
        QVERIFY(!saveNowBtn->isVisibleTo(&window));
        QVERIFY(window.findChild<QLabel*>("captureModeLabel")->text().contains("PREVIEW"));

        monitorBtn->click();
        QTRY_VERIFY_WITH_TIMEOUT(saveNowBtn->isEnabled(), 3000);
        QVERIFY(saveNowBtn->isVisibleTo(&window));
        QVERIFY(window.findChild<QLabel*>("captureModeLabel")->text().contains("MONITORING"));

        stopBtn->click();
        QTRY_VERIFY_WITH_TIMEOUT(startBtn->isEnabled(), 3000);
        QVERIFY(startBtn->isVisibleTo(&window));
        QVERIFY(!stopBtn->isVisibleTo(&window));

        // Recording source reveals Open... and playback controls, hides Connect at startup.
        chooseSource(window, "recording");
        QVERIFY(openFile->isVisibleTo(&window));
        QVERIFY(playbackBar->isVisibleTo(&window));
        QVERIFY(!connectStartup->isVisibleTo(&window));
        chooseSource(window, "zwo");

        // Focus view hides all open docks for maximum preview width, then restores them.
        auto* histToggle = window.findChild<QToolButton*>("previewHistogramToggle");
        auto* focusBtn = window.findChild<QToolButton*>("previewFocusView");
        QVERIFY(histToggle && focusBtn);
        histToggle->click();
        QApplication::processEvents();
        QVERIFY(!histogramDock->isHidden());
        // Verify the 3-slot right sidebar stacks Camera above Detection and Histogram below Detection (not crushed into one tab bar).
        QVERIFY(cameraDock->y() < detectionDock->y());
        QVERIFY(detectionDock->y() < histogramDock->y());
        QVERIFY(!window.tabifiedDockWidgets(detectionDock).contains(cameraDock));
        QVERIFY(!window.tabifiedDockWidgets(detectionDock).contains(histogramDock));
        const int previewWidthWithPanels = window.findChild<PreviewWidget*>()->width();
        focusBtn->click();
        QApplication::processEvents();
        QVERIFY(cameraDock->isHidden());
        QVERIFY(detectionDock->isHidden());
        QVERIFY(histogramDock->isHidden());
        QVERIFY(window.findChild<PreviewWidget*>()->width() > previewWidthWithPanels + 150);
        focusBtn->click();
        QApplication::processEvents();
        QVERIFY(!cameraDock->isHidden());
        QVERIFY(!detectionDock->isHidden());
        QVERIFY(!histogramDock->isHidden());

        // Item 1: Theme switching (Dark -> Red night vision -> System) persists via QSettings.
        auto* nightRed = window.findChild<QAction*>("themeNightRedAction");
        auto* darkAct = window.findChild<QAction*>("themeDarkAction");
        auto* sysAct = window.findChild<QAction*>("themeSystemAction");
        QVERIFY(nightRed && darkAct && sysAct);
        nightRed->trigger();
        QCOMPARE(currentUiTheme(), UiTheme::NightRed);
        QCOMPARE(loadUiTheme(), UiTheme::NightRed);

        // Item 4: closing saves geometry and dock state; Reset layout restores defaults.
        window.resize(1320, 780);
        window.close();
    }

    // Reopen: theme and window layout persist, then Reset window layout restores default state.
    {
        MainWindow reopened(nullptr, dir.path(), api);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        QCOMPARE(currentUiTheme(), UiTheme::NightRed);
        QVERIFY(reopened.findChild<QAction*>("themeNightRedAction")->isChecked());
        QVERIFY(!QSettings().value("ui/windowState").toByteArray().isEmpty());

        reopened.findChild<QAction*>("resetLayoutAction")->trigger();
        QVERIFY(QSettings().value("ui/windowState").toByteArray().isEmpty());
        QCOMPARE(reopened.dockWidgetArea(reopened.findChild<QDockWidget*>("detectionDock")), Qt::RightDockWidgetArea);
        QCOMPARE(reopened.dockWidgetArea(reopened.findChild<QDockWidget*>("histogramDock")), Qt::RightDockWidgetArea);
        QVERIFY(reopened.findChild<QDockWidget*>("histogramDock")->isHidden());

        // Restore Dark theme for any subsequent tests.
        reopened.findChild<QAction*>("themeDarkAction")->trigger();
        QCOMPARE(currentUiTheme(), UiTheme::Dark);
    }
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("SeleneWatchTests");
    QCoreApplication::setApplicationName("Feature9-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const int font = QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
    if (font >= 0) app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(), 9));
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/seguisym.ttf");
    UiTest tests;
    const int result = QTest::qExec(&tests, argc, argv);
    QSettings saved; saved.clear(); saved.sync();
    return result;
}
#include "ui_tests.moc"
