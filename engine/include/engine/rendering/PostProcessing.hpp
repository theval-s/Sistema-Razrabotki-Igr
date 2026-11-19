#pragma once

#include <engine/engine_export.hpp>
#include <engine/rendering/ShaderParameters.hpp>
#include <engine/rendering/RenderingDiagnostics.hpp>
#include <engine/rendering/resources/Texture.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace engine {

struct RenderingContext;
struct RenderingResources;
struct RenderWorld;

enum class PostProcessStage { BeforeTonemap, AfterTonemap, AfterUI };

// Tokens are renderer/owner scoped, never serialized, and never reused.
struct PostProcessOwner { std::uint64_t value{}; bool operator==(const PostProcessOwner&) const = default; };
struct PostProcessShader { std::uint64_t value{}; bool operator==(const PostProcessShader&) const = default; };
struct PostProcessBuffer { std::uint64_t value{}; bool operator==(const PostProcessBuffer&) const = default; };
struct PostProcessPass { std::uint64_t value{}; bool operator==(const PostProcessPass&) const = default; };
struct PostProcessTexture {
    std::uint64_t value{};
    // Nonzero for a borrowed frame texture; valid only in that execution.
    std::uint64_t execution{};
    bool operator==(const PostProcessTexture&) const = default;
};

struct PostProcessTargetDesc {
    // Zero width/height selects viewport-relative dimensions (ceil, minimum 1).
    std::uint32_t width{}, height{};
    float scale = 1.0f;
    DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
};

struct PostProcessFrame {
    PostProcessTexture inputColor, outputColor, depth, normals;
    std::uint32_t inputWidth{}, inputHeight{}, outputWidth{}, outputHeight{};
};

class PostProcessing;

/// Copyable capability, revoked when the callback returns. Engine-thread only.
/// Pixel bindings are explicit. b0-b4 and s0-s4 belong to the engine.
class ENGINE_API PostProcessContext {
public:
    void SetPixelShader(PostProcessShader shader);
    void SetRenderTarget(PostProcessTexture texture);
    void BindTexture(std::uint32_t slot, PostProcessTexture texture);
    void BindConstantBuffer(std::uint32_t slot, PostProcessBuffer buffer);
    void ClearRenderTarget(PostProcessTexture texture, const std::array<float, 4>& color = {});
    void DrawFullscreen();
    [[nodiscard]] std::array<std::uint32_t, 2> GetTextureSize(PostProcessTexture texture) const;

private:
    friend class PostProcessing;
    struct State;
    explicit PostProcessContext(const std::shared_ptr<State>& state);
    [[nodiscard]] std::shared_ptr<State> GetState() const;
    std::weak_ptr<State> state_;
};

/// Fullscreen effect resources and scheduling, independent of Lua.
/// Creation/updates validate and throw std::invalid_argument on bad authoring.
/// Native GPU failures retain RenderingException diagnostics.
class ENGINE_API PostProcessing {
public:
    using Callback = std::function<void(PostProcessContext&, const PostProcessFrame&)>;

    PostProcessing();
    ~PostProcessing();
    PostProcessing(const PostProcessing&) = delete;
    PostProcessing& operator=(const PostProcessing&) = delete;

    void Initialize(RenderingContext& context, std::uint32_t width, std::uint32_t height,
                    std::function<void(RenderingFailure)> resourceFailure = {});
    void Shutdown() noexcept;
    void Resize(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] PostProcessOwner CreateOwner();
    void ReleaseOwner(PostProcessOwner owner) noexcept;
    [[nodiscard]] bool IsOwnerAlive(PostProcessOwner owner) const noexcept;
    [[nodiscard]] PostProcessShader CreateShader(PostProcessOwner owner, const CompiledShader& shader);
    [[nodiscard]] ShaderParameters ReflectParameters(PostProcessOwner owner, PostProcessShader shader,
                                                     const std::string& name) const;
    [[nodiscard]] PostProcessBuffer CreateConstantBuffer(PostProcessOwner owner, PostProcessShader shader,
        const std::string& name, std::span<const ShaderParameterUpdate> initialValues = {});
    void SetParameters(PostProcessOwner owner, PostProcessBuffer buffer,
                       std::span<const ShaderParameterUpdate> updates);
    [[nodiscard]] const std::vector<ShaderParameterField>& GetParameterFields(
        PostProcessOwner owner, PostProcessBuffer buffer) const;
    [[nodiscard]] PostProcessTexture CreateTarget(PostProcessOwner owner, const PostProcessTargetDesc& desc);
    // Retain a read-only SRV snapshot. It survives asset unload; reload to see asset changes.
    [[nodiscard]] PostProcessTexture ImportTexture(PostProcessOwner owner, TextureHandle texture);
    [[nodiscard]] PostProcessPass AddPass(PostProcessOwner owner, std::string name, PostProcessStage stage,
                                         int order, Callback callback);
    void RemovePass(PostProcessOwner owner, PostProcessPass pass);
    void SetEnabled(PostProcessOwner owner, PostProcessPass pass, bool enabled);
    [[nodiscard]] std::string GetPassError(PostProcessOwner owner, PostProcessPass pass) const;

    // RenderingSystem lifecycle. Schedule changes become visible on BeginFrame.
    // Removal/owner retirement revoke pending callbacks immediately; native release waits for EndFrame.
    void BeginFrame();
    void EndFrame() noexcept;
    [[nodiscard]] bool HasPasses() const noexcept;
    [[nodiscard]] TextureHandle Execute(PostProcessStage stage, TextureHandle input,
                                        RenderWorld& world, RenderingResources& resources);
    [[nodiscard]] TextureHandle GetCompositionTarget();
    void CopyTo(TextureHandle input, TextureHandle output, std::uint32_t width, std::uint32_t height);

private:
    friend class PostProcessContext;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine
