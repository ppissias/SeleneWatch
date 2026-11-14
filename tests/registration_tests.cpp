#include "tools/translation_estimator.h"
#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
Frame scene(int dx = 0, int dy = 0, double gain = 1, double offset = 0)
{
    Frame frame; frame.width = 160; frame.height = 128; frame.data.resize(160*128);
    for (int y = 0; y < 128; ++y) for (int x = 0; x < 160; ++x)
    {
        const double sx = x-dx, sy = y-dy;
        const double value = 80 + 30*std::sin(sx*0.071) + 20*std::cos(sy*0.113)
            + 15*std::sin((sx+sy)*0.047) + 25*std::exp(-((sx-75)*(sx-75)+(sy-63)*(sy-63))/150);
        frame.data[static_cast<size_t>(y)*160+x] = static_cast<uint8_t>(std::clamp(std::round(value*gain+offset),0.0,255.0));
    }
    return frame;
}
}

class RegistrationTest : public QObject
{
    Q_OBJECT
private slots:
    void findsKnownShiftsWithoutChangingSamples()
    {
        const Frame reference = scene();
        TranslationEstimator estimator(reference, 16);
        for (const auto [x,y] : {std::pair{0,0}, {3,-4}, {-11,9}, {16,-16}})
        {
            Frame moved = scene(x,y,1.08,5);
            const auto bytes = moved.data;
            const auto estimate = estimator.estimate(moved);
            QVERIFY(estimate.valid);
            QCOMPARE(estimate.dx,x); QCOMPARE(estimate.dy,y);
            QVERIFY(estimate.correlation > 0.99);
            QCOMPARE(moved.data,bytes);
        }
    }
    void rejectsFeaturelessAndAmbiguousFields()
    {
        Frame frame = scene(); frame.data.assign(frame.data.size(),100);
        TranslationEstimator flat(frame,16);
        QVERIFY(!flat.estimate(frame).valid);
        for (int y=0;y<128;++y) for (int x=0;x<160;++x) frame.data[static_cast<size_t>(y)*160+x]=static_cast<uint8_t>(30+x);
        TranslationEstimator ramp(frame,16);
        QVERIFY(!ramp.estimate(frame).valid);
    }
    void rejectsAnUnaccountedBayerPhase()
    {
        Frame frame = scene(); frame.bayer = BayerPattern::RGGB;
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, TranslationEstimator(frame,16));
    }
    void rejectsInvalidSearchAndInterior()
    {
        const Frame frame = scene();
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, TranslationEstimator(frame,-1));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, TranslationEstimator(frame,257));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, static_cast<void>(registrationInterior({},16,0,0)));
        const auto source = std::make_shared<const Frame>(frame);
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, static_cast<void>(registrationInterior(source,64,0,0)));
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, static_cast<void>(registrationInterior(source,16,17,0)));
    }
    void commonInteriorKeepsNativeWordsAndTiming()
    {
        const auto native = [](Frame frame, PixelFormat format, BitAlignment alignment) {
            const auto samples=frame.data;
            frame.data.clear(); frame.format=format; frame.alignment=alignment;
            frame.significantBits=format==PixelFormat::Mono16?12:8;
            for (const auto sample:samples)
            {
                if (format==PixelFormat::Mono16)
                {
                    const int value=(sample*4095/255) << (alignment==BitAlignment::High?4:0);
                    frame.data.push_back(static_cast<uint8_t>(value&255)); frame.data.push_back(static_cast<uint8_t>(value>>8));
                }
                else if (format==PixelFormat::RGB888) { frame.data.push_back(sample); frame.data.push_back(255-sample); frame.data.push_back(sample/2); }
                else frame.data.push_back(sample);
            }
            return frame;
        };
        for (auto format:{PixelFormat::Mono8,PixelFormat::Mono16,PixelFormat::RGB888})
            for (auto alignment:{BitAlignment::Low,BitAlignment::High})
            {
                if (format!=PixelFormat::Mono16 && alignment==BitAlignment::High) continue;
                Frame moved=native(scene(3,-4),format,alignment);
                moved.frameIndex=99; moved.timestamp=std::chrono::system_clock::time_point{}+std::chrono::seconds{3};
                const auto raw=moved.data;
                const auto source=std::make_shared<const Frame>(moved);
                const auto referenceSource=std::make_shared<const Frame>(native(scene(),format,alignment));
                TranslationEstimator estimator(*referenceSource,16);
                const auto shift=estimator.estimate(*source);
                QVERIFY(shift.valid); QCOMPARE(shift.dx,3); QCOMPARE(shift.dy,-4);
                const auto aligned=registrationInterior(source,32,shift.dx,shift.dy);
                const auto reference=registrationInterior(referenceSource,32,0,0);
                QCOMPARE(aligned->data,reference->data);
                QCOMPARE(source->data,raw);
                QCOMPARE(aligned->frameIndex,source->frameIndex);
                QVERIFY(aligned->timestamp==source->timestamp);
                QCOMPARE(aligned->significantBits,source->significantBits);
                QCOMPARE(aligned->alignment,source->alignment);
                QVERIFY(aligned->testSource);
            }
    }
};
QTEST_GUILESS_MAIN(RegistrationTest)
#include "registration_tests.moc"
