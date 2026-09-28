#pragma once

#include <QWidget>

#include <engine/core/Surface.hpp>

namespace editor {

/// The area the engine renders into. Owns a native window (HWND) that is
/// handed to the engine as its NativeSurface.
///
/// Until a rendering subsystem exists this just paints a placeholder. Once
/// the renderer presents into our HWND, set Qt::WA_PaintOnScreen and return
/// nullptr from paintEngine() so Qt stops painting over the swap chain.
class ViewportWidget : public QWidget {
    Q_OBJECT

public:
    explicit ViewportWidget(QWidget* parent = nullptr);

    [[nodiscard]] engine::NativeSurface surface();

signals:
    /// Size in physical pixels.
    void surfaceResized(quint32 width, quint32 height);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    [[nodiscard]] QSize pixelSize() const;
};

}  // namespace editor
