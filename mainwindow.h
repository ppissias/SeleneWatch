#pragma once

#include "recording/event_writer.h"
#include "frame.h"
#include "sky_exposure.h"
#include "sources/frame_source.h"
#include "session.h"

#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>

class QAction;
class QCheckBox;
class QDialog;
class QCloseEvent;
class QProgressDialog;
class QMenu;
class QPushButton;
class QSpinBox;
class QTimer;
class QToolButton;
class WheelGuard;
class RecordingReader;
class RecordingSource;
class SyntheticFrameSource;
class AutoTunePanel;
class DetectionPanel;
class DisplayPanel;
class HistogramPanel;
class GuidingPanel;
class TimelapsePanel;
class ResharpenDialog;
class ManualDialog;
class TipsDialog;
class ReleaseChecker;
class SimulatedPointing;
class CameraFamilyPanel;
class ZwoApi;
class QhyApi;
class PlayerOneApi;
class ToupTekApi;
class QDockWidget;
namespace Ui { class MainWindowClass; }

// The window: source selection, preview, controls, and status.
//
// The frame path lives in the Session. The window makes the selected source
// at each Start, hands it to the Session, and is told about frames,
// candidates, written clips, and the end of a recording. Nothing on the
// frame path waits for the window.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Empty outputRoot writes beside the executable. Tests supply an isolated
    // directory and test cameras; a camera family without one uses its SDK,
    // when the build has it.
    explicit MainWindow(QWidget *parent = nullptr, QString outputRoot = {}, std::shared_ptr<ZwoApi> cameraApi = {},
        std::shared_ptr<QhyApi> qhyApi = {}, std::shared_ptr<PlayerOneApi> playerOneApi = {},
        std::shared_ptr<ToupTekApi> toupTekApi = {});
    ~MainWindow() override;
    // Selects a file while stopped; Start opens it. Shared by the file dialog
    // and the window test, which supplies an isolated recording.
    void selectRecording(QString path);

private slots:
    void onStartClicked();
    void onStopClicked();
    void onOpenFileClicked();
    void onSourceKindChanged();
    void onTargetFpsChanged(int fps);
    void onSpeedChanged();
    void onInjectFlashClicked();
    void onInjectBlipClicked();
    void onSaveNowClicked();
    void onApplyCamera();
    void onApplyLiveCamera();
    void onMonitoringClicked();

private:
    void setupStatusBar();
    void setupPreviewZoomBar();
    void updateZoomControls(double effectiveZoom, bool fitToWindow);
    void setupBlipControls();
    void onBlipKindChanged();
    [[nodiscard]] BlipKind selectedBlipKind() const;
    // The status bar's message line. A message stays until the next one,
    // or for timeoutMs when that is above zero.
    void showMessage(const QString& text, int timeoutMs = 0);
    void startStream(Session::Mode mode);
    // Feature-14: the Session menu (site and instrument, a scheduled
    // recording), the Record button, and the saved site and clock check.
    void setupSessionMenu();
    QWidget* reportOffer_{nullptr};
    void setupHelpMenu();
    void showAcknowledgements();
    void onSiteAndInstrument();
    void onCameraSupport();
    void onScheduleRecording();
    void onOutputFolder();
    bool ensureOutputFolderReady(const QString& actionName);
    void checkLegacyEventsNotice();
    void onRecordClicked();
    void onRecordingChanged();
    // The recording's line under the capture mode, or none.
    [[nodiscard]] QString recordingReadout() const;
    [[nodiscard]] static ObservingSite savedSite();
    static void saveSite(const ObservingSite& site, bool ntp, const QString& server);
