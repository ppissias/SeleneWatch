#include "detection_panel.h"
#include "display_panel.h"
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
#include <QtGui/QFontDatabase>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
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

class UiTest : public QObject
{
    Q_OBJECT
private slots:
    void init() { QSettings saved; saved.clear(); saved.sync(); }
    void controlsPreserveValidSizeLimits();
    void livePanelInjectionEvidenceAndSavedSettings();
    void stretchARecordingWhileCapturingThenRestoreSettings();
};

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
    QVERIFY(panel.settings().valid());
}

void UiTest::livePanelInjectionEvidenceAndSavedSettings()
{
    QTemporaryDir output;
    QVERIFY(output.isValid());
    DetectionSettings chosen{7.5, 9, 500, true};
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
        start->click();
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
