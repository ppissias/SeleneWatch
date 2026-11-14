#pragma once

#include "cameras/zwo_api.h"
#include <QtCore/QStringList>

class QCoreApplication;

// Manual camera QA command, also exercised with a deterministic SDK boundary.
// The caller owns the event loop; this function drains all session callbacks
// before returning. No camera is opened by CTest.
int runCameraProbe(QCoreApplication& app, const QStringList& args, std::shared_ptr<ZwoApi> api);