public:
    // Saves the site and the clock check and hands them to the session; the
    // dialog's OK, and the window test.
    void applySite(const ObservingSite& site, bool ntp, const QString& server);
    // Saves the single remembered output folder (empty: the default) after
    // checking that it can be written; the dialog's OK, and the window test.
    bool applyOutputFolder(const QString& folder, QString& error);
    // Recreates the current output folder in place (safe even while Preview
    // is running because the configured output root does not change).
    bool recreateOutputFolder(QString& error);
    // Help → About: the release, the source revision and the licenses.
    [[nodiscard]] static QString aboutText();
    // Help → Acknowledgements: ACKNOWLEDGEMENTS.md, built into the program.
    [[nodiscard]] static QString acknowledgementsMarkdown();
    // Help → User Manual & Help → Tips (and non-modal startup tips).
    void showManual(const QString& section = {});
    void showTips();
    void maybeShowStartupTips();
    // Once at normal startup; asynchronous and independent of acquisition.
    void checkForUpdates();
    // View → Testing tools: the synthetic source and the test flash. Off by
    // default and remembered; changes only while stopped. The menu, and the
    // window tests.
    void setTestingTools(bool on);
private:
    void setupDetectionPanel();
    void updateDetectionDisplay();
    // Feature-11: each target's detection and capture settings are saved
    // apart. The Moon keeps the keys used before the choice existed.
    [[nodiscard]] static DetectionSettings savedDetectionSettings(Target target);
    [[nodiscard]] static CaptureSettings savedCaptureSettings(Target target);
    // Save the panels' settings under the target they belong to.
    void saveDetectionSettings();
    void saveCaptureSettings();
    void onTargetRequested(Target target);
    // Guiding, the preview's mask, and the drag on the preview follow the target.
    void applyTarget();
    // Feature-28: what the next drag on the preview draws. Only one at a time.
    enum class Drawing { LockBox, EarthshineBox, Anchor, Mask };
    void chooseDrawing(Drawing drawing);
public:
    // Feature-15: closes the mask shape whose corners were clicked; the
    // preview's double-click, and the window test.
    void closeMaskShape();
    // Feature-15, every 30 s and on a change: starts or stops watching by
    // the Sun's altitude. Public for the window test.
    void checkNightSchedule();
    // Feature-15, every 5 s: measures the sky and changes the camera's
    // exposure or gain when the rule says so. Public so the window test can
    // give the time.
    void checkSkyExposure(std::chrono::steady_clock::time_point now);
    // Feature-26: the earthshine box, as a drag on the preview would set it.
    // Public for the window test.
    void setEarthshineBox(QRect box);
