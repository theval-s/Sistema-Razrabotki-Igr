#pragma once

#include <QString>

#include <editor/editor_core_export.hpp>

class QMenu;
class QToolButton;

namespace editor {

/// A button that opens `menu` (which can also be set later with setMenu()) when clicked, with a
/// down arrow right of its text. A plain QToolButton with a menu tucks a small arrow into its
/// bottom-right corner instead.
[[nodiscard]] EDITOR_CORE_API QToolButton* makeMenuButton(const QString& text, QMenu* menu = nullptr);

}  // namespace editor
