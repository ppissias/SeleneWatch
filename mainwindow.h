#pragma once

#include "event_writer.h"
#include "frame.h"
#include "frame_source.h"
#include "session.h"
#include "ui_mainwindow.h"

#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>

#include <chrono>
#include <cstdint>
#include <memory>

class QTimer;
class RecordingSource;
class SyntheticFrameSource;

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
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onStartClicked();
    void onStopClicked();
    void onOpenFileClicked();
    void onSourceKindChanged();
    void onTargetFpsChanged(int fps);
    void onSpeedChanged();
    void onInjectFlashClicked();
    void onInjectBlipClicked();

private:
    void setupStatusBar();

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

    [[nodiscard]] bool recordingSelected() const;

    Ui::MainWindowClass ui;
    std::unique_ptr<Session> session_;

    // The current source, where it has controls of its own. Owned by the session.
    SyntheticFrameSource* synthetic_{nullptr};
    RecordingSource* recording_{nullptr};

    QString recordingPath_;

    QLabel* statusLabel_{nullptr};
    QLabel* fpsLabel_{nullptr};
    QLabel* framesLabel_{nullptr};
    QLabel* candidatesLabel_{nullptr};
    QLabel* bufferLabel_{nullptr};
    QLabel* frameLabel_{nullptr};
    QTimer* bufferReadoutTimer_{nullptr};

    uint64_t lastAcquiredSample_{0};
    std::chrono::steady_clock::time_point lastFpsSampleTime_{};

    int candidates_{0};
    int clipsNotSaved_{0};
    bool sourceFailed_{false};
    bool playbackEnded_{false};

    // The session's record is broken. Shown until the stream is restarted.
    bool recordFailed_{false};
};
