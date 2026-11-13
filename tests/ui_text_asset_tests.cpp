#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/engine.hpp>
#include <engine/input/InputSubsystem.hpp>
#include <engine/scripting/ScriptSubsystem.hpp>
#include <engine/ui/UiSubsystem.hpp>

#include <gtest/gtest.h>
#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#endif

namespace {
using namespace engine;

std::string Utf8(std::u8string_view text) {
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct TextAssetRoots {
    std::filesystem::path game = AssetPathResolver::GetGameRoot();
    ~TextAssetRoots() { AssetPathResolver::SetGameRoot(game); }
};

struct WorkingDirectory {
    std::filesystem::path previous = std::filesystem::current_path();
    explicit WorkingDirectory(const std::filesystem::path& path) { std::filesystem::current_path(path); }
    ~WorkingDirectory() { std::error_code ignored; std::filesystem::current_path(previous, ignored); }
};

struct TextHost final : UiHost {
    std::string clipboard;
    unsigned reads = 0, writes = 0, cancellations = 0;
    UiTextInputState text;
    UiCaptureState capture;
    bool ReadClipboard(std::string& value, std::string&) override { ++reads; value = clipboard; return true; }
    bool WriteClipboard(std::string_view value, std::string&) override { ++writes; clipboard = value; return true; }
    void SetCursor(std::string_view) override {}
    void SetTextInput(const UiTextInputState& value) override { text = value; }
    void CancelComposition() override { ++cancellations; }
    void SetCapture(const UiCaptureState& value) override { capture = value; }
};

// Match ui_tests.cpp's native session/owner lifetime; no Rml pointers or direct
// model-write/event-queue seams. Text travels through the real InputSubsystem.
class UiTextAssetTest : public testing::Test {
protected:
    TextAssetRoots roots;
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        FromUtf8("sri-ui-" + NewUuid() + Utf8(u8"-\u041f\u0440\u043e\u0435\u043a\u0442-\u65e5\u672c"));
    std::shared_ptr<TextHost> platform = std::make_shared<TextHost>();
    Engine host{EngineConfig{.taskThreads = 1}};
    InputSubsystem* input = host.AddSubsystem<InputSubsystem>();
    UiSubsystem* ui = host.AddSubsystem<UiSubsystem>(UiSubsystemConfig{
        .renderer = UiRendererBackend::Null, .host = platform, .nullSurface = {800, 600, 1.0f}});
    UiSession session;
    UiOwner owner;
    std::string error;

    void SetUp() override {
        std::filesystem::create_directories(root);
        AssetPathResolver::SetGameRoot(root);
        ASSERT_TRUE(host.Initialize());
        ASSERT_TRUE(ui->BeginNativeSession(session, error)) << error;
        ASSERT_TRUE(ui->CreateNativeOwner(session, "text/assets test", owner, error)) << error;
    }
    void TearDown() override {
        host.Shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    AssetReference Asset(std::string_view name, std::string_view source, CatalogAssetKind kind) {
        const auto file = root / FromUtf8(name);
        std::filesystem::create_directories(file.parent_path());
        EXPECT_TRUE(CreateNewFile(file, source, error)) << error;
        AssetMetadata metadata{NewUuid(), kind};
        EXPECT_TRUE(WriteAssetMetadata(file, metadata, error)) << error;
        return {metadata.id, "game/" + std::string(name)};
    }
    UiDocument Load(std::string_view path, UiDocumentDesc desc = {}) {
        EXPECT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
        UiDocument document;
        EXPECT_TRUE(ui->LoadDocument(owner, path, desc, document, error)) << error;
        return document;
    }
    UiElement Element(UiDocument document, std::string_view id) {
        UiElement element;
        EXPECT_TRUE(ui->FindElement(owner, document, id, element, error)) << error;
        return element;
    }
    void Tick() { host.Tick(); }
    template<class Payload> void Send(Payload payload, InputUiModifiers modifiers = {}) {
        InputUiEvent event;
        event.modifiers = modifiers;
        event.payload = std::move(payload);
        input->OnUiEvent(std::move(event));
    }
    void KeyStroke(Key key, InputUiModifiers modifiers = {}) {
        Send(InputUiKey{.logical = key, .down = true}, modifiers);
        Send(InputUiKey{.logical = key, .down = false}, modifiers);
    }
    void Click(float x, float y) {
        for (bool down : {true, false}) {
            InputUiEvent event;
            event.pointerX = x; event.pointerY = y; event.hasPointerPosition = true;
            event.payload = InputUiPointerButton{.button = MouseButton::Left, .down = down};
            input->OnUiEvent(std::move(event));
        }
        Tick();
    }
    void ExpectValue(UiElement element, std::string_view expected) {
        std::string value;
        ASSERT_TRUE(ui->GetFormValue(owner, element, value, error)) << error;
        EXPECT_EQ(value, expected);
    }
    UiElement OpenField() {
        Asset("ui/text.rml", R"(<rml><head><title>Text integration</title><style>
            body { font-family: DejaVu Sans; font-size: 18px; }
            input { position: absolute; left: 40dp; top: 30dp; width: 240dp; height: 32dp; }
            </style></head><body><input id="edit" type="text"/></body></rml>)", CatalogAssetKind::UiDocument);
        const auto document = Load("game/ui/text.rml");
        EXPECT_TRUE(ui->Show(owner, document, UiDocumentMode::Interactive, error)) << error;
        const auto field = Element(document, "edit");
        EXPECT_TRUE(ui->Focus(owner, field, error)) << error;
        Tick();
        return field;
    }
};

TEST_F(UiTextAssetTest, UnicodeInsertionNavigationAndDeletionUseScalarsIncludingSupplementaryCharacters) {
    const auto field = OpenField();
    // Four scalars, eight UTF-8 bytes, five UTF-16 units.
    const auto text = Utf8(u8"A\u0416\U0001f642Z");
    Send(InputUiText{text});
    Tick();
    ExpectValue(field, text);
    ASSERT_TRUE(platform->text.active);
    EXPECT_EQ(platform->text.surroundingText, text);
    EXPECT_EQ(platform->text.cursor, 4u);
    KeyStroke(Key::Left);
    KeyStroke(Key::Backspace);
    Tick();
    ExpectValue(field, Utf8(u8"A\u0416Z"));
    EXPECT_EQ(platform->text.cursor, 2u);
    Send(InputUiText{Utf8(u8"\U0001f680")});
    Tick();
    ExpectValue(field, Utf8(u8"A\u0416\U0001f680Z"));
    EXPECT_EQ(platform->text.cursor, 3u);
    KeyStroke(Key::A, {.control = true});
    Tick();
    EXPECT_EQ(platform->text.selectionStart, 0u);
    EXPECT_EQ(platform->text.selectionLength, 4u);
}

TEST_F(UiTextAssetTest, ImePreeditReplacementCommitAndCancelWriteThroughRealBoundForm) {
    UiModelDesc desc;
    desc.name = "entry";
    desc.schema.fields = {{"value", {UiValueKind::String, {}, {}}}};
    desc.initialValues = StateValue::Object{{"value", "seed"}};
    UiModel model;
    ASSERT_TRUE(ui->CreateModel(owner, desc, model, error)) << error;
    Asset("ui/bound.rml", R"(<rml><head><title>IME binding</title>
        <style>body { font-family: DejaVu Sans; } input { width: 300px; height: 30px; }</style>
        </head><body data-model="entry"><input id="edit" type="text" data-value="value"/>
        <input id="mirror" type="text" data-value="value"/></body></rml>)", CatalogAssetKind::UiDocument);
    const auto document = Load("game/ui/bound.rml", {{model}});
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Interactive, error)) << error;
    const auto field = Element(document, "edit"), mirror = Element(document, "mirror");
    Tick();
    ExpectValue(field, "seed");
    const auto prefix = Utf8(u8"\u0416\U0001f642");
    ASSERT_TRUE(ui->SetModelValue(owner, model, "value", prefix, error)) << error;
    Tick();
    ExpectValue(field, prefix);
    ASSERT_TRUE(ui->Focus(owner, field, error)) << error;
    // Ctrl+End targets the buffer end. Plain End uses Rml's cached visual line,
    // which the null renderer need not have reformatted after a bound value change.
    Send(InputUiKey{.navigation = InputUiNavigationKey::End, .down = true}, {.control = true});
    Send(InputUiKey{.navigation = InputUiNavigationKey::End, .down = false}, {.control = true});
    Tick();
    ASSERT_EQ(platform->text.cursor, 2u);
    ASSERT_EQ(platform->text.selectionLength, 0u);
    Send(InputUiComposition{.phase = InputUiCompositionPhase::Begin});
    Tick();
    ExpectValue(field, prefix);
    Send(InputUiComposition{.phase = InputUiCompositionPhase::Update,
        .utf8 = Utf8(u8"\u304b\U0001f680"), .cursor = 2});
    Tick();
    ExpectValue(field, prefix + Utf8(u8"\u304b\U0001f680"));
    EXPECT_EQ(platform->text.cursor, 4u);
    // A second preedit replaces the first, rather than accumulating it.
    Send(InputUiComposition{.phase = InputUiCompositionPhase::Update,
        .utf8 = Utf8(u8"\u6f22"), .cursor = 1});
    Send(InputUiComposition{.phase = InputUiCompositionPhase::Commit,
        .utf8 = Utf8(u8"\u6f22\U0001f680"), .cursor = 2});
    Tick();
    const auto committed = prefix + Utf8(u8"\u6f22\U0001f680");
    ExpectValue(field, committed);
    ExpectValue(mirror, committed);
    StateValue value;
    ASSERT_TRUE(ui->GetModelValue(owner, model, "value", value, error)) << error;
    EXPECT_EQ(value.String(), committed);
    EXPECT_EQ(platform->text.cursor, 4u);
    const auto cancellations = platform->cancellations;
    Send(InputUiComposition{.phase = InputUiCompositionPhase::Begin});
    Send(InputUiComposition{.phase = InputUiCompositionPhase::Update, .utf8 = "discard", .cursor = 7});
    Send(InputUiComposition{.phase = InputUiCompositionPhase::Cancel});
    Tick();
    ExpectValue(field, committed);
    ExpectValue(mirror, committed);
    EXPECT_GT(platform->cancellations, cancellations);
    ASSERT_TRUE(ui->GetModelValue(owner, model, "value", value, error)) << error;
    EXPECT_EQ(value.String(), committed);
    Send(InputUiText{"!"});
    Tick();
    ExpectValue(mirror, committed + "!");
    // An IME result may replace surrounding text without an earlier preedit.
    // Offsets here cross a supplementary scalar and must not use byte/UTF-16 indices.
    Send(InputUiComposition{.phase = InputUiCompositionPhase::Commit,
        .utf8 = Utf8(u8"\u66ff"), .cursor = 1, .replacementStart = -3, .replacementLength = 2});
    Tick();
    ExpectValue(field, prefix + Utf8(u8"\u66ff!"));
    ExpectValue(mirror, prefix + Utf8(u8"\u66ff!"));
    EXPECT_EQ(platform->text.cursor, 3u);
}

TEST_F(UiTextAssetTest, ClipboardShortcutsUseHostAndPreserveUnicodeInBothDirections) {
    const auto field = OpenField();
    const auto original = Utf8(u8"\u041f\u0440\u0438\u0432\u0435\u0442 \U0001f642");
    Send(InputUiText{original});
    KeyStroke(Key::A, {.control = true});
    KeyStroke(Key::C, {.control = true});
    Tick();
    EXPECT_GT(platform->writes, 0u);
    EXPECT_EQ(platform->clipboard, original);
    KeyStroke(Key::X, {.control = true});
    Tick();
    ExpectValue(field, "");
    EXPECT_EQ(platform->clipboard, original);
    platform->clipboard = Utf8(u8"\u65e5\u672c\U0001f680");
    KeyStroke(Key::V, {.control = true});
    Tick();
    EXPECT_GT(platform->reads, 0u);
    ExpectValue(field, platform->clipboard);
    EXPECT_EQ(platform->text.cursor, 3u);
}

TEST_F(UiTextAssetTest, DpiOnlyChangesMoveHitTargetsAndCaretInPhysicalSurfacePixels) {
    const auto field = OpenField();
    Send(InputUiFocus{false});
    Send(InputUiFocus{true});
    Tick();
    Click(50, 40);
    Send(InputUiText{"a"});
    Tick();
    ExpectValue(field, "a");
    const auto caret = platform->text.caret;
    EXPECT_GE(caret.x, 40.0f);
    EXPECT_GE(caret.y, 30.0f);
    EXPECT_GT(caret.height, 0.0f);
    Send(InputUiFocus{false});
    Send(InputUiFocus{true});
    Tick();
    // Same surface pixel dimensions: dp-ratio alone must relayout.
    ui->SetSurfaceMetrics({800, 600, 2.0f});
    Tick();
    Click(50, 40); // outside the moved field (80,60)-(560,124)
    Send(InputUiText{"wrong"});
    Tick();
    ExpectValue(field, "a");
    Click(90, 75);
    KeyStroke(Key::A, {.control = true});
    Send(InputUiText{"b"});
    Tick();
    ExpectValue(field, "b");
    EXPECT_GE(platform->text.caret.x, 80.0f);
    EXPECT_GE(platform->text.caret.y, 60.0f);
    EXPECT_LT(platform->text.caret.x, 560.0f);
    EXPECT_GT(platform->text.caret.y, caret.y + 20.0f);
}

TEST_F(UiTextAssetTest, NestedTemplateStylesAndIntrinsicPngResolveWithinUnicodeRootFromUnrelatedCwd) {
    Asset("ui/styles/layout.rcss", R"(
        #frame { position: absolute; left: 200px; top: 100px; width: 60px; height: 30px; }
        #picture { position: absolute; left: 300px; top: 100px; }
        )", CatalogAssetKind::UiStylesheet);
    Asset("ui/templates/frame.rmlt", R"(<template name="frame" content="slot"><head>
        <link type="text/rcss" href="../styles/layout.rcss"/></head><body>
        <button id="frame">Template</button><div id="slot"/></body></template>)", CatalogAssetKind::UiTemplate);
    Asset("ui/screens/main.rml", R"(<rml><head><title>Nested resources</title>
        <link type="text/template" href="../templates/frame.rmlt"/></head>
        <body template="frame"><img id="picture" src="../images/badge.png"/></body></rml>)", CatalogAssetKind::UiDocument);
    std::filesystem::create_directories(root / "ui/images");
    std::filesystem::copy_file(std::filesystem::path(SRI_SOURCE_ROOT) / "example_game/ui/badge.png",
                               root / "ui/images/badge.png");
    ASSERT_TRUE(WriteAssetMetadata(root / "ui/images/badge.png",
        AssetMetadata{NewUuid(), CatalogAssetKind::Texture}, error)) << error;
    std::filesystem::create_directories(root / "unrelated");
    WorkingDirectory cwd(root / "unrelated");
    const auto document = Load("game/ui/screens/main.rml");
    ASSERT_TRUE(ui->IsValid(document));
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Interactive, error)) << error;
    unsigned frames = 0, pictures = 0;
    UiSubscription frame, picture;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "frame", "click", [&](const UiEvent&) { ++frames; }, frame, error)) << error;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "picture", "click", [&](const UiEvent&) { ++pictures; }, picture, error)) << error;
    Tick();
    Click(220, 115); // Template's stylesheet, resolved relative to the template.
    EXPECT_EQ(frames, 1u);
    Click(330, 130); // PNG's intrinsic 32x32 bounds, no authored width/height.
    EXPECT_EQ(pictures, 1u);
    Click(335, 135);
    EXPECT_EQ(pictures, 1u);
}

