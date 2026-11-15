#pragma once

#include "cameras/zwo_api.h"
#include "sources/frame_source.h"
#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <functional>

class QCoreApplication;

// Manual camera QA command, also exercised with a deterministic SDK boundary.
// The caller owns the event loop; this function drains all session callbacks
// before returning. No camera is opened by CTest.
int runCameraProbe(QCoreApplication& app, const QStringList& args, std::shared_ptr<ZwoApi> api);

// Native layout and metadata used to verify clips produced by a diagnostic run.
struct CameraProbeReadback
{
    QJsonObject metadata;
    int width{0},height{0};
    PixelFormat format{PixelFormat::Mono8};
    BayerPattern bayer{BayerPattern::None};
};
// Source and readback function supplied by the vendor-specific probe to the shared runner.
struct CameraProbeInput
{
    std::unique_ptr<FrameSource> source;
    // Called exactly once after Session opens the source, while it still owns
    // it. Reads an immutable profile; it must not call the live SDK from GUI.
    std::function<CameraProbeReadback()> readback;
};
// Shared ASI/QHY diagnostic loop: output guards, periodic capture, resource
// sampling, writer drain and native evidence verification. Vendor setup stays
// in its two callers, not in another production camera hierarchy.
int runCameraProbeCommand(QCoreApplication& app,const QStringList& args,
    const std::function<CameraProbeInput(const QJsonObject&)>& input);
