#include <engine/rendering/PostProcessing.hpp>

#include <engine/core/Log.hpp>
#include <engine/rendering/RenderingContext.hpp>
#include <engine/rendering/RenderingDiagnostics.hpp>
#include <engine/rendering/RenderingResources.hpp>
#include <engine/rendering/RenderWorld.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine {
namespace {
std::uint64_t Token() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    do {
        if (value == (std::numeric_limits<std::uint64_t>::max)())
            throw std::overflow_error("Postprocess token space exhausted.");
    } while (!next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return value;
}

std::size_t StageIndex(PostProcessStage stage) {
    switch (stage) {
    case PostProcessStage::BeforeTonemap: return 0;
    case PostProcessStage::AfterTonemap: return 1;
    case PostProcessStage::AfterUI: return 2;
    }
    throw std::invalid_argument("Unknown postprocess stage.");
}

bool TargetFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R8_UNORM;
}

TextureDesc TargetDescription(const PostProcessTargetDesc& desc, std::uint32_t width, std::uint32_t height) {
    if (!TargetFormat(desc.format) || !std::isfinite(desc.scale) || desc.scale <= 0 ||
        ((desc.width == 0) != (desc.height == 0)))
        throw std::invalid_argument("Invalid postprocess target format, dimensions, or scale.");
    const double w = desc.width ? desc.width : (std::max)(1.0, std::ceil(double(width) * desc.scale));
    const double h = desc.height ? desc.height : (std::max)(1.0, std::ceil(double(height) * desc.scale));
    if (w > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || h > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw std::invalid_argument("Postprocess target exceeds the maximum texture dimensions.");
    return {.width = static_cast<std::uint32_t>(w), .height = static_cast<std::uint32_t>(h),
            .format = desc.format, .usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget};
}

std::array<std::uint32_t, 2> TextureSize(const Texture& texture) {
    if (auto* resource = texture.GetTexture()) {
        D3D11_TEXTURE2D_DESC desc{};
        resource->GetDesc(&desc);
        return {desc.Width, desc.Height};
    }
    // Imported assets may only store an SRV.
    if (texture.HasShaderResourceView()) {
        ComPtr<ID3D11Resource> resource;
        texture.GetSRV()->GetResource(&resource);
        ComPtr<ID3D11Texture2D> native;
        if (SUCCEEDED(resource.As(&native))) {
            D3D11_TEXTURE2D_DESC desc{};
            native->GetDesc(&desc);
            return {desc.Width, desc.Height};
        }
    }
    throw std::invalid_argument("Postprocess texture must be a live 2D texture.");
}
}

struct PostProcessing::Impl {
    struct ShaderRecord { PostProcessOwner owner; ShaderHandle handle; CompiledShader compiled; };
    struct BufferRecord {
        PostProcessOwner owner;
        BufferHandle handle;
        PostProcessShader shader;
        ShaderParameters parameters;
        bool dirty = true;
    };
    struct TextureRecord {
        PostProcessOwner owner;
        TextureHandle handle;
        PostProcessTargetDesc desc;
        bool imported = false;
    };
    struct PassRecord {
        PostProcessOwner owner;
        std::string name;
        PostProcessStage stage;
        int order;
        std::uint64_t sequence;
        Callback callback;
        bool enabled = true;
        std::string error;
    };
    RenderingContext* context{};
    std::uint32_t width{}, height{};
    bool frameActive{};
    bool failed{};
    std::function<void(RenderingFailure)> resourceFailure;
    std::unordered_set<std::uint64_t> owners;
    std::unordered_set<std::uint64_t> tonemappingBypassOwners;
    std::unordered_map<std::uint64_t, ShaderRecord> shaders;
    std::unordered_map<std::uint64_t, BufferRecord> buffers;
    std::unordered_map<std::uint64_t, TextureRecord> textures;
    std::unordered_map<std::uint64_t, std::shared_ptr<PassRecord>> passes;
    std::array<std::vector<std::shared_ptr<PassRecord>>, 3> schedule;
    ShaderHandle fullscreenVS, copyPS;
    TextureHandle composition;
    std::array<TextureHandle, 2> hdrTargets{}, colorTargets{};