TEST_F(UiTextAssetTest, ExplicitCatalogPathLoadsButMissingUuidNeverFallsBackToThatPath) {
    const auto asset = Asset("ui/identity.rml", R"(<rml><head><title>Identity</title></head>
        <body><input id="identity" type="text" value="right document"/></body></rml>)", CatalogAssetKind::UiDocument);
    const auto document = Load(asset.path);
    ASSERT_TRUE(ui->IsValid(document));
    UiDocument output = document;
    EXPECT_FALSE(ui->LoadDocument(owner, AssetReference{NewUuid(), asset.path}, {}, output, error));
    EXPECT_EQ(output, document);
    EXPECT_FALSE(error.empty());
    ASSERT_TRUE(ui->LoadDocument(owner, AssetReference{asset.id, "game/stale-name.rml"}, {}, output, error)) << error;
    ExpectValue(Element(output, "identity"), "right document");
    EXPECT_TRUE(ui->IsValid(document));
}

TEST_F(UiTextAssetTest, MalformedReloadRetainsLiveDomFormAndRealEventSubscription) {
    Asset("ui/reload.rml", R"(<rml><head><title>Good</title><style>
        input { position: absolute; left: 40px; top: 40px; width: 200px; height: 30px; }
        </style></head><body><input id="edit" type="text" value="retained"/></body></rml>)", CatalogAssetKind::UiDocument);
    const auto document = Load("game/ui/reload.rml");
    ASSERT_TRUE(ui->Show(owner, document, UiDocumentMode::Modal, error)) << error;
    const auto field = Element(document, "edit");
    ASSERT_TRUE(ui->Focus(owner, field, error)) << error;
    unsigned changes = 0;
    UiSubscription subscription;
    ASSERT_TRUE(ui->SubscribeNative(owner, document, "edit", "change", [&](const UiEvent&) { ++changes; }, subscription, error)) << error;
    Tick();
    std::vector<std::string> diagnostics;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg& message) {
        if (message.level >= spdlog::level::warn) diagnostics.emplace_back(message.payload.data(), message.payload.size());
    });
    struct SinkScope {
        std::shared_ptr<spdlog::sinks::callback_sink_mt> sink;
        ~SinkScope() { log::Sinks()->remove_sink(sink); }
    } scope{sink};
    log::Sinks()->add_sink(sink);
    ASSERT_TRUE(WriteFileAtomic(root / "ui/reload.rml", R"(<rml><head><title>Malformed</title></head>
        <body><div><input id="edit" value="must not replace"/></span></body></rml>)", error)) << error;
    ASSERT_TRUE(ui->RequestReload(owner, document, error)) << error;
    Tick();
    EXPECT_FALSE(diagnostics.empty());
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const std::string& message) {
        return message.find("game/ui/reload.rml") != std::string::npos;
    }));
    EXPECT_TRUE(ui->IsValid(document));
    EXPECT_TRUE(ui->IsVisible(document));
    ASSERT_TRUE(ui->IsValid(field));
    EXPECT_TRUE(ui->IsValid(subscription));
    EXPECT_TRUE(ui->GetCaptureState().keyboard);
    ExpectValue(field, "retained");
    ASSERT_TRUE(ui->Focus(owner, field, error)) << error;
    KeyStroke(Key::A, {.control = true});
    Send(InputUiText{"still live"});
    Tick();
    ExpectValue(field, "still live");
    EXPECT_GT(changes, 0u);
}

