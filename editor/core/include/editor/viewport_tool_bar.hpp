#pragma once

#include <QWidget>

#include <array>

#include <editor/editor_core_export.hpp>

class QToolBar;

namespace editor {

/// The strip above the viewport: gizmo tools on the left, play controls centered, and uh.... right?
class EDITOR_CORE_API ViewportToolBar final : public QWidget {
public:
    enum class Section { Left, Center, Right };

    explicit ViewportToolBar(QWidget* parent = nullptr);

    [[nodiscard]] QToolBar* section(Section section) const;

private:
    std::array<QToolBar*, 3> sections_{};
};

}  // namespace editor
