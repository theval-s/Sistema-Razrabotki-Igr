#include <editor/viewport_labels.hpp>

#include <engine/core/Log.hpp>
#include <CommonStates.h>
#include <SpriteBatch.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QRawFont>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <vector>

namespace editor {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t MaxBytes = 32 * 1024 * 1024;
constexpr int MaxDimension = 8192;
constexpr std::size_t MaxLabels = 1024;
constexpr qsizetype MaxEntries = 256;
}

struct ViewportLabels::Impl {
    struct Label {
        QPointF baseline;
        QString text;
        DirectX::SimpleMath::Color color;
    };
    struct Texture {
        ComPtr<ID3D11ShaderResourceView> view;
        QPointF offset; // Physical pixels from the text baseline, including padding.
        std::size_t bytes = 0;
        std::uint64_t used = 0;
    };
    QFont font;
    QFont rasterFont;
    qreal dpr = 0;
    std::vector<Label> labels;
    QHash<QString, Texture> cache;
    std::size_t cacheBytes = 0;
    std::uint64_t frame = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::unique_ptr<DirectX::SpriteBatch> batch;
    std::unique_ptr<DirectX::CommonStates> states;
    bool reportedFailure = false;

    void report(const char* message) {
        if (!reportedFailure) {
            engine::log::Render().error("ViewportLabels: {}", message);
            reportedFailure = true;
        }
    }

    void resetGpu() {
        cache.clear();
        cacheBytes = 0;
        batch.reset();
        states.reset();
        context.Reset();
        device.Reset();
    }

    Texture makeTexture(const QString& text) {
        Texture result;
        if (!QRawFont::fromFont(rasterFont).isValid()) {
            report("no usable font for viewport labels; check the Qt platform font database");
            return result;
        }
        // The raster font has a logical pixel size, independent of QImage's DPI.
        const QRectF bounds = QFontMetricsF(rasterFont).boundingRect(text);
        if (bounds.isEmpty()) return result;
        const qreal left = std::floor(bounds.left() * dpr) - 2;
        const qreal top = std::floor(bounds.top() * dpr) - 2;
        const qreal width = std::ceil(bounds.right() * dpr) + 2 - left;
        const qreal height = std::ceil(bounds.bottom() * dpr) + 2 - top;
        if (!std::isfinite(width) || !std::isfinite(height) ||
            width <= 0 || height <= 0 || width > MaxDimension || height > MaxDimension ||
            width * height * 4 > MaxBytes) {
            report("label exceeds texture resource limits");
            return result;
        }
        QImage image(static_cast<int>(width), static_cast<int>(height),
                     QImage::Format_RGBA8888_Premultiplied);
        if (image.isNull()) {
            report("could not allocate label image");
            return result;
        }
        image.fill(Qt::transparent);
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::TextAntialiasing);
            painter.setFont(rasterFont);
            painter.setPen(Qt::white);
            painter.scale(dpr, dpr);
            painter.drawText(QPointF(-left / dpr, -top / dpr), text);
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = image.width();
        desc.Height = image.height();
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = image.constBits();
        data.SysMemPitch = static_cast<UINT>(image.bytesPerLine());
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device->CreateTexture2D(&desc, &data, texture.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = device->CreateShaderResourceView(texture.Get(), nullptr, result.view.GetAddressOf());
        if (FAILED(hr)) {
            report("D3D11 label texture/SRV allocation failed");
            return {};
        }
        result.offset = {left, top};
        result.bytes = static_cast<std::size_t>(image.sizeInBytes());
        return result;
    }

    void evictFor(std::size_t bytes) {
        while (!cache.isEmpty() && (cache.size() >= MaxEntries || cacheBytes + bytes > MaxBytes)) {
            auto oldest = cache.begin();
            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->used < oldest->used) oldest = it;
            cacheBytes -= oldest->bytes;
            cache.erase(oldest);
        }
    }
};

ViewportLabels::ViewportLabels() : impl_(std::make_unique<Impl>()) {}
ViewportLabels::~ViewportLabels() = default;

void ViewportLabels::beginFrame(const QFont& font, qreal devicePixelRatio) {
    auto& p = *impl_;
    p.labels.clear();
    ++p.frame;
    const qreal dpr = std::isfinite(devicePixelRatio) && devicePixelRatio > 0 &&
        devicePixelRatio <= 16 ? devicePixelRatio : 0;
    if (p.font != font || p.dpr != dpr) {
        p.cache.clear();
        p.cacheBytes = 0;
        p.font = font;
        p.rasterFont = font;
        // Preserve explicit pixels instead of asking QFontInfo for a resolved
        // size (which can return -1 when the platform font database is empty).
        // Resolve point sizes using the viewport font metrics' logical DPI.
        const int pixels = font.pixelSize() > 0 ? font.pixelSize() :
            qRound(font.pointSizeF() * QFontMetricsF(font).fontDpi() / 72.0);
        p.rasterFont.setPixelSize((std::max)(1, pixels));
        p.dpr = dpr;
    }
}

