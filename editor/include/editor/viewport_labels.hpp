#pragma once

#include <memory>
#include <QFont>
#include <QPointF>
#include <QString>
#include <SimpleMath.h>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace editor {

/// Rasterizes cached text into transparent images offscreen, then composites it
/// through DX11 into the current render target. Never creates or paints a QWidget.
/// Main/render-thread only. Coordinates are logical pixels relative to the viewport.
class ViewportLabels final {
public:
    ViewportLabels();
    ~ViewportLabels();
    ViewportLabels(const ViewportLabels&) = delete;
    ViewportLabels& operator=(const ViewportLabels&) = delete;

    /// Starts a fresh frame, including frames with no labels. A font/DPI change
    /// invalidates cached glyph textures. Text positions denote the baseline.
    void beginFrame(const QFont& font, qreal devicePixelRatio);
    void add(const QPointF& baseline, const QString& text, const DirectX::SimpleMath::Color& color);
    /// Call with a bound target/viewport before swap-chain Present. No back-buffer
    /// references are retained. Resources are recreated when the device changes.
    void render(ID3D11Device* device, ID3D11DeviceContext* context);
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace editor
