#pragma once

#include <QtCore/QString>

// Builds an offline review from the journal and immutable files in one watch.
// Returns the report path after an atomic replacement, or an error.
QString writeNightlyReport(const QString& folder, QString& error);
