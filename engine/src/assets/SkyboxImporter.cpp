#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include <utility>

namespace engine {
bool SkyboxImporter::ReadCpu(const AssetDesc& desc, ID3D11Device*, CpuPayload& payload,
                            std::string& error) const {
    try {
        auto path = AssetPathResolver::ToAbsolute(desc.path);
        if (path.extension().empty()) path += L".skybox.json";
        std::string text;
        SkyboxDefinition definition;
        if (!ReadTextFile(path, text, error, 1024 * 1024) ||
            !ParseSkyboxDefinition(text, definition, error)) return false;
        payload.skybox = std::move(definition);
        payload.kind = CpuPayload::Kind::Skybox;
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool SkyboxImporter::UploadGpu(const UploadTransfer&, const CpuPayload& payload,
                              GpuPayload&, std::string& error) const {
    if (payload.kind != CpuPayload::Kind::Skybox) {
        error = "SkyboxImporter: wrong payload kind";
        return false;
    }
    return ValidateSkyboxDefinition(payload.skybox, error);
}
} // namespace engine
