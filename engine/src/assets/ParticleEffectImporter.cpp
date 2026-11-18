#include <engine/assets/AssetPipeline.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include <utility>

namespace engine {
bool ParticleEffectImporter::ReadCpu(const AssetDesc& desc, ID3D11Device*, CpuPayload& payload,
                                    std::string& error) const {
    try {
        auto path = AssetPathResolver::ToAbsolute(desc.path);
        if (path.extension().empty()) path += L".particle-effect.json";
        std::string text;
        ParticleEffectDefinition definition;
        if (!ReadTextFile(path, text, error, 1024 * 1024) ||
            !ParseParticleEffectDefinition(text, definition, error)) return false;
        payload.particleEffect = std::move(definition);
        payload.kind = CpuPayload::Kind::ParticleEffect;
        error.clear();
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
bool ParticleEffectImporter::UploadGpu(const UploadTransfer&, const CpuPayload& payload,
                                      GpuPayload&, std::string& error) const {
    if (payload.kind != CpuPayload::Kind::ParticleEffect) {
        error = "ParticleEffectImporter: wrong payload kind";
        return false;
    }
    error.clear();
    return true;
}
} // namespace engine
