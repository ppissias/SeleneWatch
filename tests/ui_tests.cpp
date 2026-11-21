#include "detection_panel.h"
#include "display_panel.h"
#include "camera_panel.h"
#include "camera_settings_editor.h"
#include "elided_label.h"
#include "guiding_panel.h"
#include "tests/fake_zwo_api.h"
#include "tests/fake_qhy_api.h"
#include "tests/fake_playerone_api.h"
#include "tests/fake_touptek_api.h"
#include "qhy_camera_panel.h"
#include "playerone_camera_panel.h"
#include "touptek_camera_panel.h"
#include "camera_support_dialog.h"
#include "runtime_chooser.h"
#include "mainwindow.h"
#include "manual_dialog.h"
#include "preview_widget.h"
#include "site_dialog.h"
#include "startup_tips.h"
#include "tips_dialog.h"
#include "target_defaults.h"
#include "release_checker.h"
#include "recording/output_paths.h"
#include "recording/ser_reader.h"
#include "recording/ser_writer.h"
#include "sources/synthetic_scene.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
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
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextBrowser>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QWhatsThis>
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
#include "resharpen_dialog.h"
#include "timelapse/timelapse_settings.h"
#include "timelapse_panel.h"

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
    void diskReserveStartsAtTenGiBAndIsRemembered();
    void everyControlFitsOnASmallLaptopScreen();
    void darkeningIsForTheMoonOnly();
    void everyControlHasHelp();
    void cameraSlidersSetTheBoxAndReachTheCameraOnce();
    void siteAndRecordingControls();
    void nightScheduleAndExposureFollower();
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
        QTRY_VERIFY_WITH_TIMEOUT(text(eventsFolder(dir.path(), Target::Moon) + "/candidates.txt").contains(" testing_tools=\"on\" "), 3000);
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
        for (const auto& folder:QDir(moonDir).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
            complete+=text(moonDir+"/"+folder+"/event.txt").contains("complete: yes");
        return complete>=2;
    })(),5000);
    int before=0,after=0;
    for (const auto& folder:QDir(moonDir).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
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
    QVERIFY(!QFile::exists(eventsFolder(dir.path(), Target::Moon)+"/candidates.txt"));
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
    const QString moonDir=eventsFolder(dir.path(),Target::Moon);
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        int complete=0;
        for (const auto& folder:QDir(moonDir).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
            complete+=text(moonDir+"/"+folder+"/event.txt").contains("complete: yes");
        return complete>=2;
    })(),5000);
    int old=0,changed=0;
    for (const auto& folder:QDir(moonDir).entryList(QDir::Dirs|QDir::NoDotAndDotDot))
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
    const QString moonOutput=eventsFolder(output,Target::Moon);
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
    QCOMPARE(panel->target(), Target::Moon);
    QVERIFY(choice->isEnabled());
    QVERIFY(guiding->isEnabled());
    QCOMPARE(panel->settings().brighteningShare, 0.0);
    QCOMPARE(display->captureSettings().diskReserveBytes, uint64_t{10} * 1024 * 1024 * 1024);
    sigma->setValue(7);
    // Feature-13: darkening is off by default, and saved per target like the rest.
    auto* darkening = panel->findChild<QCheckBox*>("darkeningTrigger");
    QVERIFY(!darkening->isChecked());
    darkening->setChecked(true);
    QVERIFY(panel->settings().darkening);

    choice->setCurrentIndex(1);
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
    // The Moon's settings were saved under their old keys on the way out.
    QCOMPARE(QSettings().value("detection/sigma").toDouble(), 7.0);
    QCOMPARE(QSettings().value("target").toInt(), 1);

    // A mask drawn on the sky, and a Sky K.
    panel->findChild<QToolButton*>("drawMask")->setChecked(true);
    QVERIFY(panel->addMaskRect(QRect(0, 400, 640, 80)));
    QCOMPARE(panel->settings().mask.count(), 1);
    sigma->setValue(9);

    choice->setCurrentIndex(0);
    QCOMPARE(panel->settings().sigma, 7.0);
    QVERIFY(panel->settings().darkening);
    QCOMPARE(QSettings().value("detection/darkening").toBool(), true);
    QCOMPARE(panel->settings().maxPixels, 400);
    QCOMPARE(panel->settings().brighteningShare, 0.0);
    QVERIFY(panel->settings().mask.empty());
    QVERIFY(!panel->drawingMask());
    QCOMPARE(display->captureSettings().target, Target::Moon);
    QCOMPARE(display->captureSettings().maxPostSeconds, 10.0);
    QVERIFY(guiding->isEnabled());
    QCOMPARE(QSettings().value("sky/detection/sigma").toDouble(), 9.0);
    QCOMPARE(QSettings().value("sky/detection/mask").toString(), QString("0,400 640,400 640,480 0,480"));

    choice->setCurrentIndex(1);
    QCOMPARE(panel->settings().sigma, 9.0);
    QCOMPARE(panel->settings().mask.count(), 1);
    QVERIFY(panel->settings().mask.shapes()[0] == DetectionMask::rectangle(0, 400, 640, 80));
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
        auto* choice = window.findChild<DetectionPanel*>()->findChild<QComboBox*>("targetChoice");
        QCOMPARE(reserve->value(), 10.0);
        reserve->setValue(25);
        QCOMPARE(display->captureSettings().diskReserveBytes, 25 * kGiB);

        choice->setCurrentIndex(1);
        QCOMPARE(reserve->value(), 10.0);
        reserve->setValue(0);
        QCOMPARE(reserve->text(), QString("None"));
        QCOMPARE(display->captureSettings().diskReserveBytes, uint64_t{0});
    }
    MainWindow window(nullptr, directory.path());
    auto* display = window.findChild<DisplayPanel*>();
    auto* choice = window.findChild<DetectionPanel*>()->findChild<QComboBox*>("targetChoice");
    QCOMPARE(display->captureSettings().target, Target::Sky);
    QCOMPARE(display->captureSettings().diskReserveBytes, uint64_t{0});
    choice->setCurrentIndex(0);
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
    // either target: it can be resized in width, not only in height.
    for (const Target target : {Target::Moon, Target::Sky})
    {
        window.findChild<DetectionPanel*>()->targetRequested(target);
        window.resize(400, 300);
        QApplication::processEvents();
        if (window.width() > 1150)
            problems << QString("%1: the window cannot be narrower than %2 px")
                .arg(target == Target::Sky ? "Sky" : "Moon").arg(window.width());
    }
    problems.removeDuplicates();
    for (const QString& problem : problems) qWarning().noquote() << problem;
    QVERIFY2(problems.isEmpty(), qPrintable(QString("%1 problems").arg(problems.size())));
}

