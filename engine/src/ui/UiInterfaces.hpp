#pragma once
#include <RmlUi/Core.h>
#include "UiElementObserver.hpp"
#include <RmlUi/Core/TextInputContext.h>
#include <RmlUi/Core/TextInputHandler.h>
#include <engine/ui/UiHost.hpp>
#include <engine/input/InputUiEvent.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace engine::ui_detail {
class NullHost final : public UiHost {
    std::string clipboard_;
public:
    bool ReadClipboard(std::string& text, std::string&) override { text = clipboard_; return true; }
    bool WriteClipboard(std::string_view text, std::string&) override { clipboard_ = text; return true; }
    void SetCursor(std::string_view) override {}
    void SetTextInput(const UiTextInputState&) override {}
    void CancelComposition() override {}
    void SetCapture(const UiCaptureState&) override {}
};
// Shared by file IO, the system URL joiner and renderer resource lookup. Only rooted,
// canonically contained paths are accepted; catalog entries are copied by the caller.
bool ResolveAsset(std::string_view url, std::filesystem::path& physical, std::string& logical);
std::string JoinAsset(std::string_view source, std::string_view relative);
bool InspectMarkup(std::string_view source, std::set<std::string>& models, std::string& error);
class Files final : public Rml::FileInterface {
    struct File { std::ifstream stream; std::size_t length = 0; };
    std::map<Rml::FileHandle, std::unique_ptr<File>> files_;
    Rml::FileHandle next_ = 0;
public:
    bool collecting = false;
    std::vector<std::string> diagnostics;
    std::set<std::string> modelNames;
    Rml::FileHandle Open(const Rml::String&) override;
    void Close(Rml::FileHandle) override;
    std::size_t Read(void*, std::size_t, Rml::FileHandle) override;
    bool Seek(Rml::FileHandle, long, int) override;
    std::size_t Tell(Rml::FileHandle) override;
    std::size_t Length(Rml::FileHandle) override;
};
class System final : public Rml::SystemInterface, public Rml::TextInputHandler {
public:
    explicit System(std::shared_ptr<UiHost> host) : host(std::move(host)) {}
    std::shared_ptr<UiHost> host;
    double time = 0;
    bool collecting = false;
    std::vector<std::string> diagnostics;
    Rml::TextInputContext* text = nullptr; // borrowed only between OnActivate/OnDestroy
    ElementObserver activeElement;
    UiTextInputState snapshot;
    int compositionStart = 0, compositionEnd = 0;
    bool composing = false;
    double GetElapsedTime() override { return time; }
    void JoinPath(Rml::String&, const Rml::String&, const Rml::String&) override;
    bool LogMessage(Rml::Log::Type, const Rml::String&) override;
    void SetMouseCursor(const Rml::String& name) override { host->SetCursor(name); }
    void SetClipboardText(const Rml::String&) override;
    void GetClipboardText(Rml::String&) override;
    void ActivateKeyboard(Rml::Vector2f, float) override;
    void DeactivateKeyboard() override;
    void OnActivate(Rml::TextInputContext*) override;
    void OnDeactivate(Rml::TextInputContext*) override;
    void OnDestroy(Rml::TextInputContext*) override;
    void Cancel();
    void Changed();
    void Publish(Rml::Context*);
    void Compose(const InputUiComposition&);
};
std::unique_ptr<Rml::RenderInterface> CreateNullRenderer();
}