void ViewportLabels::add(const QPointF& baseline, const QString& text,
                         const DirectX::SimpleMath::Color& color) {
    auto& p = *impl_;
    if (p.dpr <= 0 || text.isEmpty() || text.size() > 16384 || p.labels.size() >= MaxLabels ||
        !std::isfinite(baseline.x()) || !std::isfinite(baseline.y()) ||
        std::abs(baseline.x() * p.dpr) > 1e7 || std::abs(baseline.y() * p.dpr) > 1e7 ||
        !std::isfinite(color.x) || !std::isfinite(color.y) ||
        !std::isfinite(color.z) || !std::isfinite(color.w)) return;
    p.labels.push_back({baseline, text, color});
}

void ViewportLabels::render(ID3D11Device* device, ID3D11DeviceContext* context) {
    auto& p = *impl_;
    if (!device || !context) return;
    if (p.device.Get() != device || p.context.Get() != context) {
        p.resetGpu();
        p.device = device;
        p.context = context;
    }
    if (p.labels.empty() || p.dpr <= 0) return;
    try {
        ComPtr<ID3D11Device> contextDevice;
        context->GetDevice(contextDevice.GetAddressOf());
        if (contextDevice.Get() != device) {
            p.report("device/context mismatch");
            return;
        }
        if (!p.batch) {
            p.states = std::make_unique<DirectX::CommonStates>(device);
            p.batch = std::make_unique<DirectX::SpriteBatch>(context);
        }
        struct Draw { Impl::Texture texture; const Impl::Label* label; };
        std::vector<Draw> draws;
        draws.reserve(p.labels.size());
        std::size_t frameBytes = 0;
        // Prepare everything before Begin. Each draw owns its SRV even if a
        // subsequent insertion evicts it from the bounded cache.
        for (const auto& label : p.labels) {
            auto it = p.cache.find(label.text);
            if (it == p.cache.end()) {
                auto texture = p.makeTexture(label.text);
                if (!texture.view) continue;
                p.evictFor(texture.bytes);
                p.cacheBytes += texture.bytes;
                it = p.cache.insert(label.text, std::move(texture));
            }
            it->used = p.frame;
            if (frameBytes + it->bytes > MaxBytes) break;
            frameBytes += it->bytes;
            draws.push_back({*it, &label});
        }
        if (draws.empty()) return;
        // AlphaBlend is ONE / INV_SRC_ALPHA, matching Qt's premultiplied image.
        p.batch->Begin(DirectX::SpriteSortMode_Deferred, p.states->AlphaBlend(),
                       p.states->LinearClamp(), p.states->DepthNone(), p.states->CullNone());
        for (const auto& draw : draws) {
            const auto& label = *draw.label;
            const float x = static_cast<float>(label.baseline.x() * p.dpr + draw.texture.offset.x());
            const float y = static_cast<float>(label.baseline.y() * p.dpr + draw.texture.offset.y());
            const float a = std::clamp(label.color.w, 0.0f, 1.0f);
            const auto color = DirectX::XMVectorSet(std::clamp(label.color.x, 0.0f, 1.0f) * a,
                std::clamp(label.color.y, 0.0f, 1.0f) * a,
                std::clamp(label.color.z, 0.0f, 1.0f) * a, a);
            p.batch->Draw(draw.texture.view.Get(), DirectX::XMFLOAT2(x + static_cast<float>(p.dpr),
                y + static_cast<float>(p.dpr)), nullptr, DirectX::XMVectorSet(0, 0, 0, a));
            p.batch->Draw(draw.texture.view.Get(), DirectX::XMFLOAT2(x, y), nullptr, color);
        }
        p.batch->End();
        // Release the last SRV binding as well as our own cache on clear/shutdown.
        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(0, 1, &empty);
    } catch (const std::exception& error) {
        p.report(error.what());
        // A failed Begin/End may leave SpriteBatch inside a batch. Recreate next time.
        p.resetGpu();
    } catch (...) {
        p.report("unknown exception while compositing viewport labels");
        p.resetGpu();
    }
}

void ViewportLabels::clear() {
    impl_->labels.clear();
    impl_->resetGpu();
}

} // namespace editor
