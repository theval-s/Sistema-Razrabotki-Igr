#pragma once

#include <SimpleMath.h>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/engine_export.hpp>
#include <string>

namespace engine {
/// Background only: does not affect lighting, exposure or output encoding.
/// Scene JSON root `environment` is optional; each field below is optional too.
/// Omitted environment/fields use these defaults; skybox:null means no skybox.
/// References use {"id":"<uuid>","path":"<optional hint>"}; unknown fields fail.
struct EnvironmentSettings {
    AssetReference skybox;
    /// Positive yaw rotates the sky about world +Y using Matrix::CreateRotationY.
    /// Sampling applies the inverse yaw to the world-space camera ray. No wrapping.
    float yawRadians{0.0f};
    float brightness{1.0f};
    DirectX::SimpleMath::Vector3 tint{1.0f, 1.0f, 1.0f};
    /// Linear RGB; rendered directly when no ready skybox exists, without tint/brightness.
    DirectX::SimpleMath::Vector3 fallbackColor{0.0f, 0.0f, 0.0f};
    bool operator==(const EnvironmentSettings&) const = default;
};

/// All numbers finite; brightness, tint and fallbackColor components >= 0 (HDR allowed).
/// Empty skybox is allowed only with an empty path; nonempty UUIDs need not exist yet.
/// Does not resolve assets. Clears error on success.
[[nodiscard]] ENGINE_API bool ValidateEnvironmentSettings(const EnvironmentSettings& value, std::string& error);
} // namespace engine
