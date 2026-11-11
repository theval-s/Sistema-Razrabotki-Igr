#pragma once

#include <QObject>
#include <QStringList>

#include <optional>

#include <SimpleMath.h>

#include <engine/assets/AssetCatalog.hpp>

class QString;

namespace editor {
class EditorContext;
}

namespace content {

/// The mesh that putting `path` in a scene renders: the mesh file itself, or the model.<ext> of a
/// model folder, as importing lays them out. Null for anything else.
[[nodiscard]] const engine::CatalogEntry* placeableMesh(const engine::AssetCatalog& catalog, const QString& path);

/// Adds an entity rendering each item of `paths` on `at`, else in front of the viewport camera
/// Returns how many were added.
int addToScene(editor::EditorContext& context, const QStringList& paths,
               std::optional<DirectX::SimpleMath::Vector3> at = std::nullopt);

/// Meshes and models dragged onto the viewport go where the cursor points
class ViewportDropTarget final : public QObject {
public:
    ViewportDropTarget(editor::EditorContext& context, QObject* parent);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] std::optional<DirectX::SimpleMath::Vector3> dropPoint(QPointF position) const;

    editor::EditorContext& context_;
    /// Of the drag over the viewport, checked once as it enters.
    QStringList paths_;
};

}  // namespace content
