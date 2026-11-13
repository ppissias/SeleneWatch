#include "preview_widget.h"

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

class PreviewWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void mapsTheWholeImageIncludingItsLastPixel();
    void marginsAndRightClicksDoNotSelect();
    void resizingKeepsTheSelectedFramePixel();
    void selectionNeverChangesTheImage();
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

QTEST_MAIN(PreviewWidgetTest)
#include "preview_tests.moc"
