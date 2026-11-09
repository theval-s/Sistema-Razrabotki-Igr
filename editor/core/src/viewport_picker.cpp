#include <editor/viewport_picker.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/rendering/RenderComponents.hpp>
#include <engine/scene/Scene.hpp>
#include <engine/scene/SceneQueries.hpp>

#include <editor/selection.hpp>

namespace editor {

namespace {

/// A click at most this many pixels from the last one, along each axis, is on the same spot.
constexpr float SameSpotPixels = 3.0f;

}  // namespace

ViewportPicker::ViewportPicker(engine::Engine& engine, Selection& selection) : engine_(engine), selection_(selection) {}

void ViewportPicker::pick(const ViewportView& view, const Vector2 cursor, const bool toggle) {
    std::vector<entt::entity> found = hits(view, cursor);
    const DirectX::SimpleMath::Matrix view_projection = view.view * view.projection;
    // Only while nothing changed since the last click, including what it selected.
    const bool cycle = last_ && std::abs(cursor.x - last_->cursor.x) <= SameSpotPixels &&
                       std::abs(cursor.y - last_->cursor.y) <= SameSpotPixels &&
                       view_projection == last_->viewProjection && found == last_->hits &&
                       (toggle || selection_.primary() == last_->picked);
    std::size_t index = 0;
    if (cycle) {
        const auto previous = std::ranges::find(found, last_->picked);
        if (previous != found.end()) index = (static_cast<std::size_t>(previous - found.begin()) + 1) % found.size();
    }
    const entt::entity picked = found.empty() ? entt::null : found[index];
    if (found.empty()) {
        last_.reset();
    } else {
        last_ = LastPick{cursor, view_projection, std::move(found), picked};
    }
    if (toggle) {
        if (picked != entt::null) selection_.toggle(picked);
    } else {
        selection_.select(picked);
    }
}

void ViewportPicker::reset() {
    last_.reset();
}

std::vector<entt::entity> ViewportPicker::hits(const ViewportView& view, const Vector2 cursor) const {
    const ViewportView::Ray ray = view.ray(cursor);
    engine::Scene& scene = engine_.GetScene();
    engine::AssetSystem& assets = engine_.GetSceneAssets();
    std::vector<entt::entity> drawable;
    for (const engine::RayHit& hit : engine::RaycastAll(scene, assets, ray.origin, ray.direction, ray.length)) {
        // The general scene query also returns authored Bounds without a drawable mesh.
        const auto* renderer = scene.TryGetComponent<engine::MeshRenderer>(hit.entity);
        if (renderer && assets.Peek(renderer->mesh) && assets.Peek(renderer->materialInstance)) {
            drawable.push_back(hit.entity);
        }
    }
    return drawable;
}

}  // namespace editor
