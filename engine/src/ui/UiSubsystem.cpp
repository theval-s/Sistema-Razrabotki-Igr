#include <engine/ui/UiSubsystem.hpp>
#include "UiRuntimeAccess.hpp"
#include "UiModelStorage.hpp"
#include "UiInterfaces.hpp"
#include "UiDx11Renderer.hpp"
#include <engine/engine.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/rendering/RenderingSubsystem.hpp>
#include <engine/core/Log.hpp>
#include <RmlUi/Debugger.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/Variant.h>
#include <atomic>
#include <mutex>
#include <limits>
#include <optional>
#include <deque>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <climits>
#include <type_traits>

namespace engine {
namespace {
std::mutex runtimeMutex;
bool runtimeLeased = false;
std::atomic<std::uint64_t> nextSession{1};
std::uint64_t AllocateSession() {
    auto id = nextSession.load();
    while (id && !nextSession.compare_exchange_weak(id, id == UINT64_MAX ? 0 : id + 1)) {}
    return id;
}
auto& Logger() { return log::Channel("UI"); }
struct SceneCallbackScope {
    Scene& scene;
    explicit SceneCallbackScope(Engine& engine) : scene(engine.GetScene()) { scene.BeginExternalCallback(); }
    ~SceneCallbackScope() { scene.EndExternalCallback(); }
};
template<class F> void Walk(Rml::Element* element, F&& f) {
    f(element); for (int i = 0; i < element->GetNumChildren(); ++i) Walk(element->GetChild(i), f);
}
Rml::Input::KeyIdentifier TranslateKey(const InputUiKey& key) {
    using namespace Rml::Input;
    switch (key.navigation) {
    case InputUiNavigationKey::Home: return KI_HOME; case InputUiNavigationKey::End: return KI_END;
    case InputUiNavigationKey::PageUp: return KI_PRIOR; case InputUiNavigationKey::PageDown: return KI_NEXT;
    case InputUiNavigationKey::Insert: return KI_INSERT; default: break;
    }
    auto k = key.logical;
    if (k >= Key::A && k <= Key::Z) return static_cast<KeyIdentifier>(KI_A + int(k) - int(Key::A));
    if (k >= Key::Num0 && k <= Key::Num9) return static_cast<KeyIdentifier>(KI_0 + int(k) - int(Key::Num0));
    if (k >= Key::F1 && k <= Key::F12) return static_cast<KeyIdentifier>(KI_F1 + int(k) - int(Key::F1));
    switch (k) {
    case Key::Space: return KI_SPACE; case Key::Enter: return KI_RETURN; case Key::Escape: return KI_ESCAPE;
    case Key::Tab: return KI_TAB; case Key::Backspace: return KI_BACK; case Key::Delete: return KI_DELETE;
    case Key::Left: return KI_LEFT; case Key::Right: return KI_RIGHT; case Key::Up: return KI_UP; case Key::Down: return KI_DOWN;
    case Key::Shift: return KI_LSHIFT; case Key::Control: return KI_LCONTROL; case Key::Alt: return KI_LMENU;
    default: return KI_UNKNOWN;
    }
}
int Modifiers(const InputUiModifiers& m) {
    using namespace Rml::Input;
    return (m.shift ? KM_SHIFT : 0) | (m.control ? KM_CTRL : 0) | (m.alt ? KM_ALT : 0) | (m.meta ? KM_META : 0) | (m.capsLock ? KM_CAPSLOCK : 0) | (m.numLock ? KM_NUMLOCK : 0);
}
bool CopyVariant(const Rml::Variant& v, StateValue& out) {
    switch (v.GetType()) {
    case Rml::Variant::BOOL: out = v.Get<bool>(); return true;
    case Rml::Variant::BYTE: case Rml::Variant::CHAR: case Rml::Variant::FLOAT: case Rml::Variant::DOUBLE:
    case Rml::Variant::INT: case Rml::Variant::INT64: case Rml::Variant::UINT: case Rml::Variant::UINT64:
    { auto d = v.Get<double>(); if (!std::isfinite(d)) return false; out = d; return true; }
    case Rml::Variant::STRING: out = v.Get<Rml::String>(); return true;
    default: return false;
    }
}
}

struct UiSubsystem::Impl {
    UiSubsystem& api;
    UiSubsystemConfig config;
    Engine* engine = nullptr;
    InputSubsystem* input = nullptr;
    RenderingSubsystem* rendering = nullptr;
    std::shared_ptr<void> lifetime;
    std::unique_ptr<ui_detail::System> system;
    std::unique_ptr<ui_detail::Files> files;
    std::unique_ptr<Rml::RenderInterface> renderer;
    Rml::Context* context = nullptr;
    bool leased = false, core = false, initialized = false, stopping = false, debugger = false, overlayAttached = false;
    unsigned traversal = 0, callbackDepth = 0;
    bool draining = false, suppressEvents = false, pendingDomClose = false;
    std::uint64_t inputSequence = 0;
    UiSurfaceMetrics metrics;
    UiCaptureState capture;
    std::set<int> pressedKeys, pressedButtons;
    std::set<std::string> fonts;
    bool dragging = false, focused = true;

