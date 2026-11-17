#pragma once

#include <QString>

namespace editor {

[[nodiscard]] QString activeLogFile();
[[nodiscard]] QString fileLogError();
void initializeSessionLog();
void logSession(const QString& projectPath);

}  // namespace editor