    void RequireOwner(PostProcessOwner owner) const {
        if (!context || failed || !owner.value || !owners.contains(owner.value))
            throw std::invalid_argument("Postprocess owner is no longer alive.");
    }
    void RequireSetup(PostProcessOwner owner) const {
        RequireOwner(owner);
        if (frameActive) throw std::invalid_argument("Create postprocess resources and passes outside rendering callbacks.");
    }
    template<class Map, class Handle>
    static auto& Owned(Map& map, Handle handle, PostProcessOwner owner) {
        auto found = map.find(handle.value);
        if (found == map.end() || found->second.owner != owner)
            throw std::invalid_argument("Invalid, expired, or foreign postprocess resource.");
        return found->second;
    }
    const Texture& TextureFor(TextureHandle handle) const {
        const auto* texture = context->GetResourceManager().TryGetTexture(handle);
        if (!texture) throw std::invalid_argument("Postprocess texture is no longer available.");
        return *texture;
    }
    TextureHandle CreateTarget(const PostProcessTargetDesc& desc) {
        return context->GetResourceManager().CreateTexture(TargetDescription(desc, width, height));
    }
    void ReleaseTargets() noexcept {
        if (!context) return;
        auto& manager = context->GetResourceManager();
        manager.DestroyTexture(composition);
        composition = {};
        for (auto* targets : {&hdrTargets, &colorTargets})
            for (auto& target : *targets) { manager.DestroyTexture(target); target = {}; }
    }
    void Collect() noexcept {
        if (!context || frameActive) return;
        auto& manager = context->GetResourceManager();
        std::erase_if(shaders, [&](auto& pair) {
            if (owners.contains(pair.second.owner.value)) return false;
            manager.DestroyShader(pair.second.handle);
            return true;
        });
        std::erase_if(buffers, [&](auto& pair) {
            if (owners.contains(pair.second.owner.value)) return false;
            manager.DestroyBuffer(pair.second.handle);
            return true;
        });
        std::erase_if(textures, [&](auto& pair) {
            if (owners.contains(pair.second.owner.value)) return false;
            manager.DestroyTexture(pair.second.handle);
            return true;
        });
    }
    void Upload(BufferRecord& buffer) {
        if (!buffer.dirty) return;
        context->UpdateCBufferBytes(buffer.handle, buffer.parameters.Bytes());
        buffer.dirty = false;
    }
    void ReportResourceFailure(const RenderingException& error) {
        failed = true;
        if (resourceFailure) {
            auto failure = error.GetFailure();
            if (failure.pass.empty()) failure.pass = "PostProcessing.Setup";
            resourceFailure(std::move(failure));
        }
    }
    void FullscreenState() {
        context->SetDepthState(DepthState::Disabled);
        context->SetBlendState(BlendState::Opaque);
        context->SetRasterizerState(RasterizerState::CullNone);
        context->SetShader(fullscreenVS);
        context->GetDeviceContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->BindSamplers();
    }
};

struct PostProcessContext::State {
    PostProcessing::Impl* system{};
    PostProcessOwner owner;
    std::uint64_t execution{};
    std::array<TextureHandle, 4> frameTextures;
    TextureHandle target;
    PostProcessShader shader;
    std::array<TextureHandle, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> sampled{};
    std::array<PostProcessBuffer, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers{};
    bool wroteOutput{};
    std::exception_ptr nativeFailure;

    TextureHandle Resolve(PostProcessTexture texture, bool writable = false) const {
        system->RequireOwner(owner);
        TextureHandle handle;
        if (texture.execution) {
            if (texture.execution != execution || texture.value < 1 || texture.value > frameTextures.size())
                throw std::invalid_argument("Borrowed postprocess texture has expired.");
            if (writable && texture.value != 2)
                throw std::invalid_argument("Only outputColor and pass-owned targets are writable.");
            handle = frameTextures[static_cast<std::size_t>(texture.value - 1)];
        } else {
            const auto& record = PostProcessing::Impl::Owned(system->textures, texture, owner);
            if (writable && record.imported) throw std::invalid_argument("Imported asset textures are read-only.");
            handle = record.handle;
        }
        const auto& resource = system->TextureFor(handle);
        if (writable ? !resource.HasRenderTargetView() : !resource.HasShaderResourceView())
            throw std::invalid_argument("Postprocess texture does not support the requested binding.");
        return handle;
    }
};

