#pragma once
#include "cameras/qhy_api.h"
#include <QtCore/QStringList>
class QCoreApplication;
// Version only loads the library and does not initialize camera resources.
int runQhyProbe(QCoreApplication& app,QStringList args,std::shared_ptr<QhyApi> api);
