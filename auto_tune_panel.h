#pragma once

#include "auto_tune.h"
#include "auto_tune_watch.h"

#include <QtWidgets/QWidget>

#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QToolButton;
class QVBoxLayout;

// Feature-26: the auto-tune panel, in its own window (Feature-28), opened by
// the Auto-tune button at the top of the Detection dock.
//
// Two steps, camera first and then K, each ending in suggestions the
// observer ticks and applies. The panel holds the latest suggestions and the
// K results it shows, and which events the observer excluded; it measures
// nothing itself. The window runs the measurements, applies what is asked,
// and tells the panel what is allowed now.
class AutoTunePanel final : public QWidget
{
    Q_OBJECT
public:
    explicit AutoTunePanel(QWidget* parent = nullptr);

    // What the window allows now, and what is being tuned.
    struct Situation
    {
        TuneWorkflow workflow{TuneWorkflow::MoonFlashes};
        bool preview{false};      // Preview is running.
        bool camera{false};       // The source is a camera, not a recording.
        bool recording{false};    // A manual recording is running.
        bool testingTools{false};
        bool busy{false};         // A measurement is waiting or running.
        bool haveBox{false};      // Moon flashes: an earthshine box is drawn.
        QString twilight;         // Advice about the Sun's altitude; empty when none.
    };
    void setSituation(const Situation& situation);

    // Moon flashes: what the earthshine area is, or why only the box is measured.
    void setAreaText(const QString& text);
    void setDrawingBox(bool on);
    [[nodiscard]] bool boxOnly() const;
    // Sky: the longest exposure for meteors, in microseconds.
    [[nodiscard]] double skyMaxExposureUs() const;
    [[nodiscard]] double watchSeconds() const;

    // The current measurement's progress, or the last result's state.
    void setProgress(const QString& text, double progress, bool running);

    // Step 1: the camera.
    void setCameraSuggestion(const CameraSuggestion& suggestion);
    void clearCamera(const QString& why);
    [[nodiscard]] std::vector<TuneRow> tickedCameraRows() const;

    // Step 2: K. A watch's rows replace the table; with only the live
    // detector measured, each Try K adds its row to the earlier ones.
    void setKResults(const TuneWatchStatus& status);
    void clearK(const QString& why);
    [[nodiscard]] std::optional<double> suggestedK() const;
    [[nodiscard]] double highestTestedK() const;
    // For the log line: the K values tested, their raw and remaining counts.
    [[nodiscard]] QString kRecord() const;

    // The flash check's result.
    void setCheckResult(const QString& text, std::optional<bool> caught);

signals:
    void drawBoxToggled(bool on);
    void boxOnlyToggled(bool on);
    void measureCameraRequested();
    void applyCameraRequested();
    void watchRequested(bool higher);
    void tryKRequested(double k);
    void applyKRequested();
    void checkFlashRequested();
    void cancelRequested();

private:
    void showK();
    void updateButtons();

    Situation situation_;
    QLabel* aim_;
    QLabel* twilight_;
    QWidget* boxRow_;
    QToolButton* drawBox_;
    QCheckBox* boxOnly_;
    QLabel* area_;
    QPushButton* measureCamera_;
    QWidget* skyMaxRow_;
    QDoubleSpinBox* skyMax_;
    QLabel* cameraVerdict_;
    QVBoxLayout* cameraRowsLayout_;
    std::vector<std::pair<QCheckBox*, TuneRow>> cameraRows_;
    QLabel* cameraWarnings_;
    QPushButton* applyCamera_;
    QComboBox* length_;
    QPushButton* watch_;
    QPushButton* higher_;
    QWidget* tryRow_;
    QDoubleSpinBox* tryK_;
    QPushButton* tryButton_;
    QTableWidget* kTable_;
    QListWidget* events_;
    QLabel* kVerdict_;
    QPushButton* applyK_;
    QPushButton* checkFlash_;
    QLabel* checkResult_;
    QLabel* status_;
    QProgressBar* progress_;
    QPushButton* cancel_;

    std::optional<CameraSuggestion> camera_;
    // Step 2's results: the rows shown, the watch length they are judged
    // against, and the observer's exclusions.
    std::vector<KRowResult> kRows_;
    double kWatchSeconds_{0};
    bool liveOnly_{false};
    double areaNoise_{0};
    PixelFormat format_{PixelFormat::Mono8};
    std::vector<TuneEventGroup> groups_;
    std::vector<bool> excluded_;
    KChoice choice_;
};
