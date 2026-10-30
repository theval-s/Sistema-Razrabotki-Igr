#pragma once

#include <QIcon>
#include <QString>

#include <engine/assets/AssetCatalog.hpp>

#include <editor/editor_core_export.hpp>

namespace editor {

/// Placeholder icon for a kind of asset: a page with a coloured label. Cached; GUI thread only.
[[nodiscard]] EDITOR_CORE_API QIcon assetIcon(engine::CatalogAssetKind kind);

/// Plural, for menus and filters: "Textures", "Scenes", ...
[[nodiscard]] EDITOR_CORE_API QString assetKindName(engine::CatalogAssetKind kind);

}  // namespace editor