// Use a junction on Windows so canonical containment is exercised without the
// developer-mode/symlink privilege requirement.
std::error_code DirectoryAlias(const std::filesystem::path& link, const std::filesystem::path& target) {
#ifdef _WIN32
    std::filesystem::create_directory(link);
    const HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return {static_cast<int>(GetLastError()), std::system_category()};
    const std::wstring print = std::filesystem::canonical(target).native(), substitute = L"\\??\\" + print;
    struct Header { DWORD tag; WORD length, reserved, substituteOffset, substituteLength, printOffset, printLength; };
    static_assert(sizeof(Header) == 16);
    const auto a = substitute.size() * sizeof(wchar_t), b = print.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(Header) + a + b + 2 * sizeof(wchar_t));
    const Header header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(buffer.size() - 8), 0,
        0, static_cast<WORD>(a), static_cast<WORD>(a + sizeof(wchar_t)), static_cast<WORD>(b)};
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.c_str(), a + sizeof(wchar_t));
    std::memcpy(buffer.data() + sizeof(header) + header.printOffset, print.c_str(), b + sizeof(wchar_t));
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
        nullptr, 0, &returned, nullptr) != 0;
    const DWORD failure = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    return {static_cast<int>(failure), std::system_category()};
#else
    std::error_code error;
    std::filesystem::create_directory_symlink(target, link, error);
    return error;
