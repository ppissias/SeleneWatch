#pragma once

#include <QtCore/QStringList>

// Feature-50 Stage 1: benchmarks runtime sub-pixel shift stabilization against
// unstabilized and integer-shifted detection on real recordings, controlled
// synthetic drift/failure/rotation tests, and Release x64 CPU microbenchmarks.
int runStabilizeBench(const QStringList& args);
