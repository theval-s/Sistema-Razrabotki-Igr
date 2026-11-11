#include <editor/viewport_tool_bar.hpp>

#include <QHBoxLayout>
#include <QToolBar>

namespace editor {

ViewportToolBar::ViewportToolBar(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    for (QToolBar*& section : sections_) {
        section = new QToolBar;
        section->setIconSize({16, 16});
    }
    // Equal stretch on both sides keeps the center section centered while there's room.
    layout->addWidget(section(Section::Left), 1, Qt::AlignLeft);
    layout->addWidget(section(Section::Center));
    layout->addWidget(section(Section::Right), 1, Qt::AlignRight);
}

QToolBar* ViewportToolBar::section(const Section section) const {
    return sections_[static_cast<std::size_t>(section)];
}

}  // namespace editor