private:
    void saveSkyExtras();
    // Feature-26, in mainwindow_autotune.cpp. The panel holds the suggestions
    // and the K results; the window runs the measurements through the
    // session, applies what the observer accepts, and logs it.
    void setupAutoTune();
    void updateAutoTune();
    void onAutoTuneChanged();
    void onCameraReadbackForAutoTune();
    void onDetectionSettingsChangedForAutoTune();
    void startCameraMeasurement(bool check);
    void applyAutoTuneCamera();
    void startKWatch(bool higher, std::optional<double> onlyK = {});
    void applyAutoTuneK();
    void startFlashCheck();
    void updateEarthshineArea();
    [[nodiscard]] bool autoTuneMeasuring() const;
    [[nodiscard]] bool autoTuneShown() const;
    [[nodiscard]] TuneWorkflow autoTuneWorkflow() const;
    [[nodiscard]] TuneArea autoTuneArea() const;
    [[nodiscard]] TuneCamera tuneCamera() const;
    [[nodiscard]] QString twilightAdvice() const;
    [[nodiscard]] std::vector<std::string> guidingWarnings() const;
    void setupDisplayPanel();
    void setupHistogramPanel();
    void setupGuidingPanel();
    void setupTimelapsePanel();
    void onStartTimeLapse();
    void onStopTimeLapse();
    void onTimeLapseFinished(const TimelapseFinishResult& result);
    // Closing while a time-lapse runs or builds its video: the window stays
    // open with a progress dialog until the video is built (or abandoned),
    // then closes itself.
    void closeEvent(QCloseEvent* event) override;
    void showClosingProgress();
    void updateClosingProgress(const TimelapseStatus& status);
    void onOpenResharpen(const QString& sessionDir = {});
    void onLockChanged();
    void onLockAtSelection();
    void onConnectMount();
    void onChooseMountDriver();
    void sendTolerance();
    void saveGuidingSettings();
    void saveDisplaySettings();
    void renderPreview();
    void updateHistogram(bool force = false);

    // The source the controls describe. Null, with a message, when none can be made.
    std::unique_ptr<FrameSource> makeSource();

    // Stops the session after Stop, the end of a recording, or a failure.
    void finishStream();

    void updateControls();
    void updateBufferReadout();
    void updateStreamReadout();
    void updateCandidateReadout();
    void updateStatusLabel();
    void updateFrameLabel(const Frame& frame);
    void updateBlipReadout();

    // Session notifications, on the GUI thread.
    void showLatestFrame();
    void onCandidateStarted();
    void onClipWritten(const ClipWriteResult& result);
    void onPlaybackEnded();
    void onSourceFailed(const QString& message);

    // The chosen recording is opened off this thread, since an AVI file
    // without an index is walked chunk by chunk. Its reader is kept for
    // every run of that file at that frame rate.
    void prepareRecording();
    // Takes a finished read, waiting up to wait for it. False when none finished.
    bool takePreparedRecording(std::chrono::milliseconds wait);
    [[nodiscard]] bool recordingReady() const;

    [[nodiscard]] bool recordingSelected() const;
    // The Source list: "Choose a source…" (nothing chosen, as at start), the
    // synthetic source with the testing tools on, the recording file, and the
    // camera families. Rebuilt when the testing tools change; keeps the
    // choice when it is still listed.
    void setupSourceChoice();
    void arrangeTopControls();
    void applyTestingTools(bool on);
    void rebuildSourceList();
    // Feature-16: a camera family's Source entry, dock and connections.
    void addCameraFamily(CameraFamilyPanel* panel, const QString& dockTitle, const QString& dockName);
    // The camera family chosen under Source, or null.
    [[nodiscard]] CameraFamilyPanel* selectedCamera() const;
    // The camera family that made the session's source, or null.
    [[nodiscard]] CameraFamilyPanel* activeCamera() const;
    [[nodiscard]] bool validateCameraSettings();

    std::unique_ptr<Ui::MainWindowClass> ui_;
    std::unique_ptr<Session> session_;
    ReleaseChecker* releaseChecker_{nullptr};
    DetectionPanel* detectionPanel_{nullptr};
    QDockWidget* detectionDock_{nullptr};
    // What is watched. The detection and display panels hold its settings.
    Target target_{Target::Moon};
    // The corners of a mask shape being drawn (Feature-15).
    std::vector<MaskPoint> pendingShape_;
    // Feature-15: the timer of the night schedule and the exposure follower,
    // and the follower's memory. The schedule's own record: whether it has
    // started tonight, and what it started that still runs (the stream, or
    // only Monitoring on the observer's stream). The observer's Stop ends
    // what it owns.
    QTimer* skyTimer_{nullptr};
    int skyTicks_{0};
    enum class NightOwns { Nothing, Monitoring, Stream };
    NightOwns nightOwns_{NightOwns::Nothing};
    bool nightStartedTonight_{false};
    SkyExposureState skyExposureState_;
    // Feature-26: auto-tune's panel, the earthshine box and the area grown
    // from it, and the measurement in hand. A K result is bound to the
    // detection and camera settings it was measured with.
    AutoTunePanel* autoTunePanel_{nullptr};
    // Feature-28: auto-tune has its own window, opened from the Detection dock.
    QDialog* autoTuneWindow_{nullptr};
    std::optional<QRect> earthshineBox_;
    EarthshineArea earthshineArea_;
    bool drawingEarthshineBox_{false};
    enum class TunePhase { Idle, MeasuringCamera, WaitingForCamera, CheckingCamera, WatchingK, CheckingFlash };
    TunePhase tunePhase_{TunePhase::Idle};
    uint64_t tuneWatchId_{0};
    TuneCamera tuneMeasuredCamera_;
    std::optional<TuneMeasurement> tuneLast_, tunePrevious_;
    bool tuneHasK_{false};
    DetectionSettings tuneBoundDetection_;
    TuneCamera tuneBoundCamera_;
    QTimer* tuneReadbackTimer_{nullptr};
    DetectionSummary latestDetection_;
    DisplayPanel* displayPanel_{nullptr};
    QDockWidget* displayDock_{nullptr};
    HistogramPanel* histogramPanel_{nullptr};
    QDockWidget* histogramDock_{nullptr};
    GuidingPanel* guidingPanel_{nullptr};
    QDockWidget* guidingDock_{nullptr};
    TimelapsePanel* timelapsePanel_{nullptr};
    QDockWidget* timelapseDock_{nullptr};
    QProgressDialog* closingProgress_{nullptr};
    bool closeAfterTimeLapse_{false};
    double measuredFps_{30.0};
    int lastFrameWidth_{640};
    int lastFrameHeight_{480};
    int lastFrameBpp_{1};
    QLabel* lockLabel_{nullptr};
    QTimer* guidingTimer_{nullptr};
    // Moves the synthetic Moon for the simulated mount. Shared with the
    // synthetic source and the simulated mount on the guide thread.
    std::shared_ptr<SimulatedPointing> simulatedPointing_;
    QString ascomDriver_;
    int frameShortSide_{0};
    FramePtr latestFrame_;
    std::chrono::steady_clock::time_point lastHistogramTime_{};
    // Feature-16: each camera family's panel and dock, in the Source list's
    // order. The panels hold their SDK boundaries.
    struct CameraFamily
    {
        CameraFamilyPanel* panel;
        QDockWidget* dock;
    };
    std::vector<CameraFamily> cameraFamilies_;
    // The camera family chosen at startup ("Connect at startup"), or none.
    QString startupCamera_;
    QCheckBox* connectAtStartup_{nullptr};
    // The testing tools, the View menu's switch and the badge beside Source.
    bool testingTools_{false};
    QMenu* viewMenu_{nullptr};
    QAction* testingAction_{nullptr};
    QLabel* testingBadge_{nullptr};

    // The current source, where it has controls of its own. Owned by the session.
    SyntheticFrameSource* synthetic_{nullptr};
    RecordingSource* recording_{nullptr};

    QString recordingPath_;
    // A read of the chosen recording, its result, and a start waiting for it.
    struct PreparedRecording
    {
        QString path;
        int fps{0};
        std::shared_ptr<RecordingReader> reader;
        QString error;
        QString summary;
    };
    std::future<PreparedRecording> preparing_;
    bool prepareAgain_{false};
    QTimer* prepareTimer_{nullptr};
    std::shared_ptr<RecordingReader> preparedReader_;
    QString preparedPath_;
    int preparedFps_{0};
    QString preparedError_;
    std::optional<Session::Mode> startWhenRead_;

    // The status bar: a line for messages, with the readouts below it, so
    // neither takes the other's width.
    QLabel* messageLabel_{nullptr};
    QTimer* messageTimer_{nullptr};
    QLabel* statusLabel_{nullptr};
    QLabel* fpsLabel_{nullptr};
    QLabel* framesLabel_{nullptr};
    QLabel* candidatesLabel_{nullptr};
    QLabel* bufferLabel_{nullptr};
    QLabel* frameLabel_{nullptr};
    QLabel* modeLabel_{nullptr};
    QTimer* bufferReadoutTimer_{nullptr};
    QPushButton* recordButton_{nullptr};
    QSpinBox* recordFramesSpinBox_{nullptr};
    bool customOutputRoot_{false};

    uint64_t lastAcquiredSample_{0};
    std::chrono::steady_clock::time_point lastFpsSampleTime_{};

    int candidates_{0};
    int manualCaptures_{0};
    int clipsNotSaved_{0};
    int clipsLimited_{0};
    bool sourceFailed_{false};
    bool playbackEnded_{false};

    // The session's record is broken. Shown until the stream is restarted.
    bool recordFailed_{false};

    ManualDialog* manualDialog_{nullptr};
    TipsDialog* tipsDialog_{nullptr};
    WheelGuard* wheelGuard_{nullptr};
    QToolButton* zoomFitButton_{nullptr};
    QToolButton* zoom100Button_{nullptr};
    QToolButton* zoom200Button_{nullptr};
    QLabel* zoomReadoutLabel_{nullptr};
};
