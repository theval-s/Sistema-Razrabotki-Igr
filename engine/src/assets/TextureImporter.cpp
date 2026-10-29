#include <format>
#include <utility>
#include <vector>

#include <ole2.h>

#include <DDSTextureLoader.h>
#include <WICTextureLoader.h>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {
namespace {

// DirectXTK caches its WIC factory process-wide. Releasing the last MTA reference
// after a single decode tears down that apartment and leaves the cached factory
// pointing at dead COM state. Keep the MTA alive for the lifetime of this module,
// including periods when no worker is currently decoding and engine restarts.
class WicApartment {
public:
    WicApartment() : ok_(SUCCEEDED(CoIncrementMTAUsage(&cookie_))) {}
    ~WicApartment() { if (ok_) CoDecrementMTAUsage(cookie_); }
    WicApartment(const WicApartment&) = delete;
    WicApartment& operator=(const WicApartment&) = delete;
    explicit operator bool() const noexcept { return ok_; }
private:
    CO_MTA_USAGE_COOKIE cookie_ = nullptr;
    bool ok_;
};

/// WIC is COM and COM is initialized per thread, so a scheduler worker that nobody initialized
/// would fail every decode. CoInitializeEx returns S_FALSE when the thread already was, meaning
/// this call took a refcount, so the matching CoUninitialize stays balanced either way.
class ScopedCom {
public:
    ScopedCom() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {
    }

    ~ScopedCom() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; // an existing STA is usable too
    }

private:
    HRESULT result_;
};

} // namespace

bool TextureImporter::ReadCpu(const AssetDesc& desc, ID3D11Device* device, CpuPayload& payload,
                              std::string& error) const {
    if (device == nullptr) {
        error = "no device yet: a texture cannot be decoded before the rendering subsystem is up";
        return false;
    }

    const auto path = AssetPathResolver::ResolveTexture(desc);

    std::vector<uint8_t> fileBytes;
    if (!ReadWholeFileInto(path, fileBytes, error)) {
        return false;
    }

    payload = CpuPayload{};
    payload.kind = CpuPayload::Kind::Texture;

    ComPtr<ID3D11ShaderResourceView> view;
    HRESULT res = E_FAIL;
    if (_wcsicmp(path.extension().c_str(), L".dds") == 0) {
        res = DirectX::CreateDDSTextureFromMemory(
            device, fileBytes.data(), fileBytes.size(), nullptr, view.GetAddressOf());
    } else {
        static const WicApartment apartment;
        const ScopedCom com;
        if (!apartment || !com) {
            error = std::format("cannot initialize COM on this thread to decode '{}'", ToUtf8(path));
            return false;
        }
        // The device-only overload: with a context DirectXTK would defer the mip chain to a
        // GenerateMips call, which is main-thread work again. Decoding and building the chain
        // here is the whole point of the CPU phase for a texture.
        res = DirectX::CreateWICTextureFromMemoryEx(
            device,
            fileBytes.data(), fileBytes.size(),
            0,
            D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE,
            0,
            0,
            DirectX::WIC_LOADER_FORCE_SRGB,
            nullptr,
            view.GetAddressOf());
    }

    if (FAILED(res) || !view) {
        error = std::format("decoding '{}' failed with {:08x}", ToUtf8(path), static_cast<unsigned>(res));
        return false;
    }

    payload.texture.view = std::move(view);
    return true;
}

bool TextureImporter::UploadGpu(const UploadTransfer& transfer, const CpuPayload& payload,
                                GpuPayload& result, std::string& error) const {
    if (payload.kind != CpuPayload::Kind::Texture) {
        error = "TextureImporter: payload is not a decoded texture";
        return false;
    }

    // A copy rather than a move: the payload is const here and stays readable, so a record that
    // is recycled and re-uploaded still has what it needs.
    ComPtr<ID3D11ShaderResourceView> view = payload.texture.view;
    result.texture = transfer.resources->RegisterTexture(Texture(std::move(view)));
    return true;
}

} // namespace engine
