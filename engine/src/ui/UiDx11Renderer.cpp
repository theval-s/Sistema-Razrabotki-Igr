#include "UiDx11Renderer.hpp"
#include "UiInterfaces.hpp"
#include <RmlUi_Renderer_DX11.h>
#include <engine/rendering/RenderingContext.hpp>
#include <engine/core/Log.hpp>
#include <wincodec.h>
#include <wrl/client.h>
#include <limits>
#include <stdexcept>
#include <vector>

namespace engine {
namespace {
using Microsoft::WRL::ComPtr;

class UiDx11Renderer final : public RenderInterface_DX11 {
public:
    UiDx11Renderer(ID3D11Device* device, ComPtr<ID3D11DeviceContext1> context,
                   ComPtr<ID3DDeviceContextState> state)
        : RenderInterface_DX11(device), context_(std::move(context)), cleanState_(std::move(state)) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninitializeCom_ = SUCCEEDED(initialized);
        // RPC_E_CHANGED_MODE means the host already initialized COM in another apartment.
        if (SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE)
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(factory_.GetAddressOf()));
    }
    ~UiDx11Renderer() override {
        ReleaseSurface();
        factory_.Reset();
        if (uninitializeCom_) CoUninitialize();
    }
    bool Ready() const { return IsValid() && factory_; }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        std::filesystem::path path;
        std::string logical;
        if (!ui_detail::ResolveAsset(source, path, logical)) return TextureError(source, "outside asset roots");
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) ||
            FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
            FAILED(factory_->CreateFormatConverter(converter.GetAddressOf())) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPRGBA,
                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            return TextureError(source, "WIC could not decode the image");
        UINT width = 0, height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || !width || !height ||
            width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            std::uint64_t(width) * height * 4 > (std::numeric_limits<UINT>::max)())
            return TextureError(source, "unsupported image dimensions");
        std::vector<Rml::byte> pixels(std::size_t(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
            return TextureError(source, "WIC pixel conversion failed");
        // Match Rml vertex colors, font atlases and the upstream backend: display-encoded
        // RGBA, premultiplied in that space, sampled as UNORM (never a forced-sRGB SRV).
        // The engine's tone-mapped presentation target is also UNORM. UI blends after it.
        const Rml::Vector2i size{static_cast<int>(width), static_cast<int>(height)};
        auto texture = GenerateTexture(pixels, size);
        if (texture) dimensions = size;
        return texture;
    }

    void Begin(RenderingContext& engineContext, int width, int height) {
        if (previousState_ || target_) throw std::logic_error("UI overlay frames cannot nest");
        engineContext.GetDeviceContext()->OMGetRenderTargets(1, target_.GetAddressOf(), nullptr);
        if (!target_) throw std::runtime_error("UI overlay has no final scene target");
        // Swap the ENTIRE D3D11 state, not only the partial state saved upstream. This
        // isolates GS/HS/DS, all SRVs and constant buffers, targets, viewports and scissors.
        // Restoring the original state also preserves RenderingContext's input-layout cache.
        context_->SwapDeviceContextState(cleanState_.Get(), previousState_.GetAddressOf());
        SetViewport(width, height);
        frameActive_ = true;
        BeginFrame();
    }
    void End() {
        if (!previousState_) return;
        EndFrame(target_.Get());
        frameActive_ = false;
        ReleaseSurface();
    }
    void ReleaseSurface() noexcept {
        if (previousState_) {
            if (frameActive_) { CancelFrame(); frameActive_ = false; }
            // The clean context-state object must never retain a back-buffer reference.
            context_->ClearState();
            context_->SwapDeviceContextState(previousState_.Get(), nullptr);
            previousState_.Reset();
        }
        target_.Reset();
    }
private:
    static Rml::TextureHandle TextureError(const std::string& source, const char* reason) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "UI image '%s': %s", source.c_str(), reason);
        return 0;
    }
    ComPtr<ID3D11DeviceContext1> context_;
    ComPtr<ID3DDeviceContextState> cleanState_, previousState_;
    ComPtr<ID3D11RenderTargetView> target_; // only between Begin/End, never across a frame
    ComPtr<IWICImagingFactory> factory_;
    bool uninitializeCom_ = false;
    bool frameActive_ = false;
};
}

std::unique_ptr<Rml::RenderInterface> CreateUiDx11Renderer(RenderingContext& context, std::string& error) {
    ComPtr<ID3D11Device1> device;
    ComPtr<ID3D11DeviceContext1> immediate;
    auto* rawDevice = context.GetDevice();
    auto* rawContext = context.GetDeviceContext();
    if (!rawDevice || !rawContext || FAILED(rawDevice->QueryInterface(IID_PPV_ARGS(device.GetAddressOf()))) ||
        FAILED(rawContext->QueryInterface(IID_PPV_ARGS(immediate.GetAddressOf())))) {
        error = "RmlUi DX11 requires ID3D11Device1 and ID3D11DeviceContext1";
        return {};
    }
    const auto level = device->GetFeatureLevel();
    ComPtr<ID3DDeviceContextState> state;
    if (FAILED(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                 __uuidof(ID3D11Device), nullptr, state.GetAddressOf()))) {
        error = "D3D11.1 device-context state isolation is unavailable for UI";
        return {};
    }
    UINT samples = 0;
    if (FAILED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, NUM_MSAA_SAMPLES, &samples)) || !samples) {
        error = "RmlUi DX11 layer multisampling is unsupported by this device";
        return {};
    }
    auto renderer = std::make_unique<UiDx11Renderer>(rawDevice, std::move(immediate), std::move(state));
    if (!renderer->Ready()) { error = "RmlUi DX11 shaders/resources or WIC initialization failed"; return {}; }
    return renderer;
}
void UiDx11BeginFrame(Rml::RenderInterface& renderer, RenderingContext& context, int width, int height) {
    static_cast<UiDx11Renderer&>(renderer).Begin(context, width, height);
}
void UiDx11EndFrame(Rml::RenderInterface& renderer, RenderingContext&) {
    static_cast<UiDx11Renderer&>(renderer).End();
}
void UiDx11ReleaseSurface(Rml::RenderInterface& renderer) noexcept {
    static_cast<UiDx11Renderer&>(renderer).ReleaseSurface();
}
}
