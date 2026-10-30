#pragma once

#include <QIcon>
#include <QString>

#include <engine/assets/AssetCatalog.hpp>

namespace content {

/// Placeholder icon for a kind of asset: a page with a coloured label. Cached; GUI thread only.
[[nodiscard]] QIcon assetIcon(engine::CatalogAssetKind kind);

/// Plural, for menus and filters: "Textures", "Scenes", ...
[[nodiscard]] QString assetKindName(engine::CatalogAssetKind kind);

}  // namespace content