PostProcessContext::PostProcessContext(const std::shared_ptr<State>& state) : state_(state) {}
std::shared_ptr<PostProcessContext::State> PostProcessContext::GetState() const {
    auto state = state_.lock();
    if (!state) throw std::invalid_argument("Postprocess context is only valid inside its execute callback.");
    state->system->RequireOwner(state->owner);
    return state;
}

void PostProcessContext::SetPixelShader(PostProcessShader shader) {
    const auto state = GetState();
    const auto& record = PostProcessing::Impl::Owned(state->system->shaders, shader, state->owner);
    state->system->context->SetShader(record.handle);
    state->shader = shader;
}

void PostProcessContext::SetRenderTarget(PostProcessTexture texture) {
    const auto state = GetState();
    const auto target = state->Resolve(texture, true);
    auto& context = *state->system->context;
    for (std::size_t i = 0; i < state->sampled.size(); ++i) {
        if (state->sampled[i] == target) {
            context.SetShaderResources(BindSlot(static_cast<std::uint8_t>(i), ShaderStage::Pixel),
                                       static_cast<ID3D11ShaderResourceView*>(nullptr));
            state->sampled[i] = {};
        }
    }
    context.SetRenderTargets({target}, {});
    const auto size = TextureSize(state->system->TextureFor(target));
    context.SetViewport(size[0], size[1]);
    state->target = target;
}

void PostProcessContext::BindTexture(std::uint32_t slot, PostProcessTexture texture) {
    const auto state = GetState();
    if (slot >= state->sampled.size()) throw std::invalid_argument("Pixel texture slot must be in [0, 127].");
    const auto handle = state->Resolve(texture);
    if (handle == state->target)
        throw std::invalid_argument("A fullscreen draw cannot sample its render target. Use a separate target.");
    state->system->context->SetShaderResources(BindSlot(static_cast<std::uint8_t>(slot), ShaderStage::Pixel), handle);
    state->sampled[slot] = handle;
}

void PostProcessContext::BindConstantBuffer(std::uint32_t slot, PostProcessBuffer buffer) {
    const auto state = GetState();
    if (slot < 5 || slot >= state->buffers.size())
        throw std::invalid_argument("Custom constant buffers use pixel slots b5 through b13; b0-b4 are engine-owned.");
    auto& record = PostProcessing::Impl::Owned(state->system->buffers, buffer, state->owner);
    if (slot != record.parameters.Slot())
        throw std::invalid_argument("Constant-buffer slot does not match its HLSL register.");
    try {
        state->system->Upload(record);
        state->system->context->BindCBuffer(BindSlot(static_cast<std::uint8_t>(slot), ShaderStage::Pixel), record.handle);
    } catch (const RenderingException&) {
        state->nativeFailure = std::current_exception();
        throw;
    }
    state->buffers[slot] = buffer;
}

void PostProcessContext::ClearRenderTarget(PostProcessTexture texture, const std::array<float, 4>& color) {
    const auto state = GetState();
    if (!std::all_of(color.begin(), color.end(), [](float v) { return std::isfinite(v); }))
        throw std::invalid_argument("Clear color must be finite.");
    const auto handle = state->Resolve(texture, true);
    // Clearing is a write as well: explicitly detach aliases from the input slots.
    for (std::size_t i = 0; i < state->sampled.size(); ++i) {
        if (state->sampled[i] == handle) {
            state->system->context->SetShaderResources(BindSlot(static_cast<std::uint8_t>(i), ShaderStage::Pixel),
                                                      static_cast<ID3D11ShaderResourceView*>(nullptr));
            state->sampled[i] = {};
        }
    }
    state->system->context->ClearRenderTargetView(handle, color);
    if (handle == state->frameTextures[1]) state->wroteOutput = true;
}

