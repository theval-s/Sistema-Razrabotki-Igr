#pragma once

// Private engine bridge. No Lua state exposure and no Rml headers needed here.
#include <memory>
#include <span>
#include <variant>
#include <engine/input/InputUiEvent.hpp>
#include <engine/ui/UiSubsystem.hpp>

namespace engine {
struct RenderingContext;

enum class UiDispatchResult { Delivered, Dropped, OwnerFaulted };

// Implement inside LuaRuntime.cpp where Impl/state are visible. Runtime owns the callback
// registry (sol::protected_function); this interface and every UI queue contain tokens only.
// UI retains a weak_ptr; LuaRuntime owns a shared bridge and invalidates it on shutdown.
class UiCallbackDispatcher {
public:
    virtual ~UiCallbackDispatcher() = default;
    // Validate token, session, live started owner, active/reentrant and teardown flags;
    // establish runtime active-owner scope + Impl::Scope + Instance::active guard;
    // then protected-call. Return OwnerFaulted on callback failure with diagnostics.
    virtual UiDispatchResult Dispatch(UiOwner owner, UiCallback callback, const UiEvent& event,
                                      std::string& error) = 0;
    // Synchronously erase Lua references, never invoke user code. Idempotent, noexcept.
    virtual void ReleaseCallback(UiCallback callback) noexcept = 0;
};

// Adapter storage addresses must be boxed stable path nodes, never StateValue/vector
// element addresses. Paths resolve afresh on EVERY read/write/size query.
using UiModelPathPart = std::variant<std::string, std::uint32_t>; // array indices are zero-based
using UiModelPath = std::vector<UiModelPathPart>;
class UiModelStorage {
public:
    virtual ~UiModelStorage() = default;
    virtual const UiValueSchema& Schema() const noexcept = 0; // immutable until destruction
    virtual bool Read(std::span<const UiModelPathPart> path, StateValue& copy, std::string& error) const = 0;
    virtual bool Write(std::span<const UiModelPathPart> path, const StateValue& copy, std::string& error) = 0;
    virtual bool ArraySize(std::span<const UiModelPathPart> path, std::size_t& size, std::string& error) const = 0;
    // Includes writes originating from Rml forms. Swap dirty set only at a publish boundary.
    virtual std::vector<std::string> TakeDirtyVariables() = 0;
};

struct UiRuntimeStats {
    std::size_t sessions = 0, owners = 0, documents = 0, models = 0;
    std::size_t subscriptions = 0, queuedEvents = 0, pendingRetirements = 0;
};

// Trusted lifecycle/render/test entry points; never expose through Lua or installed SDK.
class ENGINE_API UiRuntimeAccess final {
public:
    static bool BeginScriptSession(UiSubsystem& ui, std::weak_ptr<UiCallbackDispatcher> dispatcher,
                                   UiSession& session, std::string& error);
    // LuaRuntime keeps InstanceId -> UiOwner mapping privately; no public InstanceId fields needed.
    static bool CreateScriptOwner(UiSubsystem& ui, UiSession session, std::string_view diagnosticName,
                                  UiOwner& owner, std::string& error);
    // Newly created owner is dormant. Enable immediately before OnStart, which sets started=true.
    static void EnableScriptOwner(UiSubsystem& ui, UiOwner owner) noexcept;
    // Reject new registrations and deliveries, but permit cleanup/mutation of existing UI in OnDestroy.
    static void BeginOwnerTeardown(UiSubsystem& ui, UiOwner owner) noexcept;
    static void ReleaseOwner(UiSubsystem& ui, UiOwner owner) noexcept;
    static void SuspendSession(UiSubsystem& ui, UiSession session) noexcept;
    // Synchronous logical retirement + ReleaseCallback for all callbacks BEFORE returning.
    // Physical DOM/model destruction can defer, retaining exclusively C++ data and tokens.
    static void EndSession(UiSubsystem& ui, UiSession session) noexcept;

    // Allocate token before runtime inserts protected function; cancel on any registration failure.
    static bool ReserveCallback(UiSubsystem& ui, UiOwner owner, UiCallback& callback, std::string& error);
    static void CancelCallback(UiSubsystem& ui, UiOwner owner, UiCallback callback) noexcept;
    // Transfers reserved token to subscription on success; otherwise caller cancels it.
    static bool Subscribe(UiSubsystem& ui, UiOwner owner, UiDocument document,
                          std::string_view elementId, std::string_view eventName, UiCallback callback,
                          UiSubscription& subscription, std::string& error);

    // Overlay hook only, after composition/debug and before Present. No callback dispatch.
    // RenderingContext's currently bound target is borrowed for this call only.
    static void RenderOverlay(UiSubsystem& ui, RenderingContext& context, const FrameContext& frame);
    static void BeforeSurfaceResize(UiSubsystem& ui) noexcept;

    // Test seams use normal queues/validation, NEVER directly dispatch. Engine+Null backend
    // tests need AssetCatalog/Input but no RenderingSubsystem, native window or Lua runtime.
    static bool QueueEventForTesting(UiSubsystem& ui, UiSubscription subscription,
                                     const UiEvent& event, std::string& error);
    static bool WriteModelForTesting(UiSubsystem& ui, UiOwner owner, UiModel model,
                                     std::span<const UiModelPathPart> path, const StateValue& value,
                                     std::string& error);
    static UiRuntimeStats Inspect(const UiSubsystem& ui) noexcept;
};
} // namespace engine
