#pragma once
#include "cameras/qhy_api.h"
#include <QtCore/QStringList>
class QCoreApplication;
// Version does not enumerate or initialize SDK camera resources. Inspection
// and capture are explicit commands, deferred until hardware qualification.
int runQhyProbe(QCoreApplication& app,const QStringList& args,std::shared_ptr<QhyApi> api);