void PostProcessContext::DrawFullscreen() {
    const auto state = GetState();
    if (!state->target) throw std::invalid_argument("SetRenderTarget is required before DrawFullscreen.");
    const auto& shader = PostProcessing::Impl::Owned(state->system->shaders, state->shader, state->owner);
    for (const auto& binding : shader.compiled.bindings) {
        if (binding.type == D3D_SIT_TEXTURE && !state->sampled.at(binding.slot))
            throw std::invalid_argument("Missing postprocess texture binding: " + binding.name);
        if (binding.type == D3D_SIT_CBUFFER && binding.slot >= 5) {
            const auto& buffer = PostProcessing::Impl::Owned(state->system->buffers, state->buffers.at(binding.slot), state->owner);
            if (buffer.shader != state->shader)
                throw std::invalid_argument("Constant buffer must be reflected from the active pixel shader: " + binding.name);
        }
    }
    try {
        for (const auto buffer : state->buffers) {
            if (buffer.value) state->system->Upload(PostProcessing::Impl::Owned(state->system->buffers, buffer, state->owner));
        }
        state->system->context->RawDraw(6, 0);
    } catch (const RenderingException&) {
        state->nativeFailure = std::current_exception();
        throw;
    }
    if (state->target == state->frameTextures[1]) state->wroteOutput = true;
}

std::array<std::uint32_t, 2> PostProcessContext::GetTextureSize(PostProcessTexture texture) const {
    const auto state = GetState();
    return TextureSize(state->system->TextureFor(state->Resolve(texture)));
}

PostProcessing::PostProcessing() : impl_(std::make_unique<Impl>()) {}
PostProcessing::~PostProcessing() { Shutdown(); }

void PostProcessing::Initialize(RenderingContext& context, std::uint32_t width, std::uint32_t height,
                                std::function<void(RenderingFailure)> resourceFailure) {
    Shutdown();
    impl_->context = &context;
    impl_->width = width;
    impl_->height = height;
    impl_->failed = false;
    impl_->resourceFailure = std::move(resourceFailure);
    try {
        impl_->fullscreenVS = context.GetResourceManager().CompileShader(EngineShader::PresentVS);
        impl_->copyPS = context.GetResourceManager().CompileShader(EngineShader::PostProcessCopyPS);
    } catch (...) {
        Shutdown();
        throw;
    }
}

void PostProcessing::Shutdown() noexcept {
    if (!impl_->context) return;
    impl_->context->ClearState();
    impl_->frameActive = false;
    for (auto& schedule : impl_->schedule) schedule.clear();
    impl_->passes.clear();
    impl_->owners.clear();
    impl_->tonemappingBypassOwners.clear();
    impl_->Collect();
    impl_->ReleaseTargets();
    auto& manager = impl_->context->GetResourceManager();
    manager.DestroyShader(impl_->fullscreenVS);
    manager.DestroyShader(impl_->copyPS);
    impl_->fullscreenVS = impl_->copyPS = {};
    impl_->context = nullptr;
    impl_->resourceFailure = {};
}

void PostProcessing::Resize(std::uint32_t width, std::uint32_t height) {
    if (!impl_->context || impl_->frameActive || !width || !height)
        throw std::invalid_argument("Postprocess resize requires an idle, initialized renderer and nonzero dimensions.");
    impl_->width = width;
    impl_->height = height;
    impl_->ReleaseTargets();
    auto& manager = impl_->context->GetResourceManager();
    for (auto& [id, target] : impl_->textures) {
        if (target.imported || target.desc.width) continue;
        const auto replacement = impl_->CreateTarget(target.desc);
        manager.DestroyTexture(target.handle);
        target.handle = replacement;
    }
}

