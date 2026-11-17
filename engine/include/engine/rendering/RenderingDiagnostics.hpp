#pragma once

#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine {

enum class RenderingLifecycle { Uninitialized, Ready, SurfaceUnavailable, Failed };

struct ShaderDiagnosticContext {
    std::string stage; // "vertex", "pixel", or "geometry"
    std::string entryPoint; // Resolved default, never an empty shorthand.
    std::string target; // e.g. "vs_5_0"
    std::vector<std::pair<std::string, std::string>> defines;
};

/// Owned backend-neutral data; text is UTF-8, paths retain native filesystem encoding.
struct RenderingFailure {
    std::string operation; // Failing leaf operation, e.g. CreateTexture2D or D3DCompile.
    std::string pass; // Empty outside a pass; supplied by the pass dispatch boundary.
    std::string summary;
    std::string detail; // Full compiler/read/native detail, never reconstructed from logs.
    std::filesystem::path path; // Empty for resources without a source path.
    std::optional<std::int64_t> nativeError; // Signed HRESULT on DX11.
    std::optional<std::int64_t> deviceRemovalReason; // Separate from initiating HRESULT.
    std::optional<ShaderDiagnosticContext> shader;
};

/// A copy owns all its data and survives shutdown or destruction of the renderer.
/// Renderer access is engine-thread-only; copying does not make concurrent access safe.
struct RenderingDiagnostics {
    RenderingLifecycle lifecycle = RenderingLifecycle::Uninitialized;
    // Present exactly when lifecycle == Failed, including after failure cleanup/shutdown.
    std::optional<RenderingFailure> failure;
};

/// Narrow transport for rendering operations, not a global engine exception policy.
/// Catch at renderer lifecycle or asset importer boundaries. what() is only a summary;
/// GetFailure() is authoritative. Cleanup must never replace the initiating failure.
class RenderingException final : public std::exception {
public:
    explicit RenderingException(RenderingFailure failure) : failure_(std::move(failure)) {}
    [[nodiscard]] const char* what() const noexcept override { return failure_.summary.c_str(); }
    [[nodiscard]] const RenderingFailure& GetFailure() const noexcept { return failure_; }

private:
    RenderingFailure failure_;
};

} // namespace engine