#endif
}

TEST_F(UiTextAssetTest, CanonicalEscapeThroughExistingDirectoryAliasRejectsResourceLoad) {
    // A sibling under the same temporary parent catches string-prefix containment.
    const auto outside = std::filesystem::path(root.native() + std::filesystem::path("-outside").native());
    std::filesystem::create_directories(outside);
    struct Cleanup {
        std::filesystem::path link, outside;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(link, ignored); std::filesystem::remove_all(outside, ignored); }
    } cleanup{root / "escaped", outside};
    ASSERT_TRUE(CreateNewFile(outside / "external.rcss", "body { width: 17px; }", error)) << error;
    const auto aliasError = DirectoryAlias(cleanup.link, outside);
    ASSERT_FALSE(aliasError) << "Cannot create containment test alias: " << aliasError.message();
    Asset("ui/escape.rml", R"(<rml><head><title>Canonical escape</title>
        <link type="text/rcss" href="../escaped/external.rcss"/></head><body/></rml>)", CatalogAssetKind::UiDocument);
    ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
    UiDocument document;
    EXPECT_FALSE(ui->LoadDocument(owner, "game/ui/escape.rml", {}, document, error));
    EXPECT_TRUE(document.IsEmpty());
    EXPECT_FALSE(error.empty());
}

