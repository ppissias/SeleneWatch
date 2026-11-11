#pragma once

#include "frame.h"
#include "frame_ring_buffer.h"
#include "synthetic_frame_source.h"
#include "ui_mainwindow.h"

#include <QtWidgets/QMainWindow>
#include <QtWidgets/QLabel>
#include <memory>
#include <atomic>
#include <chrono>

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

private:
    void setupStatusBar();
    void updateBufferReadout();
    void updateStreamReadout();
    void onFrameArrived(Frame& frame);
    void displayImage(QImage image);

    Ui::MainWindowClass ui;
    std::unique_ptr<SyntheticFrameSource> frameSource_;
    std::unique_ptr<FrameRingBuffer> ringBuffer_;

    // Status bar indicator labels
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
};