PostProcessOwner PostProcessing::CreateOwner() {
    if (!impl_->context || impl_->failed || impl_->frameActive) throw std::invalid_argument("Postprocess registration requires an idle, ready renderer.");
    const auto token = Token();
    impl_->owners.insert(token);
    return {token};
}
void PostProcessing::ReleaseOwner(PostProcessOwner owner) noexcept {
    if (!impl_->owners.erase(owner.value)) return;
    impl_->tonemappingBypassOwners.erase(owner.value);
    std::erase_if(impl_->passes, [&](const auto& pair) { return pair.second->owner == owner; });
    impl_->Collect();
}
bool PostProcessing::IsOwnerAlive(PostProcessOwner owner) const noexcept {
    return impl_->context && !impl_->failed && impl_->owners.contains(owner.value);
}

void PostProcessing::SetBuiltinTonemappingEnabled(PostProcessOwner owner, bool enabled) {
    impl_->RequireSetup(owner);
    if (enabled) impl_->tonemappingBypassOwners.erase(owner.value);
    else impl_->tonemappingBypassOwners.insert(owner.value);
}
bool PostProcessing::IsBuiltinTonemappingEnabled() const noexcept {
    return impl_->tonemappingBypassOwners.empty();
}

PostProcessShader PostProcessing::CreateShader(PostProcessOwner owner, const CompiledShader& shader) {
    impl_->RequireSetup(owner);
    if (shader.type != ShaderType::Pixel || !shader.blob || !shader.reflection)
        throw std::invalid_argument("Fullscreen effects require a compiled pixel shader with reflection.");
    for (const auto& binding : shader.bindings) {
        if (binding.type != D3D_SIT_CBUFFER && binding.type != D3D_SIT_TEXTURE && binding.type != D3D_SIT_SAMPLER)
            throw std::invalid_argument("Unsupported fullscreen shader resource: " + binding.name);
        if (binding.type == D3D_SIT_SAMPLER && binding.slot > 4)
            throw std::invalid_argument("Fullscreen shaders use engine samplers s0-s4.");
    }
    const auto token = Token();
    auto& manager = impl_->context->GetResourceManager();
    ShaderHandle handle;
    try { handle = manager.RegisterCompiledShader(shader); }
    catch (const RenderingException& error) { impl_->ReportResourceFailure(error); throw; }
    try { impl_->shaders.emplace(token, Impl::ShaderRecord{owner, handle, shader}); }
    catch (...) { manager.DestroyShader(handle); throw; }
    return {token};
}

