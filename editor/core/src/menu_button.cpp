#include <editor/menu_button.hpp>

#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>

namespace editor {

namespace {

constexpr int ArrowSize = 10;

/// The style's own down arrow. Drawn at twice the size so it stays sharp on high-DPI screens.
QPixmap arrowPixmap(QStyle& style, const QStyle::State state) {
    constexpr qreal Scale = 2.0;
    QPixmap pixmap(static_cast<int>(ArrowSize * Scale), static_cast<int>(ArrowSize * Scale));
    pixmap.setDevicePixelRatio(Scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    QStyleOption option;
    option.rect = QRect(0, 0, ArrowSize, ArrowSize);
    option.palette = QApplication::palette();
    option.palette.setCurrentColorGroup(state & QStyle::State_Enabled ? QPalette::Active : QPalette::Disabled);
    option.state = state;
    style.drawPrimitive(QStyle::PE_IndicatorArrowDown, &option, &painter);
    return pixmap;
}

/// Shows the arrow as its icon, right of the text, instead of the style's menu indicator.
class MenuButton final : public QToolButton {
protected:
    void initStyleOption(QStyleOptionToolButton* option) const override {
        QToolButton::initStyleOption(option);
        option->features &= ~QStyleOptionToolButton::HasMenu;
        // Only for drawing: the widget stays left-to-right, so the menu still opens left-aligned.
        option->direction = Qt::RightToLeft;
    }
};

}  // namespace

QToolButton* makeMenuButton(const QString& text, QMenu* menu) {
    auto* button = new MenuButton;
    button->setText(text);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setMenu(menu);
    QIcon arrow;
    arrow.addPixmap(arrowPixmap(*button->style(), QStyle::State_Enabled), QIcon::Normal);
    arrow.addPixmap(arrowPixmap(*button->style(), QStyle::State_None), QIcon::Disabled);
    button->setIcon(arrow);
    button->setIconSize(QSize(ArrowSize, ArrowSize));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    return button;
}

}  // namespace editor
