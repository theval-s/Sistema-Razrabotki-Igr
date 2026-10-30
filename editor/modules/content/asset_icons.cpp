#include "asset_icons.hpp"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace content {

namespace {

struct Style {
    const char* label;
    QColor color;
};

Style styleOf(const engine::CatalogAssetKind kind) {
    switch (kind) {
    case engine::CatalogAssetKind::Mesh:
        return {"MESH", QColor(0xc7, 0x7d, 0x2a)};
    case engine::CatalogAssetKind::Texture:
            //TODO: figure out thumbnails, QFileIconProvider?
        return {"IMG", QColor(0x3d, 0x9a, 0x5b)};
    case engine::CatalogAssetKind::Shader:
        return {"HLSL", QColor(0x7a, 0x52, 0xb3)};
    case engine::CatalogAssetKind::Material:
        return {"MAT", QColor(0x2f, 0x7f, 0xc1)};
    case engine::CatalogAssetKind::MaterialInstance:
        return {"INST", QColor(0x3a, 0xa3, 0xc9)};
    case engine::CatalogAssetKind::Scene:
        return {"SCN", QColor(0xc4, 0x44, 0x3c)};
    }
    return {"?", Qt::gray};
}

QIcon paint(const Style& style) {
    // One large pixmap; QIcon scales it down for smaller views.
    constexpr int Size = 128;
    QPixmap pixmap(Size, Size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    // it's generated but it looks cool at least, but those sizes..... it's hell
    const QRectF page(Size * 0.18, Size * 0.06, Size * 0.64, Size * 0.88);
    const qreal fold = Size * 0.18;
    QPainterPath outline;
    outline.moveTo(page.topLeft());
    outline.lineTo(page.right() - fold, page.top());
    outline.lineTo(page.right(), page.top() + fold);
    outline.lineTo(page.bottomRight());
    outline.lineTo(page.bottomLeft());
    outline.closeSubpath();
    painter.setPen(QPen(QColor(0x8a, 0x8a, 0x8a), 3.0));
    painter.setBrush(QColor(0xe6, 0xe6, 0xe6));
    painter.drawPath(outline);
    QPainterPath corner;
    corner.moveTo(page.right() - fold, page.top());
    corner.lineTo(page.right() - fold, page.top() + fold);
    corner.lineTo(page.right(), page.top() + fold);
    painter.setBrush(QColor(0xc8, 0xc8, 0xc8));
    painter.drawPath(corner);

    // The kind's label on a coloured band across the page.
    const QRectF band(Size * 0.08, page.top() + page.height() * 0.52, Size * 0.84, Size * 0.26);
    painter.setPen(Qt::NoPen);
    painter.setBrush(style.color);
    painter.drawRoundedRect(band, 6.0, 6.0);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(static_cast<int>(band.height() * 0.62));
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(band, Qt::AlignCenter, QString::fromLatin1(style.label));
    return QIcon(pixmap);
}

}  // namespace

QIcon assetIcon(const engine::CatalogAssetKind kind) {
    static QHash<int, QIcon> cache;
    auto it = cache.find(static_cast<int>(kind));
    if (it == cache.end()) {
        it = cache.insert(static_cast<int>(kind), paint(styleOf(kind)));
    }
    return *it;
}

QString assetKindName(const engine::CatalogAssetKind kind) {
    switch (kind) {
    case engine::CatalogAssetKind::Mesh:
        return QObject::tr("Meshes");
    case engine::CatalogAssetKind::Texture:
        return QObject::tr("Textures");
    case engine::CatalogAssetKind::Shader:
        return QObject::tr("Shaders");
    case engine::CatalogAssetKind::Material:
        return QObject::tr("Materials");
    case engine::CatalogAssetKind::MaterialInstance:
        return QObject::tr("Material Instances");
    case engine::CatalogAssetKind::Scene:
        return QObject::tr("Scenes");
    }
    return {};
}

}  // namespace content
