#include <editor/tool_icon.hpp>

#include <QApplication>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>

namespace editor {

namespace {

constexpr int IconSize = 16;

/// Drawn at twice the size so it stays sharp on high-DPI screens.
QPixmap tinted(QSvgRenderer& svg, const QColor& color) {
    constexpr qreal Scale = 2.0;
    QPixmap pixmap(static_cast<int>(IconSize * Scale), static_cast<int>(IconSize * Scale));
    pixmap.setDevicePixelRatio(Scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    const QRectF bounds(0, 0, IconSize, IconSize);
    svg.render(&painter, bounds);
    // The SVG gives the shape, the palette the color.
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(bounds, color);
    return pixmap;
}

}  // namespace

QIcon toolIcon(const QString& name) {
    QSvgRenderer svg(QStringLiteral(":/icons/%1.svg").arg(name));
    const QPalette palette = QApplication::palette();
    QIcon icon;
    icon.addPixmap(tinted(svg, palette.color(QPalette::Active, QPalette::ButtonText)), QIcon::Normal);
    icon.addPixmap(tinted(svg, palette.color(QPalette::Disabled, QPalette::ButtonText)), QIcon::Disabled);
    return icon;
}

}  // namespace editor
