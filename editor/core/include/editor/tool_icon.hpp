#pragma once

#include <QIcon>
#include <QString>

#include <editor/editor_core_export.hpp>

namespace editor {

/// :/icons/<name>.svg in the palette's text color, so it suits light and dark themes.
[[nodiscard]] EDITOR_CORE_API QIcon toolIcon(const QString& name);

}  // namespace editor
