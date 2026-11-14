#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/core/Subsystem.hpp>
#include <engine/ui/UiHost.hpp>
#include <engine/ui/UiModel.hpp>
#include <engine/ui/UiTypes.hpp>

namespace engine {
struct UiSubsystemConfig {
    UiRendererBackend renderer = UiRendererBackend::Dx11;
    std::shared_ptr<UiHost> host; // null supplies in-memory clipboard and no-op host requests
    UiSurfaceMetrics nullSurface{1280, 720, 1.0f};
    bool enableDebugger = false; // rejected/disabled in non-developer builds
};

struct UiDocumentDesc {
    // Explicit dependencies, all belonging to the document owner. Loader also checks every
    // data-model name in documents/templates against this list, before committing a load.
    std::vector<UiModel> models;
};

// Main-thread-only API. All arguments are copied before deferred work. Handles do not
// extend lifetime. bool/out/error operations leave outputs unchanged on failure.
// Mutators validate the owner; Lua bindings always supply the runtime's active owner.
class ENGINE_API UiSubsystem final : public Subsystem {
public:
    explicit UiSubsystem(UiSubsystemConfig config = {});
    ~UiSubsystem() override;
    UiSubsystem(const UiSubsystem&) = delete;
    UiSubsystem& operator=(const UiSubsystem&) = delete;
    UiSubsystem(UiSubsystem&&) = delete;
    UiSubsystem& operator=(UiSubsystem&&) = delete;

    [[nodiscard]] std::string_view GetName() const override;
    [[nodiscard]] bool Initialize() override;
    void Shutdown() override;
    void Update(const FrameContext& frame) override;
    void LateUpdate(const FrameContext& frame) override;
    void Render(const FrameContext& frame) override; // intentionally no drawing
    void ResetSimulation() override; // release transient input only; retain live owner resources
    [[nodiscard]] bool IsInitialized() const noexcept;
    // Expires at Shutdown (also on failed Initialize), renewed for each successful runtime.
    // Main-thread adapters pair this with a borrowed UiSubsystem pointer and check before use.
    [[nodiscard]] std::weak_ptr<void> GetLifetimeToken() const noexcept;
    // For offscreen/custom hosts. A host-provided snapshot supersedes this on the
    // next update; DX11 dimensions are constrained to the rendering surface.
    void SetSurfaceMetrics(UiSurfaceMetrics metrics);
    [[nodiscard]] UiCaptureState GetCaptureState() const noexcept;

    // Independent of Lua. Session teardown is explicit; Shutdown closes all native sessions.
    bool BeginNativeSession(UiSession& session, std::string& error);
    bool CreateNativeOwner(UiSession session, std::string_view diagnosticName,
                           UiOwner& owner, std::string& error);
    void EndNativeSession(UiSession session) noexcept;
    void ReleaseNativeOwner(UiOwner owner) noexcept;

    bool LoadDocument(UiOwner owner, const AssetReference& asset, const UiDocumentDesc& desc,
                      UiDocument& document, std::string& error);
    bool LoadDocument(UiOwner owner, std::string_view catalogLogicalPath, const UiDocumentDesc& desc,
                      UiDocument& document, std::string& error);
    bool Show(UiOwner owner, UiDocument document, UiDocumentMode mode, std::string& error);
    bool Hide(UiOwner owner, UiDocument document, std::string& error);
    bool Close(UiOwner owner, UiDocument document, std::string& error); // idempotent same owner
    // Accepted request, not completion. Commit is atomic at next safe Rml boundary;
    // failure is logged and old DOM retained. Never changes document token.
    bool RequestReload(UiOwner owner, UiDocument document, std::string& error);
    [[nodiscard]] bool IsValid(UiDocument document) const noexcept;
    [[nodiscard]] bool IsVisible(UiDocument document) const noexcept;

    bool CreateModel(UiOwner owner, const UiModelDesc& desc, UiModel& model, std::string& error);
    bool SetModelValue(UiOwner owner, UiModel model, std::string_view variable,
                       const StateValue& value, std::string& error);
    bool GetModelValue(UiOwner owner, UiModel model, std::string_view variable,
                       StateValue& value, std::string& error) const;
    bool RemoveModel(UiOwner owner, UiModel model, std::string& error); // fails while depended on
    [[nodiscard]] bool IsValid(UiModel model) const noexcept;

    bool FindElement(UiOwner owner, UiDocument document, std::string_view id,
                     UiElement& element, std::string& error);
    [[nodiscard]] bool IsValid(UiElement element) const noexcept;
    bool SetText(UiOwner owner, UiElement element, std::string_view utf8, std::string& error);
    bool SetInnerRml(UiOwner owner, UiElement element, std::string_view rml, std::string& error);
    bool SetFormValue(UiOwner owner, UiElement element, std::string_view utf8, std::string& error);
    bool GetFormValue(UiOwner owner, UiElement element, std::string& utf8, std::string& error) const;
    bool SetClass(UiOwner owner, UiElement element, std::string_view name, bool enabled, std::string& error);
    bool SetProperty(UiOwner owner, UiElement element, std::string_view name,
                     std::string_view value, std::string& error);
    bool Focus(UiOwner owner, UiElement element, std::string& error);

    // Native owners only. Stored until disconnect/owner teardown, not handle destruction.
    // Called only in Update under a native owner/scene guard; exceptions fault the owner.
    using NativeCallback = std::function<void(const UiEvent&)>;
    bool SubscribeNative(UiOwner owner, UiDocument document, std::string_view elementId,
                         std::string_view eventName, NativeCallback callback,
                         UiSubscription& subscription, std::string& error);
    bool Disconnect(UiOwner owner, UiSubscription subscription, std::string& error);
    [[nodiscard]] bool IsValid(UiSubscription subscription) const noexcept;

private:
    friend class UiRuntimeAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace engine