// Feature-22: darkening is for silhouettes against the sunlit Moon. In Sky
// mode its controls are hidden and it is off, even when an older version
// saved it on for the sky.
void UiTest::darkeningIsForTheMoonOnly()
{
    DetectionPanel panel;
    auto* darkening = panel.findChild<QCheckBox*>("darkeningTrigger");
    auto* share = panel.findChild<QDoubleSpinBox*>("darkeningShare");
    panel.setTarget(Target::Moon);
    QVERIFY(darkening->isVisibleTo(&panel) && share->isVisibleTo(&panel));
    darkening->setChecked(true);
    QVERIFY(panel.settings().darkening);

    panel.setTarget(Target::Sky);
    DetectionSettings saved = defaultDetectionSettings(Target::Sky);
    saved.darkening = true;
    saved.darkeningShare = 0.05;
    panel.setSettings(saved);
    QVERIFY(!darkening->isVisibleTo(&panel) && !share->isVisibleTo(&panel));
    QVERIFY(!panel.settings().darkening);
    QCOMPARE(panel.settings().darkeningShare, 0.0);

    panel.setTarget(Target::Moon);
    QVERIFY(darkening->isVisibleTo(&panel));
    QVERIFY(panel.settings().darkening);
    QCOMPARE(panel.settings().darkeningShare, 0.05);
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
    for (const int target : {0, 1})
    {
        window.findChild<QComboBox*>("targetChoice")->setCurrentIndex(target);
        for (QWidget* widget : window.findChildren<QWidget*>())
        {
            if (widget->window() != &window || widget->objectName().startsWith("qt_")) continue;
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
}

// Feature-25: a ranged camera control has a slider beside its box, and the
// exposure's is logarithmic. The slider sets the box, the box moves the
// slider, and in Preview a drag reaches the camera once, after the pause.
void UiTest::cameraSlidersSetTheBoxAndReachTheCameraOnce()
{
    CameraSettingsEditor editor(2, 2, nullptr);
    editor.addControl(1, "Gain", 0, 600, 1, 100, 1, "");
    editor.addControl(2, "Exposure", 32, 2'000'000'000, 1, 10'000, 1000, " ms");
    editor.addControl(3, "High speed mode", 0, 1, 1, 0, 1, "");
    editor.addControl(4, "Temperature", -500, 500, 1, 200, 1, "", false);
    auto* gain = editor.findChild<QSlider*>("cameraControlSlider1");
    auto* exposure = editor.findChild<QSlider*>("cameraControlSlider2");
    QVERIFY(gain && exposure);
    QVERIFY(!editor.findChild<QSlider*>("cameraControlSlider3"));
    QVERIFY(!editor.findChild<QSlider*>("cameraControlSlider4"));
    QVERIFY(!gain->toolTip().isEmpty());

    // Gain: one slider position per step.
    QCOMPARE(gain->maximum(), 600);
    QCOMPARE(gain->value(), 100);
    gain->setValue(250);
    QCOMPARE(*editor.controlValue(1), 250.0);
    QVERIFY(editor.setControlValue(1, 400));
    QCOMPARE(gain->value(), 400);

    // Exposure: logarithmic, from the camera's minimum to its maximum; the
    // middle position is the geometric middle, 0.25 s, not 1000 s.
    exposure->setValue(0);
    QCOMPARE(*editor.controlValue(2), 32.0);
    exposure->setValue(exposure->maximum());
    QCOMPARE(*editor.controlValue(2), 2e9);
    exposure->setValue(exposure->maximum() / 2);
    QVERIFY(*editor.controlValue(2) > 2e5 && *editor.controlValue(2) < 3e5);

    editor.setLiveEditing(true);
    QSignalSpy requested(&editor, &CameraSettingsEditor::liveControlsRequested);
    for (int value = 300; value <= 340; value += 4) gain->setValue(value);
    QTRY_COMPARE(requested.count(), 1);
    QTest::qWait(400);
    QCOMPARE(requested.count(), 1);
    QCOMPARE(*editor.controlValue(1), 340.0);
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
        QTRY_COMPARE_WITH_TIMEOUT(record->text(), QString("Stop recording"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel*>("captureModeLabel")->text().contains("RECORDING"), 3000);
        record->click();
        QTRY_COMPARE_WITH_TIMEOUT(record->text(), QString("Record"), 3000);
        window.findChild<QPushButton*>("stopButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!record->isEnabled(), 3000);
    };
    recordInMonitoring();
    QCOMPARE(QDir(recordingsFolder(directory.path())).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), qsizetype{2});

    // A chosen output folder routes recordings, events/moon, and events/sky underneath it.
    QTemporaryDir elsewhere;
    const QString chosen = QDir(elsewhere.path()).filePath("long nights");
    QVERIFY(window.applyOutputFolder(chosen, error));
    recordInMonitoring();
    const QStringList made = QDir(recordingsFolder(chosen)).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(made.size(), qsizetype{1});
    QVERIFY(QFile::exists(QDir(QDir(recordingsFolder(chosen)).filePath(made.front())).filePath("recording.ser")));
    const QString moonLog = eventsFolder(chosen, Target::Moon) + "/candidates.txt";
    QTRY_VERIFY_WITH_TIMEOUT(text(moonLog).contains(" recording_start name=" + made.front() + " "), 3000);
    QVERIFY(text(moonLog).contains("folder=\"" + QDir::toNativeSeparators(QDir::cleanPath(recordingsFolder(chosen))) + "\""));
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

// Feature-15, Done-when 1 and 2 in the window. The site is put where it is
// local midnight now, then where it is noon, so the test runs at any hour.
void UiTest::nightScheduleAndExposureFollower()
{
    {
        QTemporaryDir directory;
        MainWindow window(nullptr, directory.path());
        useSynthetic(window);
        auto* panel = window.findChild<DetectionPanel*>();
        auto* stop = window.findChild<QPushButton*>("stopButton");
        auto* reading = panel->findChild<QLabel*>("nightReading");
        panel->findChild<QComboBox*>("targetChoice")->setCurrentIndex(1);
        const double utcHour = std::fmod(std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count() / 3600.0, 24.0);
        const auto wrap = [](double longitude) { return std::fmod(longitude + 540.0, 360.0) - 180.0; };
        ObservingSite night;
        night.latitudeDeg = 0; night.heightM = 0;
        night.longitudeDeg = wrap(-15 * utcHour);
        ObservingSite day = night;
        day.longitudeDeg = wrap(15 * (12 - utcHour));

        // The schedule off: nothing starts.
        window.applySite(night, false, "pool.ntp.org");
        window.checkNightSchedule();
        QVERIFY(!stop->isEnabled());
        SkyExtras extras;
        extras.night = true;
        extras.sunLimitDeg = -10;
        panel->setSkyExtras(extras);
        // No position: nothing starts, and the panel says why.
        window.applySite(ObservingSite{}, false, "pool.ntp.org");
        window.checkNightSchedule();
        QVERIFY(!stop->isEnabled());
        QVERIFY(reading->text().contains("position"));
        auto* mode = window.findChild<QLabel*>("captureModeLabel");
        // Night: the stream and Monitoring start.
        window.applySite(night, false, "pool.ntp.org");
        window.checkNightSchedule();
        QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);
        QVERIFY2(reading->text().contains("Dawn stop at"), qPrintable(reading->text()));
        QVERIFY2(reading->text().contains("local time (") && reading->text().contains(" UTC)."), qPrintable(reading->text()));
        // The observer stops in the night: the schedule leaves it stopped.
        stop->click();
        window.checkNightSchedule();
        QVERIFY(!stop->isEnabled());
        // Day, then the next night: it starts again, and stops at dawn.
        window.applySite(day, false, "pool.ntp.org");
        window.checkNightSchedule();
        QVERIFY2(reading->text().contains("Dusk start at"), qPrintable(reading->text()));
        window.applySite(night, false, "pool.ntp.org");
        window.checkNightSchedule();
        QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);
        window.applySite(day, false, "pool.ntp.org");
        window.checkNightSchedule();
        QVERIFY(!stop->isEnabled());
        // A stream the observer started: at night the schedule adds
        // Monitoring, and at dawn it ends only that.
        window.findChild<QPushButton*>("startButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(stop->isEnabled() && mode->text().contains("PREVIEW"), 5000);
        window.applySite(night, false, "pool.ntp.org");
        window.checkNightSchedule();
        QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("MONITORING"), 5000);
        window.applySite(day, false, "pool.ntp.org");
        window.checkNightSchedule();
        QTRY_VERIFY_WITH_TIMEOUT(mode->text().contains("PREVIEW"), 5000);
        QVERIFY(stop->isEnabled());
        window.checkNightSchedule();
        QVERIFY(stop->isEnabled());
        stop->click();
    }
    {
        // The fake camera's sky is 42% of full scale: too bright.
        QTemporaryDir directory;
        auto api = std::make_shared<FakeZwoApi>();
        MainWindow window(nullptr, directory.path(), api);
        window.show();
        auto* panel = window.findChild<DetectionPanel*>();
        auto* camera = window.findChild<CameraPanel*>();
        auto* mode = window.findChild<QLabel*>("captureModeLabel");
        panel->findChild<QComboBox*>("targetChoice")->setCurrentIndex(1);
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
        QTRY_VERIFY_WITH_TIMEOUT(text(eventsFolder(directory.path(), Target::Sky) + "/candidates.txt").contains(" exposure_change exposure_ms=1 to 0.5 "), 3000);
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
    panel->findChild<QComboBox*>("targetChoice")->setCurrentIndex(1);
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
    panel->findChild<QComboBox*>("targetChoice")->setCurrentIndex(0);
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
        const QString moonOut = eventsFolder(output.path(), Target::Moon);
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
            QDir events(eventsFolder(QDir(output.path()).filePath("events"), Target::Moon));
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

    // Switching stop condition to UTC end when stale must refresh UTC end into the future.
    auto* utcEndEdit = tl->findChild<QDateTimeEdit*>("timelapseUtcEnd");
    auto* stopKindCombo = tl->findChild<QComboBox*>("timelapseStopKind");
    QVERIFY(utcEndEdit && stopKindCombo);
    utcEndEdit->setDateTime(QDateTime::currentDateTimeUtc().addSecs(-3600));
    stopKindCombo->setCurrentIndex(2); // StopConditionKind::UtcEnd
    QVERIFY(utcEndEdit->dateTime() > QDateTime::currentDateTimeUtc());
    stopKindCombo->setCurrentIndex(0); // Restore Manual stop

    ResharpenDialog dlg(tlRoot, sessionDir, &window);
    dlg.show();
    QCoreApplication::processEvents();
    auto* stackCombo = dlg.findChild<QComboBox*>("resharpenStackCombo");
    auto* beforeLbl = dlg.findChild<QLabel*>("resharpenBefore");
    auto* afterLbl = dlg.findChild<QLabel*>("resharpenAfter");
    auto* statusLbl = dlg.findChild<QLabel*>("resharpenStatus");
    auto* applyBtn = dlg.findChild<QPushButton*>("resharpenApply");
    QVERIFY(stackCombo && beforeLbl && afterLbl && statusLbl && applyBtn);
    QVERIFY(stackCombo->count() >= 2);
    QVERIFY(!beforeLbl->pixmap().isNull());
    QVERIFY(!afterLbl->pixmap().isNull());

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

    // Sync slider -> spin box and verify coalesced live after-preview refreshes.
    const QImage afterBefore = afterLbl->pixmap().toImage();
    amountSlider->setValue(260); // 2.60
    QVERIFY(std::abs(amountSpin->value() - 2.60) < 1e-4);
    QTRY_VERIFY_WITH_TIMEOUT(afterLbl->pixmap().toImage() != afterBefore, 2000);

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
    QTRY_VERIFY_WITH_TIMEOUT(afterLbl->pixmap().toImage() != beforeLbl->pixmap().toImage(), 2000);
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
        auto* brightCheck = dlg.findChild<QCheckBox*>("resharpenBrightnessLevel");
        QVERIFY(cropCheck && brightCheck);

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
        const QImage croppedImg = afterLbl->pixmap().toImage();
        cropCheck->setChecked(false);
        QCoreApplication::processEvents();
        const QImage fullImg = afterLbl->pixmap().toImage();
        QVERIFY(croppedImg != fullImg);
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

    // Moon mode: save and reload a named profile.
    DetectionSettings moonCustom = panel->settings();
    moonCustom.sigma = 9.5;
    moonCustom.minPixels = 12;
    moonCustom.darkening = true;
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

    // Switch to Sky mode: Moon profiles do not pollute Sky profiles.
    targetCombo->setCurrentIndex(1);
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

    // --- 1. Moon: Preview (no clip written) -> Monitoring Bright flash + Dark transit ---
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
        QDir(QDir(dir.path()).filePath("events/moon")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() >= 1,
        12000);

    // Enable darkening and inject a Dark transit across the sunlit Moon.
    auto* darkChk = window.findChild<QCheckBox*>("darkeningTrigger");
    QVERIFY(darkChk);
    darkChk->setChecked(true);
    clickFramePixel(260, 240);
    kindCombo->setCurrentIndex(kindCombo->findData(static_cast<int>(BlipKind::DarkTransit)));
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 3000);
    const int moonDirsBeforeDark = QDir(QDir(dir.path()).filePath("events/moon")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
    injectBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(dir.path()).filePath("events/moon")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() > moonDirsBeforeDark,
        12000);
    stopBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stopBtn->isEnabled(), 5000);

    // Verify both Moon bright and dark clips exist on disk with valid evidence & overview/GIF.
    const QStringList moonClips = QDir(QDir(dir.path()).filePath("events/moon")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QVERIFY(moonClips.size() >= 2);
    bool sawMoonRise = false, sawMoonDark = false;
    for (const QString& sub : moonClips)
    {
        const QDir clipDir(QDir(QDir(dir.path()).filePath("events/moon")).filePath(sub));
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
    auto* targetCombo = window.findChild<QComboBox*>("targetChoice");
    targetCombo->setCurrentIndex(1); // The sky
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
    QCOMPARE(QDir(QDir(dir.path()).filePath("events/sky")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 0);

    // Moving streak (meteor) in the upper sky -> recorded on disk.
    clickFramePixel(140, 120);
    kindCombo->setCurrentIndex(kindCombo->findData(static_cast<int>(BlipKind::MovingStreak)));
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 3000);
    injectBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(dir.path()).filePath("events/sky")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() >= 1,
        12000);

    // Lightning (no pixel selection needed) -> recorded on disk.
    kindCombo->setCurrentIndex(kindCombo->findData(static_cast<int>(BlipKind::Lightning)));
    QTRY_VERIFY_WITH_TIMEOUT(injectBtn->isEnabled(), 3000);
    const int skyDirsBeforeLightning = QDir(QDir(dir.path()).filePath("events/sky")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
    injectBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        QDir(QDir(dir.path()).filePath("events/sky")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() > skyDirsBeforeLightning,
        12000);

    stopBtn->click();
    QTRY_VERIFY_WITH_TIMEOUT(!stopBtn->isEnabled(), 5000);

    const QStringList skyClips = QDir(QDir(dir.path()).filePath("events/sky")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QVERIFY(skyClips.size() >= 2);
    bool sawMeteor = false, sawLightning = false;
    for (const QString& sub : skyClips)
    {
        const QDir clipDir(QDir(QDir(dir.path()).filePath("events/sky")).filePath(sub));
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
    QVERIFY(text(QDir(dir.path()).filePath("events/sky/candidates.txt")).contains("mode=sky"));
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
