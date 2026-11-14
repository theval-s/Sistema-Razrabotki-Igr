#include "UiInterfaces.hpp"
#include "UiModelStorage.hpp"
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/core/Utf.hpp>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/BaseXMLParser.h>
#include <RmlUi/Core/StreamMemory.h>
#include <cstring>
#include <algorithm>
#include <limits>
#include <climits>

namespace engine::ui_detail {
namespace {
bool Contained(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto p = path.begin(); for (auto r = root.begin(); r != root.end(); ++r, ++p) if (p == path.end() || *p != *r) return false; return true;
}
}
bool ResolveAsset(std::string_view url, std::filesystem::path& physical, std::string& logical) {
    try {
        std::string u(url); std::replace(u.begin(), u.end(), '\\', '/');
        if (u.find(':') != std::string::npos || u.find('\0') != std::string::npos) return false;
        bool game = u.starts_with("game/"); if (!game && !u.starts_with("engine/")) return false;
        auto p = FromUtf8(u).lexically_normal();
        auto name = ToGenericUtf8(p);
        if (!name.starts_with(game ? "game/" : "engine/")) return false;
        auto root = std::filesystem::weakly_canonical(game ? AssetPathResolver::GetGameRoot() : AssetPathResolver::GetRoot() / "engine");
        auto relative = name.substr(game ? 5 : 7);
        auto result = std::filesystem::weakly_canonical(root / FromUtf8(relative));
        if (!Contained(result, root)) return false;
        physical = std::move(result); logical = std::move(name); return true;
    } catch (const std::filesystem::filesystem_error&) { return false; }
}
std::string JoinAsset(std::string_view source, std::string_view relative) {
    std::string r(relative); std::replace(r.begin(), r.end(), '\\', '/');
    auto fragment = r.find('#'); if (fragment != std::string::npos) r.resize(fragment);
    if (!r.starts_with("game/") && !r.starts_with("engine/")) r = ToGenericUtf8(FromUtf8(source).parent_path() / FromUtf8(r));
    std::filesystem::path p; std::string logical; return ResolveAsset(r, p, logical) ? logical : "[invalid-ui-url]";
}
bool InspectMarkup(std::string_view source, std::set<std::string>& models, std::string& error) {
    error.clear();
    struct Inspector final : Rml::BaseXMLParser {
        std::set<std::string>& models; std::string& error;
        Inspector(std::set<std::string>& models, std::string& error) : models(models), error(error) { RegisterCDATATag("style"); RegisterCDATATag("script"); }
        void HandleElementStart(const Rml::String& name, const Rml::XMLAttributes& attrs) override {
            if (name == "script") error = "RML script blocks are disabled";
            if (auto it = attrs.find("data-model"); it != attrs.end()) models.insert(it->second.Get<Rml::String>());
        }
    } inspector(models, error);
    Rml::StreamMemory memory(reinterpret_cast<const Rml::byte*>(source.data()), source.size());
    inspector.Parse(&memory); return error.empty();
}
Rml::FileHandle Files::Open(const Rml::String& url) {
    std::filesystem::path p; std::string logical;
    auto fail = [&]() -> Rml::FileHandle { if (collecting) diagnostics.push_back("Cannot open UI resource: " + url); return 0; };
    if (!ResolveAsset(url, p, logical)) return fail();
    auto file = std::make_unique<File>(); file->stream.open(p, std::ios::binary);
    if (!file->stream) return fail();
    file->stream.seekg(0, std::ios::end); auto length = file->stream.tellg(); if (length < 0) return fail();
    file->length = static_cast<std::size_t>(length); file->stream.seekg(0);
    if (collecting && (p.extension() == ".rml" || p.extension() == ".rmlt")) {
        std::string source(file->length, '\0'); file->stream.read(source.data(), static_cast<std::streamsize>(source.size())); file->stream.clear(); file->stream.seekg(0);
        std::string error;
        if (!InspectMarkup(source, modelNames, error)) diagnostics.push_back(error + ": " + logical);
    }
    if (next_ == std::numeric_limits<Rml::FileHandle>::max()) return fail();
    auto handle = ++next_; files_.emplace(handle, std::move(file)); return handle;
}
void Files::Close(Rml::FileHandle f) { files_.erase(f); }
std::size_t Files::Read(void* out, std::size_t size, Rml::FileHandle f) { auto it = files_.find(f); if (it == files_.end()) return 0; auto& s = it->second->stream; s.read(static_cast<char*>(out), static_cast<std::streamsize>(size)); return static_cast<std::size_t>(s.gcount()); }
bool Files::Seek(Rml::FileHandle f, long offset, int origin) { auto it = files_.find(f); if (it == files_.end()) return false; auto& s = it->second->stream; s.clear(); if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) return false; s.seekg(offset, origin == SEEK_SET ? std::ios::beg : origin == SEEK_CUR ? std::ios::cur : std::ios::end); return !!s; }
std::size_t Files::Tell(Rml::FileHandle f) { auto it = files_.find(f); if (it == files_.end()) return 0; auto pos = it->second->stream.tellg(); return pos < 0 ? it->second->length : static_cast<std::size_t>(pos); }
std::size_t Files::Length(Rml::FileHandle f) { auto it = files_.find(f); return it == files_.end() ? 0 : it->second->length; }
void System::JoinPath(Rml::String& out, const Rml::String& source, const Rml::String& path) { out = JoinAsset(source, path); }
bool System::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    auto& logger = log::Channel("UI");
    if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) logger.error("{}", message);
    else if (type == Rml::Log::LT_WARNING) logger.warn("{}", message);
    else logger.debug("{}", message);
    if (collecting && (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING)) diagnostics.push_back(message);
    return true;
}
void System::SetClipboardText(const Rml::String& text) { std::string e; if (!host->WriteClipboard(text, e)) log::Channel("UI").warn("UI clipboard: {}", e); }
void System::GetClipboardText(Rml::String& text) { std::string e; if (!host->ReadClipboard(text, e)) { text.clear(); log::Channel("UI").warn("UI clipboard: {}", e); } }
void System::ActivateKeyboard(Rml::Vector2f p, float height) { snapshot.caret = {p.x, p.y, 1, height}; snapshot.active = true; host->SetTextInput(snapshot); }
void System::DeactivateKeyboard() { snapshot.active = false; host->SetTextInput(snapshot); }
void System::OnActivate(Rml::TextInputContext* value) { if (text != value) { Cancel(); activeElement.reset(); } text = value; snapshot.active = true; }
void System::OnDeactivate(Rml::TextInputContext* value) { if (text == value) { Cancel(); text = nullptr; activeElement.reset(); DeactivateKeyboard(); } }
void System::OnDestroy(Rml::TextInputContext* value) {
    // This notification comes from the input context's destructor. Its owning
    // widget may already be in destruction: never call back through the context.
    if (text == value) { text = nullptr; activeElement.reset(); composing = false; compositionStart = compositionEnd = 0; host->CancelComposition(); DeactivateKeyboard(); }
}
void System::Changed() { if (auto control = dynamic_cast<Rml::ElementFormControl*>(activeElement.get())) { Rml::Dictionary parameters; parameters["value"] = Rml::Variant(control->GetValue()); control->DispatchEvent("change", parameters); } }
void System::Cancel() { if (text && composing) { text->SetText("", compositionStart, compositionEnd); text->SetCompositionRange(0, 0); text->SetCursorPosition(compositionStart); Changed(); } composing = false; compositionStart = compositionEnd = 0; host->CancelComposition(); }
void System::Publish(Rml::Context* context) {
    snapshot.active = text != nullptr;
    if (text) {
        int a = 0, b = 0; text->GetSelectionRange(a, b); snapshot.selectionStart = static_cast<std::uint32_t>(std::max(0, std::min(a, b))); snapshot.selectionLength = static_cast<std::uint32_t>(std::abs(b - a)); snapshot.cursor = static_cast<std::uint32_t>(std::max(0, b));
        if (auto control = dynamic_cast<Rml::ElementFormControl*>(context->GetFocusElement())) { snapshot.surroundingText = control->GetValue(); activeElement = control->GetObserverPtr(); } else { snapshot.surroundingText.clear(); activeElement.reset(); }
    } else { activeElement.reset(); snapshot.surroundingText.clear(); snapshot.selectionStart = snapshot.selectionLength = snapshot.cursor = 0; }
    host->SetTextInput(snapshot);
}
void System::Compose(const InputUiComposition& e) {
    if (!text) return;
    const auto scalarCount = utf::Utf8ScalarCount(e.utf8);
    if (!scalarCount) return;
    if (e.phase == InputUiCompositionPhase::Cancel) { Cancel(); return; }
    int a = 0, b = 0; text->GetSelectionRange(a, b);
    const auto count = *scalarCount;
    if (count > static_cast<std::size_t>(std::numeric_limits<int>::max()) || e.cursor > count || e.selectionStart > count || e.selectionLength > count - e.selectionStart) return;
    for (auto& clause : e.clauses) if (clause.start > count || clause.length > count - clause.start) return;
    if (!composing) {
        compositionStart = std::min(a, b); compositionEnd = std::max(a, b);
        if (e.replacementStart || e.replacementLength) {
            auto start = static_cast<std::int64_t>(b) + e.replacementStart;
            const auto available = utf::Utf8ScalarCount(snapshot.surroundingText);
            const auto end = start + e.replacementLength;
            if (!available || start < 0 || end > INT_MAX || static_cast<std::uint64_t>(end) > *available) return;
            compositionStart = static_cast<int>(start); compositionEnd = compositionStart + static_cast<int>(e.replacementLength);
        }
        composing = true;
    }
    if (compositionStart < 0 || compositionEnd < compositionStart || count > static_cast<std::size_t>(INT_MAX - compositionStart)) return;
    if (e.phase == InputUiCompositionPhase::Commit) {
        // Match the 6.3 native backend: first replace the preedit, then commit its
        // updated range. This also handles a result with no preceding preedit.
        text->SetText(e.utf8, compositionStart, compositionEnd);
        compositionEnd = compositionStart + static_cast<int>(count);
        text->SetCompositionRange(compositionStart, compositionEnd); text->CommitComposition(e.utf8);
        text->SetCursorPosition(compositionEnd);
        text->SetCompositionRange(0, 0); composing = false; Changed(); return;
    }
    if (e.phase == InputUiCompositionPhase::Begin) return;
    text->SetText(e.utf8, compositionStart, compositionEnd); compositionEnd = compositionStart + static_cast<int>(count);
    text->SetCompositionRange(compositionStart, compositionEnd);
    text->SetSelectionRange(compositionStart + static_cast<int>(e.selectionStart), compositionStart + static_cast<int>(e.selectionStart + e.selectionLength));
    if (!e.selectionLength) text->SetCursorPosition(compositionStart + static_cast<int>(e.cursor));
    Changed();
}
namespace {
class NullRenderer final : public Rml::RenderInterface {
    std::uintptr_t next_ = 0;
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return ++next_; }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        // Read PNG dimensions without introducing a platform/GPU dependency.
        std::filesystem::path path; std::string name; if (!ResolveAsset(source, path, name)) return 0;
        std::ifstream file(path, std::ios::binary); unsigned char header[24]{}; if (!file.read(reinterpret_cast<char*>(header), 24)) return 0;
        static const unsigned char png[] = {137,80,78,71,13,10,26,10}; if (std::memcmp(header, png, 8)) return 0;
        auto read = [&](int i) { return (unsigned(header[i]) << 24) | (unsigned(header[i+1]) << 16) | (unsigned(header[i+2]) << 8) | header[i+3]; };
        auto w = read(16), h = read(20); if (!w || !h || w > INT_MAX || h > INT_MAX) return 0; dimensions = {int(w), int(h)}; return ++next_;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return ++next_; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};
}
std::unique_ptr<Rml::RenderInterface> CreateNullRenderer() { return std::make_unique<NullRenderer>(); }
}