ShaderParameters PostProcessing::ReflectParameters(PostProcessOwner owner, PostProcessShader shader, const std::string& name) const {
    impl_->RequireOwner(owner);
    auto parameters = ShaderParameters::Reflect(Impl::Owned(impl_->shaders, shader, owner).compiled, name);
    if (parameters.Slot() < 5 || parameters.Slot() >= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
        throw std::invalid_argument("Custom constant buffers must declare a register in b5-b13.");
    return parameters;
}
PostProcessBuffer PostProcessing::CreateConstantBuffer(PostProcessOwner owner, PostProcessShader shader, const std::string& name,
                                                       std::span<const ShaderParameterUpdate> initialValues) {
    impl_->RequireSetup(owner);
    auto parameters = ReflectParameters(owner, shader, name);
    parameters.Set(initialValues);
    auto& manager = impl_->context->GetResourceManager();
    const auto token = Token();
    BufferHandle handle;
    try {
        handle = manager.CreateBuffer({.type = BufferType::Constant, .usage = BufferUsage::Dynamic,
                                       .size = static_cast<std::uint32_t>(parameters.Bytes().size())}, nullptr);
    } catch (const RenderingException& error) { impl_->ReportResourceFailure(error); throw; }
    try { impl_->buffers.emplace(token, Impl::BufferRecord{owner, handle, shader, std::move(parameters), true}); }
    catch (...) { manager.DestroyBuffer(handle); throw; }
    return {token};
}
void PostProcessing::SetParameters(PostProcessOwner owner, PostProcessBuffer buffer, std::span<const ShaderParameterUpdate> updates) {
    impl_->RequireOwner(owner);
    auto& record = Impl::Owned(impl_->buffers, buffer, owner);
    record.parameters.Set(updates);
    record.dirty = true;
}
const std::vector<ShaderParameterField>& PostProcessing::GetParameterFields(PostProcessOwner owner, PostProcessBuffer buffer) const {
    impl_->RequireOwner(owner);
    return Impl::Owned(impl_->buffers, buffer, owner).parameters.Fields();
}
PostProcessTexture PostProcessing::CreateTarget(PostProcessOwner owner, const PostProcessTargetDesc& desc) {
    impl_->RequireSetup(owner);
    const auto token = Token();
    TextureHandle handle;
    try { handle = impl_->CreateTarget(desc); }
    catch (const RenderingException& error) { impl_->ReportResourceFailure(error); throw; }
    try { impl_->textures.emplace(token, Impl::TextureRecord{owner, handle, desc, false}); }
    catch (...) { impl_->context->GetResourceManager().DestroyTexture(handle); throw; }
    return {token, 0};
}
PostProcessTexture PostProcessing::ImportTexture(PostProcessOwner owner, TextureHandle texture) {
    impl_->RequireSetup(owner);
    const auto& resource = impl_->TextureFor(texture);
    if (!resource.HasShaderResourceView()) throw std::invalid_argument("Imported texture must be sampleable.");
    ComPtr<ID3D11ShaderResourceView> view = resource.GetSRV();
    const auto token = Token();
    auto& manager = impl_->context->GetResourceManager();
    const auto retained = manager.RegisterTexture(Texture(std::move(view)));
    try { impl_->textures.emplace(token, Impl::TextureRecord{owner, retained, {}, true}); }
    catch (...) { manager.DestroyTexture(retained); throw; }
    return {token, 0};
}
PostProcessPass PostProcessing::AddPass(PostProcessOwner owner, std::string name, PostProcessStage stage, int order, Callback callback) {
    impl_->RequireSetup(owner);
    StageIndex(stage);
    if (name.empty() || !callback) throw std::invalid_argument("Fullscreen passes require a name and execute callback.");
    for (const auto& [id, pass] : impl_->passes)
        if (pass->owner == owner && pass->name == name) throw std::invalid_argument("Duplicate fullscreen pass name: " + name);
    const auto token = Token();
    impl_->passes.emplace(token, std::make_shared<Impl::PassRecord>(
        Impl::PassRecord{owner, std::move(name), stage, order, token, std::move(callback), true, {}}));
    return {token};
}
void PostProcessing::RemovePass(PostProcessOwner owner, PostProcessPass pass) {
    impl_->RequireOwner(owner);
    const auto found = impl_->passes.find(pass.value);
    if (found == impl_->passes.end()) return;
    if (found->second->owner != owner) throw std::invalid_argument("Fullscreen pass belongs to another owner.");
    impl_->passes.erase(found);
}
void PostProcessing::SetEnabled(PostProcessOwner owner, PostProcessPass pass, bool enabled) {
    impl_->RequireOwner(owner);
    const auto found = impl_->passes.find(pass.value);
    if (found == impl_->passes.end() || found->second->owner != owner) throw std::invalid_argument("Fullscreen pass has expired.");
    found->second->enabled = enabled;
    if (enabled) found->second->error.clear();
}
std::string PostProcessing::GetPassError(PostProcessOwner owner, PostProcessPass pass) const {
    impl_->RequireOwner(owner);
    const auto found = impl_->passes.find(pass.value);
    if (found == impl_->passes.end() || found->second->owner != owner) throw std::invalid_argument("Fullscreen pass has expired.");
    return found->second->error;
}

void PostProcessing::BeginFrame() {
    if (!impl_->context || impl_->frameActive) throw std::logic_error("Invalid postprocess frame boundary.");
    for (auto& schedule : impl_->schedule) schedule.clear();
    for (const auto& [id, pass] : impl_->passes) {
        if (pass->enabled && impl_->owners.contains(pass->owner.value))
            impl_->schedule[StageIndex(pass->stage)].push_back(pass);
    }
    for (auto& schedule : impl_->schedule) {
        std::sort(schedule.begin(), schedule.end(), [](const auto& a, const auto& b) {
            return a->order == b->order ? a->sequence < b->sequence : a->order < b->order;
        });
    }
    impl_->frameActive = true;
}
void PostProcessing::EndFrame() noexcept {
    for (auto& schedule : impl_->schedule) schedule.clear();
    impl_->frameActive = false;
    impl_->Collect();
}
bool PostProcessing::HasPasses() const noexcept {
    return std::any_of(impl_->schedule.begin(), impl_->schedule.end(), [](const auto& passes) { return !passes.empty(); });
}

TextureHandle PostProcessing::Execute(PostProcessStage stage, TextureHandle input, RenderWorld& world, RenderingResources& resources) {
    if (!impl_->frameActive) throw std::logic_error("Postprocess execution requires BeginFrame.");
    auto& context = *impl_->context;
    auto& targets = stage == PostProcessStage::BeforeTonemap ? impl_->hdrTargets : impl_->colorTargets;
    const auto format = stage == PostProcessStage::BeforeTonemap ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    for (const auto& pass : impl_->schedule[StageIndex(stage)]) {
        if (!impl_->owners.contains(pass->owner.value) || !impl_->passes.contains(pass->sequence)) continue;
        auto& output = targets[targets[0] == input ? 1 : 0];
        if (!output) output = impl_->CreateTarget({.format = format});
        auto state = std::make_shared<PostProcessContext::State>();
        state->system = impl_.get();
        state->owner = pass->owner;
        state->execution = Token();
        state->frameTextures = {input, output, resources.gBuffer.depthTexture, resources.gBuffer.normalTexture};
        const auto size = TextureSize(impl_->TextureFor(input));
        const PostProcessFrame frame{
            {1, state->execution}, {2, state->execution}, {3, state->execution}, {4, state->execution},
            size[0], size[1], impl_->width, impl_->height};
        PostProcessContext commands(state);
        context.ClearState();
        resources.BindCBuffers(context);
        world.BindCameraView(context, resources);
        impl_->FullscreenState();
        context.ClearRenderTargetView(output);
        try {
            pass->callback(commands, frame);
            if (state->nativeFailure) std::rethrow_exception(state->nativeFailure);
            if (!state->wroteOutput) throw std::invalid_argument("Fullscreen pass did not write outputColor.");
            if (impl_->owners.contains(pass->owner.value)) input = output;
        } catch (const RenderingException&) {
            context.ClearState();
            throw;
        } catch (const std::exception& error) {
            if (state->nativeFailure) std::rethrow_exception(state->nativeFailure);
            pass->enabled = false;
            pass->error = error.what();
            log::Render().error("Fullscreen pass '{}' disabled: {}", pass->name, pass->error);
        } catch (...) {
            if (state->nativeFailure) std::rethrow_exception(state->nativeFailure);
            pass->enabled = false;
            pass->error = "Unknown fullscreen callback error.";
            log::Render().error("Fullscreen pass '{}' disabled: {}", pass->name, pass->error);
        }
        // Retained Lua/C++ copies cannot use this capability after the callback.
        state.reset();
        context.ClearState();
    }
    return input;
}

TextureHandle PostProcessing::GetCompositionTarget() {
    if (!impl_->context) throw std::logic_error("Postprocessing is uninitialized.");
    if (!impl_->composition) impl_->composition = impl_->CreateTarget({.format = DXGI_FORMAT_R8G8B8A8_UNORM});
    return impl_->composition;
}
void PostProcessing::CopyTo(TextureHandle input, TextureHandle output, std::uint32_t width, std::uint32_t height) {
    if (input == output) throw std::invalid_argument("Fullscreen copy requires distinct input and output targets.");
    auto& context = *impl_->context;
    context.ClearState();
    impl_->FullscreenState();
    context.SetViewport(width, height);
    context.SetRenderTargets({output}, {});
    context.SetShader(impl_->copyPS);
    context.SetShaderResources(BindSlots::Texture::Pass0, input);
    context.RawDraw(6, 0);
    context.ClearHazardousState();
}
} // namespace engine
