#pragma once

#include <QString>

namespace editor::detail {
// Same one-shot initialization used by the application, with injectable paths
// for integration tests. Paths name files, not directories.
void initializeSessionLog(const QString& preferred, const QString& fallback);
}
