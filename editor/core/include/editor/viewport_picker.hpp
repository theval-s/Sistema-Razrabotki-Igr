#pragma once

#include <optional>
#include <vector>

#include <SimpleMath.h>
#include <entt/entity/entity.hpp>

#include <editor/editor_core_export.hpp>
#include <editor/viewport_view.hpp>

namespace engine {
class Engine;
}

namespace editor {

class Selection;

/// Selects what is clicked in the viewport; clicking the same spot again goes to the next entity behind.
class EDITOR_CORE_API ViewportPicker {
public:
    using Vector2 = DirectX::SimpleMath::Vector2;

    ViewportPicker(engine::Engine& engine, Selection& selection);

    /// Selects the entity under `cursor`, or nothing; `toggle` adds or removes it instead.
    void pick(const ViewportView& view, Vector2 cursor, bool toggle);
    /// Forgets the last click, e.g. when another scene opens.
    void reset();

private:
    struct LastPick {
        Vector2 cursor;
        DirectX::SimpleMath::Matrix viewProjection;
        std::vector<entt::entity> hits;
        entt::entity picked;
    };

    /// Drawable entities under `cursor`, nearest first.
    [[nodiscard]] std::vector<entt::entity> hits(const ViewportView& view, Vector2 cursor) const;

    engine::Engine& engine_;
    Selection& selection_;
    std::optional<LastPick> last_;
};

}  // namespace editor
