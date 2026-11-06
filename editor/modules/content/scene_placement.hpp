#pragma once

#include <QStringList>

#include <engine/assets/AssetCatalog.hpp>

class QString;

namespace editor {
class EditorContext;
}

namespace content {

/// The mesh that putting `path` in a scene renders: the mesh file itself, or the model.<ext> of a
/// model folder, as importing lays them out. Null for anything else.
[[nodiscard]] const engine::CatalogEntry* placeableMesh(const engine::AssetCatalog& catalog, const QString& path);

/// Adds an entity rendering each placeable item of `paths` in front of the viewport camera, side by
/// side, and selects them. A mesh gets the first material instance in its folder, or the default one.
/// Returns how many were added.
int addToScene(editor::EditorContext& context, const QStringList& paths);

}  // namespace content
