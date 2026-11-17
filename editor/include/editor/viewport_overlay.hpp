#pragma once

#include <string_view>
#include <memory>

#include <engine/core/Subsystem.hpp>

class QWidget;

namespace editor {

class Selection;
class TransformGizmo;
class ViewportLabels;

/// What the editor draws over the scene: selextion, colliders, rotation etc
/// Lines go to the RenderWorld's DebugDraw, which the renderer clears after each frame.
/// Register after RenderExtractionSubsystem and EditorCameraSubsystem: it runs after the
/// camera is final and before RenderingSubsystem::Render.
class ViewportOverlaySubsystem final : public engine::Subsystem {
public:
    /// `viewport` gives the size and scale of logical pixels.
    ViewportOverlaySubsystem(const Selection& selection, const TransformGizmo& gizmo, const QWidget& viewport);
    ~ViewportOverlaySubsystem() override;

    [[nodiscard]] std::string_view GetName() const override;
    bool Initialize() override;
    void LateUpdate(const engine::FrameContext& frame) override;
    void Shutdown() override;

private:
    const Selection& selection_;
    const TransformGizmo& gizmo_;
    const QWidget& viewport_;
    std::unique_ptr<ViewportLabels> labels_;
    bool overlayRegistered_ = false;
};

}  // namespace editor
