#include "tools/camera_probe.h"
#include "tests/fake_zwo_api.h"
#include "tests/fake_qhy_api.h"
#include "tools/qhy_probe.h"
#include "recording/ser_reader.h"
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtTest/QTest>

namespace {
QJsonObject readJson(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
int run(const QString& root, const QJsonObject& config, const std::shared_ptr<ZwoApi>& api)
{
    QFile file(root+"/config.json");
    if (!file.open(QIODevice::WriteOnly)) return -1;
    file.write(QJsonDocument(config).toJson()); file.close();
    return runCameraProbe(*QCoreApplication::instance(),{"probe","soak",root+"/result",file.fileName()},api);
}
QJsonObject shortRun()
{
    return {{"seconds",1.1},{"manualIntervalSeconds",.32},{"sampleIntervalSeconds",.05},
        {"historySeconds",.05},{"postSeconds",.15},{"maxPostSeconds",.4}};
}
}

// Runs the bench and soak workflow against fake SDKs to verify capture and progress reports.
class CameraProbeTest : public QObject
{
    Q_OBJECT
private slots:
    void qhyBayerSoakUsesTheSharedRunnerAndNeverInventsLossCounts()
    {
        QTemporaryDir dir;auto api=std::make_shared<FakeQhyApi>();auto config=shortRun();
        config["raw16"]=true;config["width"]=64;config["height"]=32;config["x"]=1;config["y"]=1;config["gain"]=13.37;
        QFile file(dir.path()+"/qhy.json");QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(config).toJson());file.close();
        QCOMPARE(runQhyProbe(*QCoreApplication::instance(),{"probe","soak",dir.path()+"/result",file.fileName()},api),0);
        const auto report=readJson(dir.path()+"/result/bench.json");
        QVERIFY(report["sdkDrops"].isNull());QVERIFY(report["serial"].isNull());QCOMPARE(report["savedSampleBits"].toInt(),16);
        QVERIFY(report["previewHashes"].toInt()>0);QCOMPARE(report["previewHashes"],report["matchedPreviewHashes"]);
        const auto outputs=report["outputs"].toArray();QVERIFY(outputs.size()>=3);
        for(const auto& output:outputs)
        {
            SerReader reader;std::string error;
            const auto path=output.toObject()["folder"].toString()+"/event.ser";
            QVERIFY(reader.open(std::filesystem::path(path.toStdWString()),30,error));
            Frame f;f.data.resize(reader.frameBytes());QVERIFY(reader.readFrame(0,f,error));
            QCOMPARE(f.width,64);QCOMPARE(f.height,32);QCOMPARE(f.bayer,BayerPattern::BGGR);QVERIFY(f.testSource);
        }
        QCOMPARE(api->opened.load(),api->closed.load());
    }
    void periodicClipsAndProgressRetainNativeEvidence()
    {
        for (bool raw16 : {false,true})
        {
            QTemporaryDir dir; auto api=std::make_shared<FakeZwoApi>();
            auto config=shortRun(); config["raw16"]=raw16;
            QCOMPARE(run(dir.path(),config,api),0);
            const auto result=readJson(dir.path()+"/result/bench.json");
            QCOMPARE(result["mode"].toString(),"soak"); QCOMPARE(result["stopReason"].toString(),"duration");
            QVERIFY(result["acquiredFrames"].toInt()>10);
            QCOMPARE(result["previewHashes"],result["matchedPreviewHashes"]);
            QVERIFY(result["previewHashes"].toInt()>0); QCOMPARE(result["memoryRefusals"].toInt(),0);
            const auto outputs=result["outputs"].toArray(); QVERIFY(outputs.size()>=3);
            int saved=0;
            for (const auto& output:outputs)
            {
                const auto path=output.toObject()["folder"].toString()+"/event.ser";
                SerReader reader; std::string error;
                QVERIFY2(reader.open(std::filesystem::path(path.toStdWString()),30,error),error.c_str());
                saved+=reader.frameCount();
                Frame f; f.data.resize(reader.frameBytes()); QVERIFY(reader.readFrame(0,f,error));
                QCOMPARE(f.format,raw16?PixelFormat::Mono16:PixelFormat::Mono8); QVERIFY(f.testSource);
                for (size_t i=0;i<static_cast<size_t>(f.width)*f.height;++i)
                {
                    if (raw16) QCOMPARE(static_cast<int>(f.data[2*i]|(f.data[2*i+1]<<8)),static_cast<int>((400+i%13)<<4));
                    else QCOMPARE(f.data[i],static_cast<uint8_t>(100+i%13));
                }
            }
            QVERIFY(saved<result["acquiredFrames"].toInt()); // Periodic capture leaves real gaps between clips.
            QFile progress(dir.path()+"/result/progress.jsonl"); QVERIFY(progress.open(QIODevice::ReadOnly));
            int rows=0; double previous=-1;
            while (!progress.atEnd())
            {
                const auto row=QJsonDocument::fromJson(progress.readLine()).object(); QVERIFY(!row.isEmpty());
                QVERIFY(row["elapsedSeconds"].toDouble()>=previous); previous=row["elapsedSeconds"].toDouble();
                QVERIFY(row.contains("chargedBytes")); QVERIFY(row.contains("writerOutstandingClips")); ++rows;
            }
            QVERIFY(rows>=5); QCOMPARE(api->opened.load(),api->closed.load());
            QCOMPARE(api->starts.load(),1); QCOMPARE(api->stops.load(),1);
        }
    }
    void outputLimitAndDisconnectFinishAvailableEvidence()
    {
        for (bool disconnect : {false,true})
        {
            QTemporaryDir dir; auto api=std::make_shared<FakeZwoApi>(); auto config=shortRun(); config["seconds"]=2;
            if (disconnect) api->disconnectAt=70;
            else
            {
                config["seconds"]=4; config["maximumOutputBytes"]=5000; config["postSeconds"]=.5;
                config["maxPostSeconds"]=.8; config["manualIntervalSeconds"]=.4;
            }
            QCOMPARE(run(dir.path(),config,api),1);
            const auto report=readJson(dir.path()+"/result/bench.json");
            QCOMPARE(report["stopReason"].toString(),disconnect?"source-failed":"output-limit");
            QVERIFY(!report["failure"].toString().isEmpty());
            QCOMPARE(api->opened.load(),api->closed.load());
            QVERIFY(!report["outputs"].toArray().empty());
            for (const auto& output:report["outputs"].toArray())
            {
                QFile note(output.toObject()["folder"].toString()+"/event.txt"); QVERIFY(note.open(QIODevice::ReadOnly));
                QVERIFY(note.readAll().contains("complete: yes"));
            }
        }
    }
    void requestedStopDrainsTheWriter()
    {
        QTemporaryDir dir; QObject context; auto api=std::make_shared<FakeZwoApi>(); auto config=shortRun();
        config["seconds"]=3;
        QTimer::singleShot(200,&context,[&] { QFile stop(dir.path()+"/result/stop.request"); QVERIFY(stop.open(QIODevice::WriteOnly)); });
        QCOMPARE(run(dir.path(),config,api),0);
        const auto result=readJson(dir.path()+"/result/bench.json");
        QCOMPARE(result["stopReason"].toString(),"requested-stop"); QVERIFY(result["elapsedSeconds"].toDouble()<2.5);
        QVERIFY(!result["outputs"].toArray().empty()); QCOMPARE(api->starts.load(),api->stops.load());
    }
    void invalidConfigurationNeverOpensACameraOrOverwritesResults()
    {
        QTemporaryDir dir; auto api=std::make_shared<FakeZwoApi>(); auto config=shortRun(); config["manualIntervalSeconds"]=0;
        QCOMPARE(run(dir.path(),config,api),1); QCOMPARE(api->opened.load(),0);
        config=shortRun(); config["seconds"]=.35; QCOMPARE(run(dir.path(),config,api),0);
        QFile previous(dir.path()+"/result/bench.json"); QVERIFY(previous.open(QIODevice::ReadOnly)); const auto before=previous.readAll(); previous.close();
        QCOMPARE(run(dir.path(),config,api),1);
        QVERIFY(previous.open(QIODevice::ReadOnly)); QCOMPARE(previous.readAll(),before);
    }
};
int main(int argc,char** argv)
{
    QGuiApplication app(argc,argv); CameraProbeTest test; return QTest::qExec(&test,argc,argv);
}
#include "camera_probe_tests.moc"