TEST(UiWithoutSubsystemTest, LuaCreationReturnsUsefulUnavailableErrorInsideStartedOwner) {
    TextAssetRoots roots;
    const auto root = std::filesystem::temp_directory_path() / ("sri-ui-absent-" + NewUuid());
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } cleanup{root};
    AssetPathResolver::SetGameRoot(root);
    Engine host{EngineConfig{.taskThreads = 1}};
    auto* scripts = host.AddSubsystem<ScriptSubsystem>();
    ASSERT_TRUE(host.Initialize());
    std::string error;
    ASSERT_TRUE(scripts->BeginPlay(root, error)) << error;
    ScriptInstanceDesc desc;
    desc.source = root / "NoUi.lua";
    desc.assetId = NewUuid();
    desc.autoloadName = "NoUi";
    ASSERT_TRUE(CreateNewFile(desc.source, R"(
        function OnStart()
            local doc, documentError = UI.LoadDocument('game/ui/absent.rml')
            assert(doc == nil and type(documentError) == 'string')
            assert(documentError:find('unavailable') and documentError:find('UiSubsystem'), documentError)
            local model, modelError = UI.CreateModel('absent', {value='test'})
            assert(model == nil and type(modelError) == 'string')
            assert(modelError:find('unavailable') and modelError:find('UiSubsystem'), modelError)
        end
    )", error)) << error;
    InstanceId instance;
    ASSERT_TRUE(scripts->GetRuntime().CreateInstance(desc, instance, error)) << error;
    ASSERT_TRUE(scripts->GetRuntime().StartInstance(instance, error)) << error;
    EXPECT_TRUE(scripts->GetRuntime().DestroyInstance(instance, error)) << error;
    host.Shutdown();
}
} // namespace
