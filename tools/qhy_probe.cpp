#include "tools/qhy_probe.h"
#include "tools/camera_probe.h"
#include "cameras/qhy_source.h"
#include "cameras/raw_camera_layout.h"
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTextStream>
#include <cmath>
#include <stdexcept>

namespace {
// Keep the opaque discovery identifier separate from the unresolved serial number.
QJsonObject profileJson(const QhyProfile& p)
{
    QJsonArray controls;
    for (const auto& c:p.controls) controls.append(QJsonObject{{"name",QString::fromStdString(c.name)},
        {"minimum",c.minimum},{"maximum",c.maximum},{"step",c.step},{"value",c.value}});
    return {{"camera",QString::fromStdString(p.camera.device.name)},{"deviceId",QString::fromStdString(p.camera.device.id)},
        {"serial",QJsonValue()},{"sdk",QString::fromStdString(p.sdkVersion)},{"sensorBits",p.camera.sensorBits},
        {"width",p.current.width},{"height",p.current.height},{"x",p.current.x},{"y",p.current.y},
        {"bin",1},{"binPolicy","commanded bin 1; no independent SDK getter; delivered geometry checked"},
        {"rawBits",p.current.format==PixelFormat::Mono16?16:8},{"controls",controls}};
}
}
int runQhyProbe(QCoreApplication& app,QStringList args,std::shared_ptr<QhyApi> api)
{
    try
    {
        if (!api) throw std::runtime_error("QHY support is not enabled. Configure the build with SELENEWATCH_QHY_SDK_DIR.");
        if (args.size()>=2 && args[1]=="--dll")
        {
            if (args.size()<4) throw std::invalid_argument("Use --dll ABSOLUTE_PATH before the probe command.");
            api->setRuntimeOverride(args.takeAt(2));
            args.removeAt(1);
        }
        if (args.size()==2 && args[1]=="version")
        { QTextStream(stdout)<<QString::fromStdString(api->version())<<'\n';return 0; }
        if (args.size()==3 && args[1]=="inspect")
        {
            QJsonArray list;for (const auto& d:api->devices()) list.append(profileJson(QhySource::inspect(api,d.id)));
            const auto bytes=QJsonDocument(QJsonObject{{"cameras",list},{"sdk",QString::fromStdString(api->version())},
                {"runtimePath",api->loadedRuntimePath()}}).toJson();
            QFile output(args[2]);return output.open(QIODevice::WriteOnly|QIODevice::NewOnly) && output.write(bytes)==bytes.size() && output.flush()?0:1;
        }
        // Resolve QHY-specific settings before handing capture and verification to the common runner.
        return runCameraProbeCommand(app,args,[api](const QJsonObject& config) {
            if (config.value("readDelayMs").toInt()!=0) throw std::invalid_argument("QHY QA read-delay mode is not implemented");
            const auto devices=api->devices();
            if (devices.empty()) throw std::runtime_error("QHY runtime loaded. Connect a QHY camera or check its Windows driver installation.");
            if (config.contains("id") && !config.value("id").isString()) throw std::invalid_argument("QHY id must be an SDK device ID string");
            const auto id=config.value("id").toString(QString::fromStdString(devices.front().id)).toStdString();
            auto profile=QhySource::inspect(api,id);auto settings=profile.current;
            settings.width=config.value("width").toInt(profile.camera.width);settings.height=config.value("height").toInt(profile.camera.height);
            settings.x=config.value("x").toInt(0);settings.y=config.value("y").toInt(0);settings.bin=config.value("bin").toInt(1);
            settings.format=config.value("raw16").toBool()?PixelFormat::Mono16:PixelFormat::Mono8;
            const auto control=[&](const char* key,QhyControl kind) {
                if (!config.contains(key)) return;
                const double value=config.value(key).toDouble();
                if (!config.value(key).isDouble() || !std::isfinite(value)) throw std::invalid_argument("QHY control must be a finite number");
                settings.controls[kind]=value;
            };
            control("exposureUs",QhyControl::Exposure);control("gain",QhyControl::Gain);control("offset",QhyControl::Offset);
            control("usbTraffic",QhyControl::UsbTraffic);control("transferSpeed",QhyControl::TransferSpeed);
            auto source=std::make_unique<QhySource>(api,profile,settings);auto* camera=source.get();
            return CameraProbeInput{std::move(source),[camera] {
                const auto p=camera->actualProfile();return CameraProbeReadback{profileJson(p),p.current.width,p.current.height,p.current.format,
                    roiBayerPattern(p.camera.bayer,p.current.x,p.current.y)};
            }};
        });
    }
    catch (const std::exception& ex) { QTextStream(stderr)<<ex.what()<<'\n';return 1; }
}
