#pragma once

#include "event_capture.h"
#include "event_writer.h"
#include "frame.h"
#include "latest_frame_slot.h"
#include "synthetic_frame_source.h"
#include "ui_mainwindow.h"

#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QLabel>

#include <atomic>
#include <chrono>
#include <memory>

class QTimer;

// The window: preview, controls, and status.
//
// It also wires the frame path together. onFrameArrived runs on the source
// thread: it publishes the frame, offers it to the preview, and runs the
// capture. Finished clips go from the capture straight to the writer. The
// window is only told about candidates and write results; nothing on the
// frame path waits for it.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onStartClicked();
    void onStopClicked();
    void onTargetFpsChanged(int fps);
    void onInjectFlashClicked();

private:
    void setupStatusBar();
    void updateBufferReadout();
    void updateStreamReadout();
    void updateCandidateReadout();
    void updateStatusLabel();

    // Source thread.
    void onFrameArrived(std::shared_ptr<Frame> frame);

    // GUI thread, queued from the source and writer threads.
    void showLatestFrame();
    void onCandidateStarted();
    void onClipWritten(const ClipWriteResult& result);
    void onSourceFailed(const QString& message);

    void displayImage(const QImage& image);

    Ui::MainWindowClass ui;
    std::unique_ptr<EventWriter> eventWriter_;
    std::unique_ptr<EventCapture> capture_;
    std::unique_ptr<SyntheticFrameSource> frameSource_;

    QLabel* statusLabel_{nullptr};
    QLabel* fpsLabel_{nullptr};
    QLabel* framesLabel_{nullptr};
    QLabel* candidatesLabel_{nullptr};
    QLabel* bufferLabel_{nullptr};
    QLabel* resolutionLabel_{nullptr};
    QTimer* bufferReadoutTimer_{nullptr};

    // Acquisition count. The source thread increments it; the UI timer reads it.
    std::atomic<uint64_t> acquiredFrames_{0};
    uint64_t lastAcquiredSample_{0};
    std::chrono::steady_clock::time_point lastFpsSampleTime_{};

    // Newest frame for the preview. The source thread puts, the GUI thread takes.
    LatestFrameSlot previewSlot_;

    // GUI thread only.
    int candidates_{0};
    int clipsNotSaved_{0};
    bool sourceFailed_{false};

    // The session's record is broken. Shown until the stream is restarted.
    bool recordFailed_{false};
};