    struct Session { bool live = true, suspended = false, native = false; std::weak_ptr<UiCallbackDispatcher> dispatcher; };
    struct Owner { bool live = true, enabled = false, retiring = false, native = false, executing = false; UiSession session; std::string name; };
    struct Document {
        bool live = true, visible = false, reload = false;
        UiOwner owner; UiDocument token; CatalogEntry asset; UiDocumentDesc desc;
        Rml::ElementDocument* dom = nullptr;
        std::uint64_t revision = 1;
        UiDocumentMode mode = UiDocumentMode::Interactive;
        UiDocument previousFocusDocument; std::string previousFocusId;
        struct Styles { ui_detail::ElementObserver element; std::optional<Rml::Property> pointer, focus; };
        std::map<Rml::Element*, Styles> hudStyles;
    };
    struct Model { bool live = true; UiOwner owner; std::string name; std::shared_ptr<ui_detail::ModelStorage> storage; Rml::DataModelHandle handle; };
    struct Element { bool live = true; UiOwner owner; UiDocument document; std::uint64_t revision; std::string id; ui_detail::ElementObserver dom; };
    struct Callback { bool live = true; UiOwner owner; };
    struct Listener final : Rml::EventListener {
        Impl& runtime; UiSubscription token;
        Listener(Impl& runtime, UiSubscription token) : runtime(runtime), token(token) {}
        void ProcessEvent(Rml::Event& event) override { runtime.Listen(token, event); }
    };
    struct Subscription {
        bool live = true; UiOwner owner; UiDocument document; UiSubscription token;
        std::string id, event; UiCallback callback; NativeCallback native;
        std::unique_ptr<Listener> listener;
        // Borrowed DOM pointer only while attached. Always detach before replacing DOM.
        ui_detail::ElementObserver attached;
        std::uint64_t attachment = 0;
        bool dormant = false;
    };
    template<class Record, class Handle> struct Table {
        struct Slot { std::uint64_t session = 0; std::uint32_t generation = 1; bool retired = false; std::unique_ptr<Record> record; };
        std::vector<std::unique_ptr<Slot>> slots;
        // Tokens, rather than slots, are cleanup identities, so reuse cannot erase idempotence.
        std::vector<std::pair<Handle, UiOwner>> tombstones;
        Handle Add(std::uint64_t session, std::unique_ptr<Record> record) {
            if constexpr (std::is_same_v<Handle, UiDocument> || std::is_same_v<Handle, UiSubscription>) {
                // Close/Disconnect run in noexcept teardown, including after an
                // allocation failure in Lua. Reserve retirement bookkeeping while
                // registration is still allowed to fail and roll back safely.
                const auto needed = tombstones.size() + slots.size() + 1;
                if (needed > tombstones.capacity()) tombstones.reserve(std::max(needed, tombstones.capacity() * 2));
            }
            std::size_t i = 0; for (; i < slots.size(); ++i) if (!slots[i]->record && !slots[i]->retired) break;
            if (i >= UINT32_MAX) throw std::overflow_error("UI slot space exhausted");
            if (i == slots.size()) slots.push_back(std::make_unique<Slot>());
            auto& s = *slots[i]; s.session = session; s.record = std::move(record);
            Handle h; h.session_ = session; h.slot_ = static_cast<std::uint32_t>(i); h.generation_ = s.generation; return h;
        }
        Record* Get(Handle h) const {
            if (h.IsEmpty() || h.slot_ >= slots.size()) return nullptr;
            auto& s = *slots[h.slot_]; return s.session == h.session_ && s.generation == h.generation_ ? s.record.get() : nullptr;
        }
        void Erase(std::size_t i) {
            auto& s = *slots[i]; s.record.reset();
            if (s.generation == UINT32_MAX) s.retired = true; else ++s.generation;
        }
        bool Was(Handle h, UiOwner o) const { return std::find(tombstones.begin(), tombstones.end(), std::pair{h, o}) != tombstones.end(); }
    };
    Table<Session, UiSession> sessions;
    Table<Owner, UiOwner> owners;
    Table<Document, UiDocument> documents;
    Table<Model, UiModel> models;
    Table<Element, UiElement> elements;
    Table<Subscription, UiSubscription> subscriptions;
    Table<Callback, UiCallback> callbacks;
    struct QueuedEvent { UiSubscription subscription; UiDocument document; UiOwner owner; std::uint64_t revision; UiEvent event; std::uint64_t attachment = 0; };
    std::deque<QueuedEvent> events;
    explicit Impl(UiSubsystem& api, UiSubsystemConfig config) : api(api), config(std::move(config)), metrics(this->config.nullSurface) {}
    struct Traverse { Impl& p; explicit Traverse(Impl& p) : p(p) { ++p.traversal; } ~Traverse() { --p.traversal; } };
    Session* SessionFor(UiOwner owner) const { auto o = owners.Get(owner); return o ? sessions.Get(o->session) : nullptr; }
    bool OwnerLive(UiOwner owner, bool create = false) const {
        auto o = owners.Get(owner); auto s = SessionFor(owner);
        return initialized && !stopping && o && o->live && s && s->live && (!create || (o->enabled && !o->retiring && !s->suspended));
    }
    bool Deliverable(UiOwner owner) const { auto s = SessionFor(owner); return OwnerLive(owner, true) && !s->suspended; }
    bool CheckOwner(UiOwner owner, std::string& e, bool create = false) const { if (OwnerLive(owner, create)) return true; e = "UI owner/session is unavailable, dormant, suspended, or revoked"; return false; }
    Document* Doc(UiDocument token) const { auto d = documents.Get(token); return d && d->live && OwnerLive(d->owner) ? d : nullptr; }
    Model* Mod(UiModel token) const { auto m = models.Get(token); return m && m->live && OwnerLive(m->owner) ? m : nullptr; }
    Subscription* Sub(UiSubscription token) const { auto s = subscriptions.Get(token); return s && s->live && Doc(s->document) && OwnerLive(s->owner) ? s : nullptr; }
    Document* Owned(UiOwner owner, UiDocument token, std::string& e) const { auto d = Doc(token); if (!d || d->owner != owner || !CheckOwner(owner, e)) { e = "Invalid or foreign UI document"; return nullptr; } return d; }
    Model* Owned(UiOwner owner, UiModel token, std::string& e) const { auto m = Mod(token); if (!m || m->owner != owner || !CheckOwner(owner, e)) { e = "Invalid or foreign UI model"; return nullptr; } return m; }
    Rml::Element* Owned(UiOwner owner, UiElement token, std::string& e) const {
        auto element = elements.Get(token); auto d = element ? Doc(element->document) : nullptr;
        if (!element || !element->live || !d || element->owner != owner || element->revision != d->revision || !CheckOwner(owner, e)) { e = "Invalid, stale, or foreign UI element"; return nullptr; }
        auto result = element->dom.get(); if (!result || result != d->dom->GetElementById(element->id)) { e = "UI element no longer exists"; return nullptr; } return result;
    }
    bool BeginSession(bool native, std::weak_ptr<UiCallbackDispatcher> dispatcher, UiSession& out, std::string& e) {
        if (!initialized || stopping) { e = "UI runtime is not initialized"; return false; }
        auto id = AllocateSession(); if (!id) { e = "UI session identity space exhausted"; return false; }
        auto s = std::make_unique<Session>(); s->native = native; s->dispatcher = std::move(dispatcher); out = sessions.Add(id, std::move(s)); return true;
    }
    bool CreateOwner(UiSession session, std::string_view name, bool native, UiOwner& out, std::string& e) {
        auto s = sessions.Get(session); if (!initialized || stopping || !s || !s->live || s->suspended || s->native != native) { e = "UI session cannot create owners"; return false; }
        auto o = std::make_unique<Owner>(); o->session = session; o->native = native; o->enabled = native; o->name = name;
        out = owners.Add(session.session_, std::move(o)); return true;
    }
    void ReleaseCallback(UiCallback token) noexcept {
        auto c = callbacks.Get(token); if (!c || !c->live) return; c->live = false;
        if (auto s = SessionFor(c->owner)) if (auto dispatcher = s->dispatcher.lock()) dispatcher->ReleaseCallback(token);
    }
    void Disconnect(Subscription& s) noexcept { if (!s.live) return; s.live = false; ReleaseCallback(s.callback); subscriptions.tombstones.emplace_back(s.token, s.owner); }
    void Close(Document& d) noexcept {
        if (!d.live) return; d.live = false; d.visible = false; d.reload = false;
        documents.tombstones.emplace_back(d.token, d.owner);
        for (auto& slot : subscriptions.slots) if (slot->record && slot->record->document == d.token) Disconnect(*slot->record);
        for (auto& slot : elements.slots) if (slot->record && slot->record->document == d.token) slot->record->live = false;
    }
    void Teardown(UiOwner owner, bool release) noexcept {
        auto o = owners.Get(owner); if (!o) return;
        auto focusDocument = context ? DocumentFor(context->GetFocusElement()) : nullptr;
        bool ownsFocus = focusDocument && focusDocument->owner == owner;
        o->retiring = true;
        for (auto& slot : callbacks.slots) if (slot->record && slot->record->owner == owner) { auto& s = *slot; UiCallback c; c.session_ = s.session; c.slot_ = static_cast<std::uint32_t>(&slot - callbacks.slots.data()); c.generation_ = s.generation; ReleaseCallback(c); }
        for (auto& slot : subscriptions.slots) if (slot->record && slot->record->owner == owner) Disconnect(*slot->record);
        std::erase_if(events, [&](const auto& e) { return e.owner == owner; });
        if (system && ownsFocus) system->Cancel();
        dragging = false;
        if (release) {
            o->live = false;
            for (auto& slot : documents.slots) if (slot->record && slot->record->owner == owner) Close(*slot->record);
            for (auto& slot : models.slots) if (slot->record && slot->record->owner == owner) slot->record->live = false;
        }
        RefreshCapture();
    }
    void Suspend(UiSession token) noexcept {
        auto s = sessions.Get(token); if (!s || !s->live) return; s->suspended = true;
        for (auto& slot : owners.slots) if (slot->record && slot->record->session == token) slot->record->retiring = true;
        std::erase_if(events, [&](const auto& e) { return e.owner.session_ == token.session_; });
        if (system) system->Cancel(); RefreshCapture();
    }
    void EndSession(UiSession token) noexcept {
        auto s = sessions.Get(token); if (!s || !s->live) return; Suspend(token);
        for (std::size_t i = 0; i < owners.slots.size(); ++i) if (auto& slot = *owners.slots[i]; slot.record && slot.record->session == token) {
            UiOwner o; o.session_ = slot.session; o.slot_ = static_cast<std::uint32_t>(i); o.generation_ = slot.generation; Teardown(o, true);
        }
        s->live = false;
        // Idempotent cleanup is scoped to a session. Play/Stop must not retain a
        // history of every document/subscription ever created by old Lua states.
        std::erase_if(documents.tombstones, [&](const auto& entry) { return entry.first.session_ == token.session_; });
        std::erase_if(subscriptions.tombstones, [&](const auto& entry) { return entry.first.session_ == token.session_; });
        Drain();
    }
    void Detach(Subscription& s) {
        if (s.attached && s.listener) s.attached->RemoveEventListener(s.event, s.listener.get());
        s.attached.reset();
    }
    void Attach(Subscription& s) {
        auto d = Doc(s.document); if (!s.live || !d || !d->dom) { Detach(s); return; }
        auto target = s.id.empty() ? d->dom : d->dom->GetElementById(s.id);
        if (s.attached.get() == target && target) return;
        if (!target && s.dormant) return;
        Detach(s); ++s.attachment;
        s.dormant = target == nullptr;
        if (target) { s.attached = target->GetObserverPtr(); s.attached->AddEventListener(s.event, s.listener.get()); }
        else Logger().warn("UI subscription dormant: {} #{} ({})", d->asset.path, s.id, s.event);
    }
    bool Subscribe(UiOwner owner, UiDocument token, std::string_view id, std::string_view event, NativeCallback native, UiCallback callback, UiSubscription& out, std::string& e) {
        auto d = Owned(owner, token, e); if (!d || !CheckOwner(owner, e, true)) return false;
        if (event.empty() || !ui_detail::Utf8(id) || !ui_detail::Utf8(event)) { e = "Invalid UI event name or element ID"; return false; }
        if (!id.empty() && !d->dom->GetElementById(std::string(id))) { e = "UI subscription target not found: " + std::string(id); return false; }
        auto s = std::make_unique<Subscription>(); s->owner = owner; s->document = token; s->id = id; s->event = event; s->native = std::move(native); s->callback = callback;
        s->listener = std::make_unique<Listener>(*this, UiSubscription{});
        auto handle = subscriptions.Add(owner.session_, std::move(s)); auto record = subscriptions.Get(handle);
        record->token = handle; record->listener->token = handle;
        try { Attach(*record); }
        catch (...) {
            Disconnect(*record); Detach(*record); subscriptions.Erase(handle.slot_);
            throw;
        }
        out = handle; return true;
    }
    void Listen(UiSubscription token, Rml::Event& event) {
        auto s = Sub(token); if (suppressEvents || !s || !Deliverable(s->owner)) return;
        auto d = Doc(s->document); UiEvent copy; copy.document = s->document; copy.sourcePath = d->asset.path; copy.type = event.GetType(); copy.inputSequence = inputSequence;
        if (auto target = event.GetTargetElement()) copy.targetId = target->GetId();
        if (auto current = event.GetCurrentElement()) copy.currentTargetId = current->GetId();
        for (auto& [key, value] : event.GetParameters()) { StateValue v; if (CopyVariant(value, v)) copy.parameters.emplace(key, std::move(v)); }
        events.push_back({token, s->document, s->owner, d->revision, std::move(copy), s->attachment});
    }
    void Dispatch(std::deque<QueuedEvent> wave) {
        for (auto& q : wave) {
            auto s = Sub(q.subscription); auto d = Doc(q.document);
            if (!s || !d || d->revision != q.revision || s->attachment != q.attachment || !s->attached || s->owner != q.owner || !Deliverable(q.owner)) continue;
            auto o = owners.Get(q.owner); if (o->executing) { Logger().warn("Dropped reentrant UI delivery for '{}'", o->name); continue; } o->executing = true; ++callbackDepth;
            UiDispatchResult result = UiDispatchResult::Dropped; std::string error;
            try {
                SceneCallbackScope scene(*engine);
                if (s->native) { auto callback = s->native; callback(q.event); result = UiDispatchResult::Delivered; }
                else if (auto session = SessionFor(q.owner)) if (auto dispatcher = session->dispatcher.lock()) result = dispatcher->Dispatch(q.owner, s->callback, q.event, error);
            } catch (const std::exception& ex) { error = ex.what(); result = UiDispatchResult::OwnerFaulted; }
            catch (...) { error = "Unknown native UI callback exception"; result = UiDispatchResult::OwnerFaulted; }
            --callbackDepth; o->executing = false;
            if (result == UiDispatchResult::OwnerFaulted) { Logger().error("UI owner '{}' fault in {} #{} {}: {}", o->name, q.event.sourcePath, q.event.targetId, q.event.type, error); Teardown(q.owner, true); }
            Drain();
            if (stopping) break;
        }
    }
    bool Dependencies(const UiDocumentDesc& desc, UiOwner owner, std::set<std::string>& names, std::string& e) {
        for (auto handle : desc.models) { auto m = Owned(owner, handle, e); if (!m) return false; names.insert(m->name); } return true;
    }
    Rml::ElementDocument* Candidate(Document& d, std::string& e) {
        std::set<std::string> allowed; if (!Dependencies(d.desc, d.owner, allowed, e)) return nullptr;
        RegisterFonts();
        // Rml caches hold shared references; clearing them leaves live DOM styles intact
        // and forces this candidate (including templates) through validation again.
        Rml::Factory::ClearStyleSheetCache(); Rml::Factory::ClearTemplateCache();
        files->diagnostics.clear(); files->modelNames.clear(); system->diagnostics.clear();
        struct RestoreCollection {
            Impl& p;
            bool events, files, system;
            ~RestoreCollection() { p.suppressEvents = events; p.files->collecting = files; p.system->collecting = system; }
        } collection{*this, suppressEvents, files->collecting, system->collecting};
        files->collecting = system->collecting = true;
        suppressEvents = true;
        Rml::ElementDocument* dom = nullptr;
        // A load can throw after Rml has attached the document (layout, diagnostic
        // collection, dependency validation). Keep that candidate transactional too.
        struct RetireCandidate {
            Impl& p; Rml::ElementDocument*& dom; bool committed = false;
            ~RetireCandidate() {
                if (dom && !committed) { Traverse guard(p); dom->Close(); p.pendingDomClose = true; }
            }
        } retirement{*this, dom};
        { Traverse guard(*this); dom = context->LoadDocument(d.asset.path); if (dom) { dom->Hide();
            // Screen-space documents need a surface-sized containing block, and
            // Rml's generic <button> needs the keyboard default its inputs already
            // receive. Prepend low-priority rules so authored styles/classes and
            // explicit tab-index:none continue to win, including after DOM changes.
            auto defaults = Rml::Factory::InstanceStyleSheetString("body { width: 100%; height: 100%; } button { tab-index: auto; }");
            if (auto authored = dom->GetStyleSheetContainer()) defaults = defaults->CombineStyleSheetContainer(*authored);
            dom->SetStyleSheetContainer(std::move(defaults));
            dom->UpdateDocument();
            Walk(dom, [&](Rml::Element* element) { if (auto attr = element->GetAttribute("data-model")) files->modelNames.insert(attr->Get<Rml::String>()); });
        } }
        for (auto& name : files->modelNames) if (!allowed.contains(name)) files->diagnostics.push_back("Undeclared or foreign data-model dependency: " + name);
        if (!dom || !files->diagnostics.empty() || !system->diagnostics.empty()) {
            e = "UI document load failed: " + d.asset.path;
            for (auto& diagnostic : files->diagnostics) e += "\n" + diagnostic;
            for (auto& diagnostic : system->diagnostics) e += "\n" + diagnostic;
            return nullptr;
        }
        retirement.committed = true;
        return dom;
    }
    void RestoreStyles(Document& d) {
        Walk(d.dom, [&](Rml::Element* e) { if (auto it = d.hudStyles.find(e); it != d.hudStyles.end() && it->second.element.get() == e) {
            if (it->second.pointer) e->SetProperty(Rml::PropertyId::PointerEvents, *it->second.pointer); else e->RemoveProperty(Rml::PropertyId::PointerEvents);
            if (it->second.focus) e->SetProperty(Rml::PropertyId::Focus, *it->second.focus); else e->RemoveProperty(Rml::PropertyId::Focus);
        } }); d.hudStyles.clear();
    }
    void ApplyMode(Document& d) {
        if (d.mode == UiDocumentMode::Hud) {
            std::erase_if(d.hudStyles, [](const auto& entry) { return !entry.second.element; });
            Walk(d.dom, [&](Rml::Element* e) {
                if (!d.hudStyles.contains(e)) { Document::Styles styles; styles.element = e->GetObserverPtr(); auto& local = e->GetLocalStyleProperties(); if (auto it = local.find(Rml::PropertyId::PointerEvents); it != local.end()) styles.pointer = it->second; if (auto it = local.find(Rml::PropertyId::Focus); it != local.end()) styles.focus = it->second; d.hudStyles.emplace(e, std::move(styles)); }
                e->SetProperty("pointer-events", "none"); e->SetProperty("focus", "none");
            });
        }
    }
    void PresentDocument(Document& d) {
        ApplyMode(d);
        if (!d.visible) return;
        d.dom->Show(d.mode == UiDocumentMode::Modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None, d.mode == UiDocumentMode::Hud ? Rml::FocusFlag::None : d.mode == UiDocumentMode::Modal ? Rml::FocusFlag::Auto : Rml::FocusFlag::None);
        if (d.mode == UiDocumentMode::Modal) d.dom->PullToFront();
    }
    void RestoreFocus(Document& d) {
        if (auto previous = Doc(d.previousFocusDocument); previous && previous->visible && previous->mode != UiDocumentMode::Hud && OwnerLive(previous->owner, true)) {
            if (auto element = previous->dom->GetElementById(d.previousFocusId)) element->Focus(); else previous->dom->Focus();
        }
    }
    void Reload(Document& d) {
        d.reload = false; std::string error;
        auto found = engine->GetAssets().GetCatalog().Find(d.asset.metadata.id);
        if (!found || found->metadata.kind != CatalogAssetKind::UiDocument) { Logger().error("UI reload rolled back: source UUID no longer resolves ({})", d.asset.path); return; }
        Document candidate; candidate.owner = d.owner; candidate.desc = d.desc; candidate.asset = *found;
        // Allocate restoration state before accepting a candidate. Otherwise an
        // allocation failure here leaves an untracked DOM attached to the context.
        std::string focusedId; if (auto focused = context->GetFocusElement(); focused && focused->GetOwnerDocument() == d.dom) focusedId = focused->GetId();
        std::map<std::string, std::pair<float, float>> scroll;
        Walk(d.dom, [&](Rml::Element* e) { if (!e->GetId().empty()) scroll[e->GetId()] = {e->GetScrollLeft(), e->GetScrollTop()}; });
        auto next = Candidate(candidate, error); if (!next) { { Traverse guard(*this); context->Update(); pendingDomClose = false; } Logger().error("UI reload rolled back: {}", error); return; }
        for (auto& slot : subscriptions.slots) if (slot->record && slot->record->document == d.token) Detach(*slot->record);
        { Traverse guard(*this); d.dom->Close(); }
        d.dom = next; d.asset = std::move(candidate.asset); ++d.revision; d.hudStyles.clear();
        for (auto& slot : elements.slots) if (slot->record && slot->record->document == d.token) slot->record->live = false;
        std::erase_if(events, [&](const auto& e) { return e.document == d.token; });
        { Traverse guard(*this); PresentDocument(d); d.dom->UpdateDocument();
            for (auto& [id, pos] : scroll) if (auto e = d.dom->GetElementById(id)) { e->SetScrollLeft(pos.first); e->SetScrollTop(pos.second); }
            if (!focusedId.empty() && d.visible && d.mode != UiDocumentMode::Hud) if (auto e = d.dom->GetElementById(focusedId)) e->Focus();
        }
        for (auto& slot : subscriptions.slots) if (slot->record && slot->record->document == d.token && slot->record->live) Attach(*slot->record);
        { Traverse guard(*this); context->Update(); }
        Logger().info("UI reloaded {}", d.asset.path);
    }
    bool Depended(UiModel model) const {
        for (auto& slot : documents.slots) if (auto d = slot->record.get(); d && d->dom && std::find(d->desc.models.begin(), d->desc.models.end(), model) != d->desc.models.end()) return true;
        return false;
    }
    void Drain() {
        if (traversal || callbackDepth || draining || !context) return;
        draining = true;
        struct ResetDraining { bool& flag; ~ResetDraining() { flag = false; } } resetDraining{draining};
        for (auto& slot : subscriptions.slots) if (slot->record && !slot->record->live) Detach(*slot->record);
        bool closed = false;
        for (auto& slot : documents.slots) if (auto d = slot->record.get(); d && !d->live && d->dom) {
            Traverse guard(*this); d->dom->Hide(); RestoreFocus(*d); d->dom->Close(); d->dom = nullptr; closed = true;
        }
        // Closed Rml DOMs remain owned by Context until Update. Model storage and
        // definitions MUST outlive that boundary, including failed reload candidates.
        if (closed || pendingDomClose) { Traverse guard(*this); context->Update(); pendingDomClose = false; }
        for (std::size_t i = 0; i < documents.slots.size(); ++i) { auto d = documents.slots[i]->record.get(); if (d && !d->live) documents.Erase(i); }
        for (std::size_t i = 0; i < subscriptions.slots.size(); ++i) { auto s = subscriptions.slots[i]->record.get(); if (s && !s->live) subscriptions.Erase(i); }
        for (std::size_t i = 0; i < elements.slots.size(); ++i) {
            auto e = elements.slots[i]->record.get();
            // Rml data-for/layout can remove nodes without SetInnerRml. Observer
            // invalidation must reclaim their slots, not just make IsValid fail.
            if (e && (!e->live || !e->dom || !Doc(e->document))) elements.Erase(i);
        }
        for (std::size_t i = 0; i < callbacks.slots.size(); ++i) { auto c = callbacks.slots[i]->record.get(); if (c && !c->live) callbacks.Erase(i); }
        // Reload first: candidates also depend on the registered model definitions.
        auto count = documents.slots.size();
        for (std::size_t i = 0; i < count; ++i) if (auto d = documents.slots[i]->record.get(); d && d->live && d->reload) Reload(*d);
        for (std::size_t i = 0; i < models.slots.size(); ++i) if (auto m = models.slots[i]->record.get(); m && !m->live) {
            auto& slot = *models.slots[i]; UiModel token; token.session_ = slot.session; token.slot_ = static_cast<std::uint32_t>(i); token.generation_ = slot.generation;
            if (!Depended(token)) { Traverse guard(*this); context->RemoveDataModel(m->name); models.Erase(i); }
        }
        for (std::size_t i = 0; i < owners.slots.size(); ++i) if (auto o = owners.slots[i]->record.get(); o && !o->live) owners.Erase(i);
        for (std::size_t i = 0; i < sessions.slots.size(); ++i) if (auto s = sessions.slots[i]->record.get(); s && !s->live) sessions.Erase(i);
        std::erase_if(events, [&](const auto& q) { auto s = Sub(q.subscription); auto d = Doc(q.document); return !s || !d || d->revision != q.revision || !Deliverable(q.owner); });
        draining = false; RefreshCapture();
    }
    void RegisterFonts() {
        std::vector<CatalogEntry> entries;
        for (auto entry : engine->GetAssets().GetCatalog().List(CatalogAssetKind::Font)) entries.push_back(*entry);
        for (auto& entry : entries) if (!fonts.contains(entry.path)) {
            if (Rml::LoadFontFace(entry.path)) fonts.insert(entry.path);
            else Logger().warn("Failed to register catalog UI font: {}", entry.path);
        }
    }
    Document* DocumentFor(Rml::Element* element) const {
        if (!element) return nullptr; auto dom = element->GetOwnerDocument();
        for (auto& slot : documents.slots) if (auto d = slot->record.get(); d && d->live && d->dom == dom && d->visible && OwnerLive(d->owner, true)) return d;
        return nullptr;
    }
    bool PointerHit() const {
        auto hover = context ? context->GetHoverElement() : nullptr; auto doc = DocumentFor(hover);
        // A full-screen document body is an incidental layout box, not an authored
        // hit region. Descendants remain interactive, including anonymous controls.
        return (doc && doc->mode != UiDocumentMode::Hud && hover != doc->dom) || DebuggerElement(hover);
    }
    bool DebuggerElement(Rml::Element* element) const {
        auto doc = element ? element->GetOwnerDocument() : nullptr;
        return debugger && doc && doc->IsVisible() && doc->GetId().starts_with("rmlui-debug-") && doc->GetId() != "rmlui-debug-hook";
    }
    void RefreshCapture() noexcept {
        UiCaptureState next;
        if (initialized && !stopping && context && focused) {
            bool modal = false, interactive = false;
            for (auto& slot : documents.slots) if (auto d = slot->record.get(); d && d->live && d->visible && OwnerLive(d->owner, true)) {
                modal |= d->mode == UiDocumentMode::Modal;
                interactive |= d->mode == UiDocumentMode::Interactive;
            }
            auto focus = DocumentFor(context->GetFocusElement());
            auto debuggerFocus = DebuggerElement(context->GetFocusElement());
            next.textInput = system && system->text && ((focus && focus->mode != UiDocumentMode::Hud) || debuggerFocus);
            next.keyboard = modal || next.textInput || debuggerFocus || (focus && focus->mode != UiDocumentMode::Hud && context->GetFocusElement() != focus->dom);
            next.pointer = modal || dragging || PointerHit(); next.wantsVisibleCursor = interactive || modal || next.pointer || next.textInput;
        }
        capture = next;
        if (input) input->SetUiCapture(next.keyboard, next.pointer);
        if (system) system->host->SetCapture(next);
    }
    void ResetInput() {
        if (!context || !system) return;
        auto previousSuppression = suppressEvents; suppressEvents = true;
        system->Cancel();
        { Traverse guard(*this); for (int key : pressedKeys) context->ProcessKeyUp(static_cast<Rml::Input::KeyIdentifier>(key), 0);
            for (int button : pressedButtons) context->ProcessMouseButtonUp(button, 0);
            context->ProcessMouseLeave(); if (auto e = context->GetFocusElement()) e->Blur(); }
        pressedKeys.clear(); pressedButtons.clear(); dragging = false; events.clear(); suppressEvents = previousSuppression; RefreshCapture();
    }
    void ProcessInput(const InputUiEvent& e) {
        system->Publish(context);
#ifndef NDEBUG
        if (auto key = std::get_if<InputUiKey>(&e.payload); key && key->logical == Key::F8 && key->down && !key->repeat && debugger && !system->text) {
            Rml::Debugger::SetVisible(!Rml::Debugger::IsVisible()); if (input) input->ConsumeUiEvent(e.sequence); return;
        }
#endif
        bool pointerEvent = std::holds_alternative<InputUiPointerMove>(e.payload) || std::holds_alternative<InputUiPointerButton>(e.payload) || std::holds_alternative<InputUiWheel>(e.payload);
        bool consume = pointerEvent ? capture.pointer : capture.keyboard;
        inputSequence = e.sequence;
        {
            Traverse guard(*this); int mods = Modifiers(e.modifiers);
            if (e.hasPointerPosition && std::isfinite(e.pointerX) && std::isfinite(e.pointerY)) context->ProcessMouseMove(static_cast<int>(std::clamp(e.pointerX, float(INT_MIN / 2), float(INT_MAX / 2))), static_cast<int>(std::clamp(e.pointerY, float(INT_MIN / 2), float(INT_MAX / 2))), mods);
            if (auto key = std::get_if<InputUiKey>(&e.payload)) {
                auto translated = TranslateKey(*key); if (translated != Rml::Input::KI_UNKNOWN) {
                    if (key->down) { pressedKeys.insert(translated); context->ProcessKeyDown(translated, mods); }
                    else { pressedKeys.erase(translated); context->ProcessKeyUp(translated, mods); }
                }
            } else if (auto button = std::get_if<InputUiPointerButton>(&e.payload)) {
                int index = static_cast<int>(button->button); if (index >= 0 && index < int(MouseButton::Count)) {
                    if (button->down) { pressedButtons.insert(index); if (PointerHit() || capture.pointer) dragging = true; context->ProcessMouseButtonDown(index, mods); }
                    else { pressedButtons.erase(index); context->ProcessMouseButtonUp(index, mods); if (pressedButtons.empty()) dragging = false; }
                }
            } else if (auto wheel = std::get_if<InputUiWheel>(&e.payload)) { if (std::isfinite(wheel->horizontal) && std::isfinite(wheel->vertical)) context->ProcessMouseWheel({-wheel->horizontal, -wheel->vertical}, mods); }
            else if (auto text = std::get_if<InputUiText>(&e.payload)) { if (!system->composing && ui_detail::Utf8(text->utf8)) context->ProcessTextInput(text->utf8); }
            else if (auto focus = std::get_if<InputUiFocus>(&e.payload)) { focused = focus->focused; if (!focused) ResetInput(); }
            else if (auto composition = std::get_if<InputUiComposition>(&e.payload)) system->Compose(*composition);
        }
        inputSequence = 0;
        RefreshCapture(); consume = consume || (pointerEvent ? capture.pointer : capture.keyboard);
        // Only the events generated by this input traversal dispatch now. Events from
        // callbacks, layout or rendering stay queued for the next Update wave.
        std::deque<QueuedEvent> wave;
        for (auto it = events.begin(); it != events.end();) if (it->event.inputSequence == e.sequence) { wave.push_back(std::move(*it)); it = events.erase(it); } else ++it;
        Dispatch(std::move(wave)); Drain(); if (!stopping) system->Publish(context); RefreshCapture();
        consume = consume || (pointerEvent ? capture.pointer : capture.keyboard);
        if (consume && input) input->ConsumeUiEvent(e.sequence);
    }
};

UiSubsystem::UiSubsystem(UiSubsystemConfig config) : impl_(std::make_unique<Impl>(*this, std::move(config))) {}
UiSubsystem::~UiSubsystem() { Shutdown(); }
std::string_view UiSubsystem::GetName() const { return "UI"; }
bool UiSubsystem::Initialize() {
    auto& p = *impl_;
    // Shutdown from a callback only retires logically until Update reaches its
    // safe boundary. Do not report a usable runtime or reacquire the global lease.
    if (p.leased && p.stopping) return false;
    if (p.initialized) return true;
    {
        std::lock_guard lock(runtimeMutex);
        // The exported Rml DLL is shared even if a process embeds both static and
        // shared engine builds. Inspect its globals before installing any interfaces.
        if (runtimeLeased || Rml::GetSystemInterface() || Rml::GetRenderInterface() ||
            Rml::GetFileInterface() || Rml::GetFontEngineInterface() || Rml::GetTextInputHandler()) {
            Logger().error("Only one RmlUi runtime may be active in this process; interfaces already owned");
            return false;
        }
        runtimeLeased = p.leased = true;
    }
    try {
        p.engine = TryGetEngine(); if (!p.engine) throw std::runtime_error("UI requires an attached Engine");
        p.stopping = false; p.focused = true;
        p.input = p.engine->GetSubsystem<InputSubsystem>();
        auto host = p.config.host ? p.config.host : std::make_shared<ui_detail::NullHost>();
        p.system = std::make_unique<ui_detail::System>(std::move(host)); p.files = std::make_unique<ui_detail::Files>();
        if (p.config.renderer == UiRendererBackend::Null) p.renderer = ui_detail::CreateNullRenderer();
        else {
            p.rendering = p.engine->GetSubsystem<RenderingSubsystem>(); if (!p.rendering) throw std::runtime_error("DX11 UI requires RenderingSubsystem before UI");
            std::string error; p.renderer = CreateUiDx11Renderer(p.rendering->GetContext(), error); if (!p.renderer) throw std::runtime_error(error);
            auto& surface = p.rendering->GetSurface(); p.metrics.pixelWidth = surface.width; p.metrics.pixelHeight = surface.height;
        }
        Rml::SetSystemInterface(p.system.get()); Rml::SetFileInterface(p.files.get()); Rml::SetRenderInterface(p.renderer.get()); Rml::SetTextInputHandler(p.system.get());
        if (!Rml::Initialise()) throw std::runtime_error("RmlUi initialization failed"); p.core = true;
        p.context = Rml::CreateContext("engine-ui", {static_cast<int>(std::min(p.metrics.pixelWidth, std::uint32_t(INT_MAX))), static_cast<int>(std::min(p.metrics.pixelHeight, std::uint32_t(INT_MAX)))});
        if (!p.context) throw std::runtime_error("RmlUi context creation failed");
        SetSurfaceMetrics(p.metrics);
        // Register catalog fonts before any document. The bootstrap also works during
        // metadata generation, while still resolving through the root-contained adapter.
        const auto& catalog = p.engine->GetAssets().GetCatalog();
        std::filesystem::path font; std::string name;
        if (auto entry = catalog.FindPath("engine/ui/fonts/DejaVuSans.ttf")) { auto copy = *entry; name = copy.path; }
        else name = "engine/ui/fonts/DejaVuSans.ttf";
        if (ui_detail::ResolveAsset(name, font, name) && std::filesystem::is_regular_file(font)) { if (!Rml::LoadFontFace(name, true)) Logger().warn("Default UI font failed: {}", name); else p.fonts.insert(name); }
        else Logger().warn("Default UI font is unavailable: {}", name);
        p.context->GetRootElement()->SetProperty("font-family", "DejaVu Sans");
        p.context->GetRootElement()->SetProperty("font-size", "16dp");
        p.RegisterFonts();
        // Resolve root inherited defaults before synchronous candidate layout. Otherwise
        // the first text document sees an unresolved font handle during OnStart.
        p.context->Update();
#ifndef NDEBUG
        if (p.config.enableDebugger) { p.debugger = Rml::Debugger::Initialise(p.context); if (!p.debugger) Logger().warn("RmlUi debugger initialization failed"); else Rml::Debugger::SetVisible(false); }
#else
        if (p.config.enableDebugger) Logger().warn("UI debugger is disabled in this build");
#endif
        p.lifetime = std::make_shared<int>(0); p.initialized = true;
        if (p.rendering) {
            std::weak_ptr<void> weak = p.lifetime;
            p.rendering->SetOverlayCallbacks([weak, this](RenderingContext& context, const FrameContext& frame) { if (auto lease = weak.lock()) UiRuntimeAccess::RenderOverlay(*this, context, frame); }, [weak, this]() { if (auto lease = weak.lock()) UiRuntimeAccess::BeforeSurfaceResize(*this); });
            p.overlayAttached = true;
        }
        p.RefreshCapture(); return true;
    } catch (const std::exception& ex) { Logger().error("UI initialization: {}", ex.what()); Shutdown(); return false; }
    catch (...) { Logger().error("UI initialization failed"); Shutdown(); return false; }
}
void UiSubsystem::Shutdown() {
    auto& p = *impl_; p.lifetime.reset(); if (!p.leased) return;
    p.stopping = true;
    if (p.rendering && p.overlayAttached) { p.rendering->ClearOverlayCallbacks(); p.overlayAttached = false; }
    for (std::size_t i = 0; i < p.sessions.slots.size(); ++i) if (auto& slot = *p.sessions.slots[i]; slot.record && slot.record->live) { UiSession s; s.session_ = slot.session; s.slot_ = static_cast<std::uint32_t>(i); s.generation_ = slot.generation; p.EndSession(s); }
    p.RefreshCapture();
    if (p.traversal || p.callbackDepth) return; // Update completes physical shutdown at its safe boundary.
    if (p.system) p.system->Cancel();
    p.events.clear();
    if (p.context) { Rml::RemoveContext("engine-ui"); p.context = nullptr; }
    p.subscriptions.slots.clear(); p.elements.slots.clear(); p.documents.slots.clear(); p.models.slots.clear(); p.callbacks.slots.clear(); p.owners.slots.clear(); p.sessions.slots.clear();
    p.subscriptions.tombstones.clear(); p.documents.tombstones.clear();
    if (p.core) { Rml::Shutdown(); p.core = false; }
    Rml::SetTextInputHandler(nullptr); Rml::SetRenderInterface(nullptr); Rml::SetFileInterface(nullptr); Rml::SetSystemInterface(nullptr);
    p.renderer.reset(); p.files.reset(); p.system.reset(); p.initialized = false; p.debugger = false; p.rendering = nullptr; p.input = nullptr; p.engine = nullptr;
    p.pressedKeys.clear(); p.pressedButtons.clear(); p.fonts.clear(); p.dragging = false;
    p.pendingDomClose = p.suppressEvents = p.draining = false;
    { std::lock_guard lock(runtimeMutex); runtimeLeased = false; p.leased = false; }
}
bool UiSubsystem::IsInitialized() const noexcept { return impl_->initialized && !impl_->stopping; }
std::weak_ptr<void> UiSubsystem::GetLifetimeToken() const noexcept { return impl_->lifetime; }
void UiSubsystem::SetSurfaceMetrics(UiSurfaceMetrics metrics) {
    auto& p = *impl_; if (!std::isfinite(metrics.dpRatio) || metrics.dpRatio <= 0) metrics.dpRatio = 1;
    p.metrics = metrics;
    if (p.context) { Impl::Traverse guard(p); p.context->SetDimensions({static_cast<int>(std::min(metrics.pixelWidth, std::uint32_t(INT_MAX))), static_cast<int>(std::min(metrics.pixelHeight, std::uint32_t(INT_MAX)))}); p.context->SetDensityIndependentPixelRatio(metrics.dpRatio); }
}
UiCaptureState UiSubsystem::GetCaptureState() const noexcept { return impl_->capture; }
void UiSubsystem::Update(const FrameContext& frame) {
    auto& p = *impl_; if (!p.initialized) return; if (p.stopping) { Shutdown(); return; }
    if (p.rendering) {
        const auto& surface = p.rendering->GetSurface();
        if (p.metrics.pixelWidth != surface.width || p.metrics.pixelHeight != surface.height)
            SetSurfaceMetrics({surface.width, surface.height, p.metrics.dpRatio});
    }
    p.system->time = frame.realTimeSeconds; p.Drain();
    std::deque<Impl::QueuedEvent> wave; wave.swap(p.events); p.Dispatch(std::move(wave)); p.Drain();
    if (!p.stopping && p.input) {
        // Callbacks may Reset InputSubsystem (e.g. stopping Play), invalidating its
        // borrowed span and the strings/IME vectors within the current event.
        const auto latched = p.input->GetUiEvents();
        const std::vector<InputUiEvent> batch(latched.begin(), latched.end());
        for (const auto& event : batch) {
            // A reset also cancels the rest of the old batch, not just its storage.
            const auto current = p.input->GetUiEvents();
            if (current.size() != batch.size() || current.empty() ||
                current.front().sequence != batch.front().sequence || current.back().sequence != batch.back().sequence) break;
            p.ProcessInput(event);
            if (p.stopping) break;
        }
    }
    if (p.stopping) { Shutdown(); return; } p.system->Publish(p.context); p.RefreshCapture();
}
void UiSubsystem::LateUpdate(const FrameContext& frame) {
    auto& p = *impl_; if (!IsInitialized()) return; p.system->time = frame.realTimeSeconds; p.Drain();
    for (auto& slot : p.models.slots) if (auto model = slot->record.get(); model && model->live) for (auto& name : model->storage->TakeDirtyVariables()) model->handle.DirtyVariable(name);
    { Impl::Traverse guard(p); for (auto& slot : p.documents.slots) if (slot->record && slot->record->live) p.ApplyMode(*slot->record); p.context->Update(); for (auto& slot : p.documents.slots) if (slot->record && slot->record->live) { p.ApplyMode(*slot->record); if (slot->record->mode == UiDocumentMode::Hud) slot->record->dom->UpdateDocument(); } }
    for (auto& slot : p.subscriptions.slots) if (slot->record && slot->record->live) p.Attach(*slot->record);
    p.system->Publish(p.context); p.RefreshCapture();
}
void UiSubsystem::Render(const FrameContext&) {}
void UiSubsystem::ResetSimulation() { impl_->ResetInput(); }
bool UiSubsystem::BeginNativeSession(UiSession& out, std::string& e) { return impl_->BeginSession(true, {}, out, e); }
bool UiSubsystem::CreateNativeOwner(UiSession session, std::string_view name, UiOwner& out, std::string& e) { return impl_->CreateOwner(session, name, true, out, e); }
void UiSubsystem::EndNativeSession(UiSession session) noexcept { auto s = impl_->sessions.Get(session); if (s && s->native) impl_->EndSession(session); }
void UiSubsystem::ReleaseNativeOwner(UiOwner owner) noexcept { auto o = impl_->owners.Get(owner); if (o && o->native) { impl_->Teardown(owner, true); impl_->Drain(); } }

bool UiSubsystem::LoadDocument(UiOwner owner, std::string_view path, const UiDocumentDesc& desc, UiDocument& out, std::string& e) {
    auto& p = *impl_; if (!p.CheckOwner(owner, e, true)) return false;
    auto found = p.engine->GetAssets().GetCatalog().FindPath(path); if (!found) { e = "UI document is not catalogued: " + std::string(path); return false; }
    auto entry = *found; return LoadDocument(owner, AssetReference{entry.metadata.id, entry.path}, desc, out, e);
}
bool UiSubsystem::LoadDocument(UiOwner owner, const AssetReference& reference, const UiDocumentDesc& desc, UiDocument& out, std::string& e) {
    auto& p = *impl_; if (!p.CheckOwner(owner, e, true)) return false;
    auto found = p.engine->GetAssets().GetCatalog().Find(reference.id); if (!found) { e = "UI document UUID not found"; return false; }
    auto entry = *found; if (entry.metadata.kind != CatalogAssetKind::UiDocument) { e = "UI asset is not an RML document: " + entry.path; return false; }
    auto d = std::make_unique<Impl::Document>(); d->owner = owner; d->asset = std::move(entry); d->desc = desc;
    d->dom = p.Candidate(*d, e); if (!d->dom) { Impl::Traverse guard(p); p.context->Update(); return false; }
    auto dom = d->dom;
    try {
        auto token = p.documents.Add(owner.session_, std::move(d)); p.documents.Get(token)->token = token; out = token; return true;
    } catch (...) {
        // Table allocation can fail after the candidate was accepted. Rml still
        // owns it; close it before dependent model storage is eligible for release.
        Impl::Traverse guard(p); dom->Close(); p.pendingDomClose = true; throw;
    }
}
bool UiSubsystem::Show(UiOwner owner, UiDocument token, UiDocumentMode mode, std::string& e) {
    auto& p = *impl_; auto d = p.Owned(owner, token, e); if (!d) return false;
    if (mode != UiDocumentMode::Hud && mode != UiDocumentMode::Interactive && mode != UiDocumentMode::Modal) { e = "Unknown UI document mode"; return false; }
    Impl::Traverse guard(p);
    if (mode == UiDocumentMode::Modal && (!d->visible || d->mode != mode)) {
        if (auto focused = p.context->GetFocusElement()) if (auto previous = p.DocumentFor(focused); previous && previous != d) { d->previousFocusDocument = previous->token; d->previousFocusId = focused->GetId(); }
    }
    p.RestoreStyles(*d); d->mode = mode; d->visible = true;
    if (mode == UiDocumentMode::Hud) p.context->UnfocusDocument(d->dom);
    p.PresentDocument(*d); d->dom->UpdateDocument(); p.RefreshCapture(); return true;
}
bool UiSubsystem::Hide(UiOwner owner, UiDocument token, std::string& e) {
    auto& p = *impl_; auto d = p.Owned(owner, token, e); if (!d) return false;
    { Impl::Traverse guard(p); d->visible = false; if (p.system->text && p.context->GetFocusElement() && p.context->GetFocusElement()->GetOwnerDocument() == d->dom) p.system->Cancel(); d->dom->Hide(); p.RestoreFocus(*d); }
    p.dragging = false; p.RefreshCapture(); return true;
}
bool UiSubsystem::Close(UiOwner owner, UiDocument token, std::string& e) {
    auto& p = *impl_; if (p.documents.Was(token, owner)) return true;
    auto d = p.Owned(owner, token, e); if (!d) return false; p.Close(*d); std::erase_if(p.events, [&](const auto& q) { return q.document == token; }); p.dragging = false; p.Drain(); p.RefreshCapture(); return true;
}
bool UiSubsystem::RequestReload(UiOwner owner, UiDocument token, std::string& e) { auto d = impl_->Owned(owner, token, e); if (!d) return false; d->reload = true; return true; }
bool UiSubsystem::IsValid(UiDocument token) const noexcept { return impl_->Doc(token) != nullptr; }
bool UiSubsystem::IsVisible(UiDocument token) const noexcept { auto d = impl_->Doc(token); return d && d->visible; }

bool UiSubsystem::CreateModel(UiOwner owner, const UiModelDesc& desc, UiModel& out, std::string& e) {
    auto& p = *impl_; if (!p.CheckOwner(owner, e, true)) return false;
    if (!ui_detail::Identifier(desc.name) || desc.schema.kind != UiValueKind::Object) { e = "UI model requires a valid name and Object root schema"; return false; }
    if (!ValidateUiValue(desc.schema, desc.initialValues, e)) return false;
    for (auto& slot : p.models.slots) if (slot->record && slot->record->name == desc.name) { e = "UI model name is already registered: " + desc.name; return false; }
    auto m = std::make_unique<Impl::Model>(); m->owner = owner; m->name = desc.name; m->storage = std::make_shared<ui_detail::ModelStorage>(desc.schema, desc.initialValues);
    // Pin reflected definitions even if Table::Add consumes/destroys the record
    // while throwing. Unregister Rml before the last storage reference can die.
    const auto storage = m->storage;
    auto constructor = p.context->CreateDataModel(desc.name); if (!constructor) { e = "RmlUi refused UI model: " + desc.name; return false; }
    try {
        if (!storage->Bind(constructor, e)) { p.context->RemoveDataModel(desc.name); return false; }
        m->handle = constructor.GetModelHandle(); auto token = p.models.Add(owner.session_, std::move(m)); out = token; return true;
    } catch (...) {
        p.context->RemoveDataModel(desc.name);
        throw;
    }
}
bool UiSubsystem::SetModelValue(UiOwner owner, UiModel token, std::string_view variable, const StateValue& value, std::string& e) { auto m = impl_->Owned(owner, token, e); UiModelPath path{std::string(variable)}; return m && m->storage->Write(path, value, e); }
bool UiSubsystem::GetModelValue(UiOwner owner, UiModel token, std::string_view variable, StateValue& out, std::string& e) const { auto m = impl_->Owned(owner, token, e); UiModelPath path{std::string(variable)}; return m && m->storage->Read(path, out, e); }
bool UiSubsystem::RemoveModel(UiOwner owner, UiModel token, std::string& e) { auto& p = *impl_; auto m = p.Owned(owner, token, e); if (!m) return false; if (p.Depended(token)) { e = "UI model still has dependent documents"; return false; } m->live = false; p.Drain(); return true; }
bool UiSubsystem::IsValid(UiModel token) const noexcept { return impl_->Mod(token) != nullptr; }

bool UiSubsystem::FindElement(UiOwner owner, UiDocument token, std::string_view id, UiElement& out, std::string& e) {
    auto& p = *impl_; auto d = p.Owned(owner, token, e); if (!d) return false;
    auto dom = id.empty() ? nullptr : d->dom->GetElementById(std::string(id));
    if (!dom) { e = "UI element ID not found: " + std::string(id); return false; }
    // Handles are non-owning, with no release operation. Intern lookups so ordinary
    // doc:GetElement calls each frame cannot allocate one permanent slot per call.
    for (std::size_t i = 0; i < p.elements.slots.size(); ++i) {
        auto& slot = *p.elements.slots[i]; auto element = slot.record.get();
        if (element && element->live && element->owner == owner && element->document == token &&
            element->revision == d->revision && element->id == id && element->dom.get() == dom) {
            UiElement handle; handle.session_ = slot.session; handle.slot_ = static_cast<std::uint32_t>(i); handle.generation_ = slot.generation;
            out = handle; return true;
        }
    }
    auto element = std::make_unique<Impl::Element>(); element->owner = owner; element->document = token; element->revision = d->revision; element->id = id; element->dom = dom->GetObserverPtr();
    out = p.elements.Add(owner.session_, std::move(element)); return true;
}
bool UiSubsystem::IsValid(UiElement token) const noexcept { auto element = impl_->elements.Get(token); if (!element) return false; std::string e; return impl_->Owned(element->owner, token, e) != nullptr; }
bool UiSubsystem::SetText(UiOwner owner, UiElement token, std::string_view text, std::string& e) {
    if (!ui_detail::Utf8(text)) { e = "UI text must be valid UTF-8"; return false; }
    // A native text node also keeps data-expression delimiters literal. Reuse the
    // structural mutation path for descendant invalidation and listener rebinding.
    if (!SetInnerRml(owner, token, "", e)) return false;
    auto& p = *impl_; auto element = p.Owned(owner, token, e); if (!element) return false;
    auto d = p.Doc(p.elements.Get(token)->document); Impl::Traverse guard(p);
    element->AppendChild(d->dom->CreateTextNode(std::string(text))); p.ApplyMode(*d); return true;
}
bool UiSubsystem::SetInnerRml(UiOwner owner, UiElement token, std::string_view markup, std::string& e) {
    auto& p = *impl_; auto element = p.Owned(owner, token, e); if (!element) return false;
    if (!ui_detail::Utf8(markup)) { e = "UI markup must be valid UTF-8"; return false; }
    auto record = p.elements.Get(token); auto d = p.Doc(record->document);
    std::set<std::string> allowed; if (!p.Dependencies(d->desc, owner, allowed, e)) return false;
    std::set<std::string> referenced;
    if (!ui_detail::InspectMarkup("<body>" + std::string(markup) + "</body>", referenced, e)) return false;
    for (auto& name : referenced) if (!allowed.contains(name)) { e = "Inserted markup has an undeclared data-model: " + name; return false; }
    for (auto& slot : p.subscriptions.slots) if (slot->record && slot->record->document == record->document) p.Detach(*slot->record);
    // Only removed descendants expire; the target and unrelated element handles survive.
    for (auto& slot : p.elements.slots) if (auto child = slot->record.get(); child && child->live && child->document == record->document) {
        auto dom = d->dom->GetElementById(child->id); for (auto parent = dom ? dom->GetParentNode() : nullptr; parent; parent = parent->GetParentNode()) if (parent == element) { child->live = false; break; }
    }
    ++d->revision;
    for (auto& slot : p.elements.slots) if (slot->record && slot->record->live && slot->record->document == record->document) slot->record->revision = d->revision;
    { Impl::Traverse guard(p); element->SetInnerRML(std::string(markup)); p.ApplyMode(*d); d->dom->UpdateDocument(); }
    for (auto& slot : p.subscriptions.slots) if (slot->record && slot->record->document == record->document && slot->record->live) p.Attach(*slot->record);
    p.RefreshCapture(); return true;
}
bool UiSubsystem::SetFormValue(UiOwner owner, UiElement token, std::string_view value, std::string& e) { auto& p = *impl_; auto element = p.Owned(owner, token, e); if (!element) return false; auto control = dynamic_cast<Rml::ElementFormControl*>(element); if (!control || !ui_detail::Utf8(value)) { e = "UI element is not a form control or value is invalid UTF-8"; return false; } Impl::Traverse guard(p); control->SetValue(std::string(value)); Rml::Dictionary parameters; parameters["value"] = Rml::Variant(control->GetValue()); control->DispatchEvent("change", parameters); return true; }
bool UiSubsystem::GetFormValue(UiOwner owner, UiElement token, std::string& out, std::string& e) const { auto element = impl_->Owned(owner, token, e); if (!element) return false; auto control = dynamic_cast<Rml::ElementFormControl*>(element); if (!control) { e = "UI element is not a form control"; return false; } out = control->GetValue(); return true; }
bool UiSubsystem::SetClass(UiOwner owner, UiElement token, std::string_view name, bool enabled, std::string& e) { auto& p = *impl_; auto element = p.Owned(owner, token, e); if (!element) return false; Impl::Traverse guard(p); element->SetClass(std::string(name), enabled); return true; }
bool UiSubsystem::SetProperty(UiOwner owner, UiElement token, std::string_view name, std::string_view value, std::string& e) { auto& p = *impl_; auto element = p.Owned(owner, token, e); if (!element) return false; Impl::Traverse guard(p); if (!element->SetProperty(std::string(name), std::string(value))) { e = "Invalid RmlUi property/value: " + std::string(name); return false; } auto d = p.Doc(p.elements.Get(token)->document); p.ApplyMode(*d); return true; }
bool UiSubsystem::Focus(UiOwner owner, UiElement token, std::string& e) { auto& p = *impl_; auto element = p.Owned(owner, token, e); if (!element) return false; auto d = p.Doc(p.elements.Get(token)->document); if (!d->visible || d->mode == UiDocumentMode::Hud) { e = "Cannot focus a hidden or HUD document"; return false; } Impl::Traverse guard(p); if (!element->Focus()) { e = "RmlUi element rejected focus"; return false; } p.RefreshCapture(); return true; }
bool UiSubsystem::SubscribeNative(UiOwner owner, UiDocument document, std::string_view id, std::string_view event, NativeCallback callback, UiSubscription& out, std::string& e) { auto o = impl_->owners.Get(owner); if (!o || !o->native || !callback) { e = "Native subscription requires a native owner and callback"; return false; } return impl_->Subscribe(owner, document, id, event, std::move(callback), {}, out, e); }
bool UiSubsystem::Disconnect(UiOwner owner, UiSubscription token, std::string& e) { auto& p = *impl_; if (p.subscriptions.Was(token, owner)) return true; auto s = p.subscriptions.Get(token); if (!s || s->owner != owner) { e = "Invalid or foreign UI subscription"; return false; } p.Disconnect(*s); std::erase_if(p.events, [&](const auto& q) { return q.subscription == token; }); p.Drain(); return true; }
bool UiSubsystem::IsValid(UiSubscription token) const noexcept { return impl_->Sub(token) != nullptr; }

bool UiRuntimeAccess::BeginScriptSession(UiSubsystem& ui, std::weak_ptr<UiCallbackDispatcher> dispatcher, UiSession& out, std::string& e) { if (dispatcher.expired()) { e = "UI script dispatcher is unavailable"; return false; } return ui.impl_->BeginSession(false, std::move(dispatcher), out, e); }
bool UiRuntimeAccess::CreateScriptOwner(UiSubsystem& ui, UiSession session, std::string_view name, UiOwner& out, std::string& e) { return ui.impl_->CreateOwner(session, name, false, out, e); }
void UiRuntimeAccess::EnableScriptOwner(UiSubsystem& ui, UiOwner owner) noexcept { if (auto o = ui.impl_->owners.Get(owner); o && o->live && !o->retiring) o->enabled = true; }
void UiRuntimeAccess::BeginOwnerTeardown(UiSubsystem& ui, UiOwner owner) noexcept { ui.impl_->Teardown(owner, false); ui.impl_->Drain(); }
void UiRuntimeAccess::ReleaseOwner(UiSubsystem& ui, UiOwner owner) noexcept { ui.impl_->Teardown(owner, true); ui.impl_->Drain(); }
void UiRuntimeAccess::SuspendSession(UiSubsystem& ui, UiSession session) noexcept { ui.impl_->Suspend(session); }
void UiRuntimeAccess::EndSession(UiSubsystem& ui, UiSession session) noexcept { ui.impl_->EndSession(session); }
bool UiRuntimeAccess::ReserveCallback(UiSubsystem& ui, UiOwner owner, UiCallback& out, std::string& e) { auto& p = *ui.impl_; if (!p.CheckOwner(owner, e, true)) return false; auto o = p.owners.Get(owner); if (o->native) { e = "Script callback cannot belong to a native owner"; return false; } auto c = std::make_unique<UiSubsystem::Impl::Callback>(); c->owner = owner; out = p.callbacks.Add(owner.session_, std::move(c)); return true; }
void UiRuntimeAccess::CancelCallback(UiSubsystem& ui, UiOwner owner, UiCallback token) noexcept { auto c = ui.impl_->callbacks.Get(token); if (c && c->owner == owner) { ui.impl_->ReleaseCallback(token); ui.impl_->Drain(); } }
bool UiRuntimeAccess::Subscribe(UiSubsystem& ui, UiOwner owner, UiDocument document, std::string_view id, std::string_view event, UiCallback callback, UiSubscription& out, std::string& e) { auto& p = *ui.impl_; auto c = p.callbacks.Get(callback); if (!c || !c->live || c->owner != owner) { e = "Invalid or foreign UI callback reservation"; return false; } for (auto& slot : p.subscriptions.slots) if (slot->record && slot->record->live && slot->record->callback == callback) { e = "UI callback is already subscribed"; return false; } return p.Subscribe(owner, document, id, event, {}, callback, out, e); }
void UiRuntimeAccess::RenderOverlay(UiSubsystem& ui, RenderingContext& context, const FrameContext& frame) { auto& p = *ui.impl_; if (!ui.IsInitialized() || !p.context || !p.metrics.pixelWidth || !p.metrics.pixelHeight || p.config.renderer == UiRendererBackend::Null) return; p.system->time = frame.realTimeSeconds; UiDx11BeginFrame(*p.renderer, context, static_cast<int>(p.metrics.pixelWidth), static_cast<int>(p.metrics.pixelHeight)); try { UiSubsystem::Impl::Traverse guard(p); p.context->Render(); } catch (...) { UiDx11EndFrame(*p.renderer, context); throw; } UiDx11EndFrame(*p.renderer, context); }
void UiRuntimeAccess::BeforeSurfaceResize(UiSubsystem& ui) noexcept { auto& p = *ui.impl_; if (p.renderer && p.config.renderer == UiRendererBackend::Dx11) UiDx11ReleaseSurface(*p.renderer); }
bool UiRuntimeAccess::QueueEventForTesting(UiSubsystem& ui, UiSubscription token, const UiEvent& event, std::string& e) { auto& p = *ui.impl_; auto s = p.Sub(token); if (!s || !p.Deliverable(s->owner)) { e = "UI subscription is unavailable"; return false; } auto d = p.Doc(s->document); UiEvent copy = event; copy.document = s->document; copy.sourcePath = d->asset.path; copy.inputSequence = 0; if (copy.type.empty()) copy.type = s->event; if (copy.currentTargetId.empty()) copy.currentTargetId = s->id; p.events.push_back({token, s->document, s->owner, d->revision, std::move(copy), s->attachment}); return true; }
bool UiRuntimeAccess::WriteModelForTesting(UiSubsystem& ui, UiOwner owner, UiModel token, std::span<const UiModelPathPart> path, const StateValue& value, std::string& e) { auto m = ui.impl_->Owned(owner, token, e); return m && m->storage->Write(path, value, e); }
UiRuntimeStats UiRuntimeAccess::Inspect(const UiSubsystem& ui) noexcept {
    auto& p = *ui.impl_; UiRuntimeStats s;
    auto count = [](const auto& table) { std::size_t n = 0; for (auto& slot : table.slots) if (slot->record && slot->record->live) ++n; return n; };
    s.sessions = count(p.sessions); s.owners = count(p.owners); s.documents = count(p.documents); s.models = count(p.models); s.subscriptions = count(p.subscriptions); s.queuedEvents = p.events.size();
    for (auto& slot : p.documents.slots) if (slot->record && !slot->record->live) ++s.pendingRetirements;
    for (auto& slot : p.models.slots) if (slot->record && !slot->record->live) ++s.pendingRetirements;
    return s;
}
}
