#include "preview_widget.h"

#include <QtGui/QWheelEvent>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>

#include <cmath>

// Checks selection coordinates through scaling and letterboxing, and verifies source image bytes.
class PreviewWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void mapsTheWholeImageIncludingItsLastPixel();
    void marginsAndRightClicksDoNotSelect();
    void resizingKeepsTheSelectedFramePixel();
    void selectionNeverChangesTheImage();
    void zoomPanAndCoordinateAccuracy();
};

void PreviewWidgetTest::mapsTheWholeImageIncludingItsLastPixel()
{
    PreviewWidget preview;
    preview.resize(802, 602);
    QVERIFY(!preview.framePosition({400, 300}));
    preview.setImage(QImage(640, 480, QImage::Format_Grayscale8));
    QCOMPARE(preview.imageRect(), QRect(1, 1, 800, 600));
    QCOMPARE(*preview.framePosition({1, 1}), QPoint(0, 0));
    QCOMPARE(*preview.framePosition({401, 301}), QPoint(320, 240));
    QCOMPARE(*preview.framePosition({800.9, 600.9}), QPoint(639, 479));
    QVERIFY(!preview.framePosition({801, 300}));
    QVERIFY(!preview.framePosition({400, 601}));
    QVERIFY(!preview.framePosition({0.9, 300}));
    QVERIFY(!preview.framePosition({400, 0.9}));
}

void PreviewWidgetTest::marginsAndRightClicksDoNotSelect()
{
    PreviewWidget preview;
    preview.resize(1002, 602);
    preview.setImage(QImage(640, 480, QImage::Format_Grayscale8));
    QCOMPARE(preview.imageRect(), QRect(101, 1, 800, 600));
    QSignalSpy selected(&preview, &PreviewWidget::positionSelected);
    QTest::mouseClick(&preview, Qt::LeftButton, Qt::NoModifier, {50, 300});
    QVERIFY(!preview.selectedPosition());
    QTest::mouseClick(&preview, Qt::RightButton, Qt::NoModifier, {501, 301});
    QVERIFY(!preview.selectedPosition());
    QCOMPARE(selected.count(), 0);
    QTest::mouseClick(&preview, Qt::LeftButton, Qt::NoModifier, {501, 301});
    QVERIFY(preview.selectedPosition());
    QCOMPARE(*preview.selectedPosition(), QPoint(320, 240));
    QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.takeFirst().at(0).toPoint(), QPoint(320, 240));

    preview.resize(802, 802);
    QCOMPARE(preview.imageRect(), QRect(1, 101, 800, 600));
    QVERIFY(!preview.framePosition({400, 50}));
    QVERIFY(!preview.framePosition({400, 750}));
}

void PreviewWidgetTest::resizingKeepsTheSelectedFramePixel()
{
    PreviewWidget preview;
    preview.resize(802, 602);
    preview.setImage(QImage(640, 480, QImage::Format_Grayscale8));
    QTest::mouseClick(&preview, Qt::LeftButton, Qt::NoModifier, {251, 188});
    QCOMPARE(*preview.selectedPosition(), QPoint(200, 149));
    preview.resize(642, 482);
    QCOMPARE(preview.imageRect(), QRect(1, 1, 640, 480));
    QCOMPARE(*preview.selectedPosition(), QPoint(200, 149));
    QCOMPARE(*preview.framePosition({201, 150}), QPoint(200, 149));
    preview.setImage(QImage(640, 480, QImage::Format_Grayscale8));
    QVERIFY(preview.selectedPosition());
    preview.setImage(QImage(320, 240, QImage::Format_Grayscale8));
    QVERIFY(!preview.selectedPosition());
    preview.clear();
    QVERIFY(!preview.framePosition({200, 150}));
}

void PreviewWidgetTest::selectionNeverChangesTheImage()
{
    QImage image(640, 480, QImage::Format_RGB888);
    image.fill(QColor(10, 20, 30));
    const QImage original = image.copy();
    PreviewWidget preview;
    preview.resize(642, 482);
    preview.setImage(image); // Shares the pixels until a write; painting must not write them.
    QTest::mouseClick(&preview, Qt::LeftButton, Qt::NoModifier, {321, 241});
    QImage screen(preview.size(), QImage::Format_RGB32);
    preview.render(&screen);
    QVERIFY(screen.pixelColor(326, 241) != original.pixelColor(325, 240));
    QCOMPARE(screen.pixelColor(321, 241), original.pixelColor(320, 240));
    QCOMPARE(image, original);
}

void PreviewWidgetTest::zoomPanAndCoordinateAccuracy()
{
    PreviewWidget preview;
    preview.resize(802, 602);
    preview.setImage(QImage(640, 480, QImage::Format_Grayscale8));

    QVERIFY(preview.isFitToWindow());
    QCOMPARE(preview.imageRect(), QRect(1, 1, 800, 600));

    QSignalSpy zoomSpy(&preview, &PreviewWidget::zoomChanged);
    QSignalSpy selSpy(&preview, &PreviewWidget::positionSelected);

    // 100% actual pixels: 640x480 centered inside 800x600 viewport.
    preview.setZoom(1.0);
    QVERIFY(!preview.isFitToWindow());
    QVERIFY(std::abs(preview.zoom() - 1.0) < 1e-6);
    QCOMPARE(preview.imageRect(), QRect(81, 61, 640, 480));
    QCOMPARE(*preview.framePosition({81, 61}), QPoint(0, 0));
    QCOMPARE(*preview.framePosition({401, 301}), QPoint(320, 240));
    QCOMPARE(zoomSpy.count(), 1);

    // 200%: 1280x960 centered inside 800x600 viewport -> left = 1 + (800-1280)/2 = -239.
    preview.setZoom(2.0);
    QCOMPARE(preview.imageRect().size(), QSize(1280, 960));
    QCOMPARE(*preview.framePosition({401, 301}), QPoint(320, 240));

    // Right-drag panning shifts the visible portion without selecting a pixel.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(400, 300), QPointF(400, 300),
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(500, 340), QPointF(500, 340),
        Qt::NoButton, Qt::RightButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(500, 340), QPointF(500, 340),
        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&preview, &press);
    QApplication::sendEvent(&preview, &move);
    QApplication::sendEvent(&preview, &release);
    QCOMPARE(selSpy.count(), 0);
    QCOMPARE(preview.panOffset(), QPointF(100, 40));
    // Shifted right by 100 widget px (50 frame px at 200%) and down by 40 widget px (20 frame px).
    QCOMPARE(*preview.framePosition({401, 301}), QPoint(270, 220));

    // Wheel zoom around an off-center cursor keeps that frame pixel pinned under the cursor.
    const QPointF cursor(250, 200);
    const auto beforeWheel = preview.framePosition(cursor);
    QVERIFY(beforeWheel.has_value());
    QWheelEvent wheel(cursor, cursor, QPoint(), QPoint(0, 120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&preview, &wheel);
    QVERIFY(preview.zoom() > 2.4);
    const auto afterWheel = preview.framePosition(cursor);
    QVERIFY(afterWheel.has_value());
    QVERIFY(std::abs(afterWheel->x() - beforeWheel->x()) <= 1);
    QVERIFY(std::abs(afterWheel->y() - beforeWheel->y()) <= 1);

    // Back to Fit restores original rect and clears pan.
    preview.setFitToWindow();
    QVERIFY(preview.isFitToWindow());
    QCOMPARE(preview.panOffset(), QPointF(0, 0));
    QCOMPARE(preview.imageRect(), QRect(1, 1, 800, 600));
}

QTEST_MAIN(PreviewWidgetTest)
#include "preview_tests.moc"
