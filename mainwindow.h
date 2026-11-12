#pragma once

#include "event_capture.h"
#include "event_writer.h"
#include "frame.h"
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
    void onEventWritten(const QString& path, const QString& error);

private:
    void setupStatusBar();
    void updateBufferReadout();
    void updateStreamReadout();
    void onFrameArrived(Frame& frame);
    void displayImage(QImage image);
    void publishPreview(const Frame& frame);
    void onCandidateStarted();
    void onClipReady(EventClip clip);
    void onSourceFailed(const QString& message);

    Ui::MainWindowClass ui;
    std::unique_ptr<EventWriter> eventWriter_;
    std::unique_ptr<EventCapture> capture_;
    std::unique_ptr<SyntheticFrameSource> frameSource_;

    QLabel* statusLabel_{nullptr};
    QLabel* fpsLabel_{nullptr};
    QLabel* framesLabel_{nullptr};
    QLabel* bufferLabel_{nullptr};
    QLabel* resolutionLabel_{nullptr};
    QTimer* bufferReadoutTimer_{nullptr};

    // Acquisition count. The source thread increments it; the UI timer reads it.
    std::atomic<uint64_t> acquiredFrames_{0};
    uint64_t lastAcquiredSample_{0};
    std::chrono::steady_clock::time_point lastFpsSampleTime_{};

    // UI rendering backpressure guard
    std::atomic<bool> renderingFrame_{false};
    bool sourceFailed_{false};
};
