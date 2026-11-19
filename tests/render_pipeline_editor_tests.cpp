#include "asset_dialogs.hpp"
#include "asset_filter_model.hpp"
#include "asset_operations.hpp"
#include "content_browser.hpp"

#include <editor/asset_picker.hpp>
#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/viewport_tool_bar.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/PostProcessDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <DockManager.h>
#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <gtest/gtest.h>

namespace {
std::string encoded(const engine::RenderPipelineDefinition& value) {
    std::string text, error;
    EXPECT_TRUE(engine::SerializeRenderPipelineDefinition(value, text, error)) << error;
    return text;
}

std::string encoded(const engine::PostProcessDefinition& value) {
    std::string text, error;
    EXPECT_TRUE(engine::SerializePostProcessDefinition(value, text, error)) << error;
    return text;
}

class RenderPipelineEditor : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        previousRoot = engine::AssetPathResolver::GetGameRoot();
        project.file = editor::toPath(directory.filePath("pipeline.sriproject"));
        ASSERT_TRUE(QDir().mkpath(editor::toQString(project.ContentDir())));
        engine::AssetPathResolver::SetGameRoot(project.ContentDir());
        std::string error;
        const auto shader = project.ContentDir() / "billboard.hlsl";
        ASSERT_TRUE(engine::CreateNewFile(shader, "// editor-only fixture", error)) << error;
        ASSERT_TRUE(engine::WriteAssetMetadata(shader, {shaderId, engine::CatalogAssetKind::Shader}, error)) << error;
        ASSERT_TRUE(host.RescanAssetCatalogs(error)) << error;
    }
    void TearDown() override { engine::AssetPathResolver::SetGameRoot(previousRoot); }

    engine::RenderPipelineDefinition definition() const {
        engine::RenderPipelineDefinition value;
        value.shader = {shaderId, "game/billboard.hlsl"};
        value.vertexEntryPoint = "BillboardVS";
        value.pixelEntryPoint = "CutoutPS";
        value.defines = {{"INSTANCE_COUNT", "3"}, {"EXPRESSION", " (1 + 2) "}, {"FLAG", ""}};
        value.blend = engine::ProceduralBlend::Alpha;
        value.depth = engine::ProceduralDepth::ReadOnly;
        return value;
    }

    QListView* items(content::ContentBrowser& browser) {
        for (auto* view : browser.findChildren<QListView*>())
            if (qobject_cast<content::AssetFilterModel*>(view->model())) return view;
        return nullptr;
    }
    QModelIndex index(QListView& view, const QString& name) {
        for (int row = 0; row < view.model()->rowCount(view.rootIndex()); ++row) {
            const auto candidate = view.model()->index(row, 0, view.rootIndex());
            if (candidate.data().toString() == name) return candidate;
        }
        return {};
    }

    QTemporaryDir directory;
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    engine::Project project;
    QMainWindow window;
    ads::CDockManager docks{&window};
    QWidget viewport;
    editor::ViewportToolBar toolbar;
    editor::EditorContext context{host, project, window, docks, viewport, toolbar};
    engine::AssetUuid shaderId{engine::NewUuid()};
    std::filesystem::path previousRoot;
};

TEST_F(RenderPipelineEditor, BrowserEditsPipelineNativelyAndCancelPreservesTheFile) {
    content::AssetOperations operations(context, &window);
    const auto file = project.ContentDir() / "Billboards.pipeline.json";
    auto expected = definition();
    ASSERT_TRUE(operations.createRenderPipeline(editor::toQString(file), expected));
    std::string error, metaBefore;
    ASSERT_TRUE(engine::ReadTextFile(engine::AssetMetadataPath(file), metaBefore, error)) << error;
    int fallbackOpens = 0;
    context.setAssetEditor(engine::CatalogAssetKind::RenderPipeline, [&](const auto&) { ++fallbackOpens; });
    content::ContentBrowser browser(context);
    browser.resize(900, 600);
    browser.show();
    auto* view = items(browser);
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return index(*view, "Billboards.pipeline.json").isValid(); }));

    bool opened = false;
    QTimer::singleShot(0, &browser, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto close = qScopeGuard([&] { if (modal) modal->reject(); });
        auto* dialog = dynamic_cast<content::RenderPipelineDialog*>(modal);
        ASSERT_NE(dialog, nullptr);
        opened = true;
        EXPECT_EQ(dialog->name(), "Billboards");
        EXPECT_EQ(encoded(dialog->definition()), encoded(expected));
        dialog->findChild<QComboBox*>("pipelineBlend")->setCurrentText("Additive");
        dialog->findChild<QComboBox*>("pipelineDepth")->setCurrentText("Disabled");
        dialog->findChild<QLineEdit*>("pipelineVertexEntry")->setText("InstancedVS");
        auto* ok = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        ASSERT_TRUE(ok->isEnabled());
        close.dismiss();
        ok->click();
    });
    view->activated(index(*view, "Billboards.pipeline.json"));
    ASSERT_TRUE(opened);
    EXPECT_EQ(fallbackOpens, 0);
    expected.blend = engine::ProceduralBlend::Additive;
    expected.depth = engine::ProceduralDepth::Disabled;
    expected.vertexEntryPoint = "InstancedVS";
    engine::RenderPipelineDefinition saved;
    ASSERT_TRUE(engine::LoadRenderPipelineDefinition(file, saved, error)) << error;
    EXPECT_EQ(encoded(saved), encoded(expected));
    std::string metaAfter, textBefore, textAfter;
    ASSERT_TRUE(engine::ReadTextFile(engine::AssetMetadataPath(file), metaAfter, error)) << error;
    EXPECT_EQ(metaAfter, metaBefore);
    ASSERT_TRUE(engine::ReadTextFile(file, textBefore, error)) << error;

    opened = false;
    QTimer::singleShot(0, &browser, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto close = qScopeGuard([&] { if (modal) modal->reject(); });
        auto* dialog = dynamic_cast<content::RenderPipelineDialog*>(modal);
        ASSERT_NE(dialog, nullptr);
        opened = true;
        dialog->findChild<QLineEdit*>("pipelinePixelEntry")->setText("UnsavedPS");
    });
    view->activated(index(*view, "Billboards.pipeline.json"));
    ASSERT_TRUE(opened);
    ASSERT_TRUE(engine::ReadTextFile(file, textAfter, error)) << error;
    EXPECT_EQ(textAfter, textBefore);
}

TEST_F(RenderPipelineEditor, MissingShaderSurvivesEditsAndInvalidDefinesCannotBeSaved) {
    content::RenderPipelineDialog dialog(host.GetAssets().GetCatalog(),
        editor::toQString(project.ContentDir()), "Missing", nullptr, true);
    auto expected = definition();
    expected.shader = {engine::NewUuid(), "game/removed.hlsl"};
    dialog.load(expected);
    auto* picker = dialog.findChild<editor::AssetPicker*>("pipelineShader");
    auto* missing = dialog.findChild<QLabel*>("pipelineMissingShader");
    auto* table = dialog.findChild<QTableWidget*>("pipelineDefines");
    auto* ok = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    ASSERT_NE(picker, nullptr);
    ASSERT_NE(missing, nullptr);
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(picker->currentIndex(), -1);
    EXPECT_FALSE(missing->isHidden());
    picker->reload();
    dialog.findChild<QComboBox*>("pipelineBlend")->setCurrentText("Opaque");
    expected.blend = engine::ProceduralBlend::Opaque;
    EXPECT_EQ(encoded(dialog.definition()), encoded(expected));
    EXPECT_TRUE(ok->isEnabled());
    table->item(1, 0)->setText("INSTANCE_COUNT");
    EXPECT_FALSE(ok->isEnabled()) << "duplicate macro names must not be saved";
    table->item(1, 0)->setText("EXPRESSION");
    EXPECT_TRUE(ok->isEnabled());
    auto* vertex = dialog.findChild<QLineEdit*>("pipelineVertexEntry");
    vertex->setText("invalid entry");
    EXPECT_FALSE(ok->isEnabled());
    vertex->setText("BillboardVS");
    EXPECT_TRUE(ok->isEnabled());
    dialog.findChild<QPushButton*>("pipelineAddDefine")->click();
    EXPECT_FALSE(ok->isEnabled()) << "a new row requires a macro name";
    const int last = table->rowCount() - 1;
    table->setItem(last, 0, new QTableWidgetItem("EXTRA"));
    EXPECT_TRUE(ok->isEnabled()) << "an empty macro value is valid";
    dialog.findChild<QPushButton*>("pipelineRemoveDefine")->click();
    EXPECT_EQ(encoded(dialog.definition()), encoded(expected));
    ASSERT_TRUE(picker->selectId(shaderId));
    EXPECT_TRUE(missing->isHidden());
    EXPECT_EQ(dialog.definition().shader.id, shaderId);
}

TEST_F(RenderPipelineEditor, NewMenuCreatesPipelineMetadataAndDuplicatesKeepTheCompoundSuffix) {
    content::ContentBrowser browser(context);
    QAction* create = nullptr;
    for (auto* button : browser.findChildren<QToolButton*>()) {
        if (!button->menu()) continue;
        for (auto* action : button->menu()->actions())
            if (action->text() == "Render Pipeline...") create = action;
    }
    ASSERT_NE(create, nullptr);
    ASSERT_TRUE(create->isEnabled());
    bool opened = false;
    QTimer::singleShot(0, &browser, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto close = qScopeGuard([&] { if (modal) modal->reject(); });
        auto* dialog = dynamic_cast<content::RenderPipelineDialog*>(modal);
        ASSERT_NE(dialog, nullptr);
        opened = true;
        auto* ok = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        EXPECT_FALSE(ok->isEnabled()) << "a new pipeline requires a shader";
        ASSERT_TRUE(dialog->findChild<editor::AssetPicker*>("pipelineShader")->selectId(shaderId));
        ASSERT_TRUE(ok->isEnabled());
        close.dismiss();
        ok->click();
    });
    create->trigger();
    ASSERT_TRUE(opened);
    const auto file = project.ContentDir() / "New Render Pipeline.pipeline.json";
    engine::AssetMetadata metadata;
    engine::RenderPipelineDefinition saved;
    std::string error;
    ASSERT_TRUE(engine::ReadAssetMetadata(file, metadata, error)) << error;
    EXPECT_EQ(metadata.kind, engine::CatalogAssetKind::RenderPipeline);
    ASSERT_TRUE(engine::LoadRenderPipelineDefinition(file, saved, error)) << error;
    EXPECT_EQ(saved.shader.id, shaderId);
    EXPECT_EQ(saved.vertexEntryPoint, "VSMain");
    EXPECT_EQ(saved.pixelEntryPoint, "PSMain");
    EXPECT_EQ(saved.blend, engine::ProceduralBlend::Opaque);
    EXPECT_EQ(saved.depth, engine::ProceduralDepth::ReadWrite);
    EXPECT_NE(host.GetAssets().GetCatalog().Find(metadata.id, engine::CatalogAssetKind::RenderPipeline), nullptr);
    content::AssetOperations operations(context, &window);
    const auto copies = operations.duplicate({editor::toQString(file)});
    ASSERT_EQ(copies.size(), 1);
    EXPECT_TRUE(copies.front().endsWith("New Render Pipeline (1).pipeline.json"));
    engine::AssetMetadata copyMetadata;
    ASSERT_TRUE(engine::ReadAssetMetadata(editor::toPath(copies.front()), copyMetadata, error)) << error;
    EXPECT_NE(copyMetadata.id, metadata.id);
    EXPECT_EQ(copyMetadata.kind, metadata.kind);
}

TEST_F(RenderPipelineEditor, BrowserEditsPostProcessNativelyAndCancelPreservesTheFile) {
    content::AssetOperations operations(context, &window);
    const auto file = project.ContentDir() / "Effects.postprocess.json";
    engine::PostProcessDefinition expected;
    expected.shader = {engine::NewUuid(), "game/removed.hlsl"};
    expected.options.entryPoint = "EffectPS";
    expected.options.defines = {{"FIRST", "1"}, {"SECOND", ""}};
    ASSERT_TRUE(operations.createPostProcess(editor::toQString(file), expected));
    std::string error, metaBefore;
    ASSERT_TRUE(engine::ReadTextFile(engine::AssetMetadataPath(file), metaBefore, error)) << error;
    engine::AssetMetadata metadataBefore;
    ASSERT_TRUE(engine::ReadAssetMetadata(file, metadataBefore, error)) << error;
    int fallbackOpens = 0;
    context.setAssetEditor(engine::CatalogAssetKind::PostProcess, [&](const auto&) { ++fallbackOpens; });
    content::ContentBrowser browser(context);
    browser.resize(900, 600);
    browser.show();
    auto* view = items(browser);
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return index(*view, "Effects.postprocess.json").isValid(); }));

    bool opened = false;
    QTimer::singleShot(0, &browser, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto close = qScopeGuard([&] { if (modal) modal->reject(); });
        auto* dialog = dynamic_cast<content::PostProcessDialog*>(modal);
        ASSERT_NE(dialog, nullptr);
        opened = true;
        EXPECT_EQ(dialog->windowTitle(), "Edit Postprocess");
        EXPECT_EQ(dialog->name(), "Effects");
        EXPECT_EQ(encoded(dialog->definition()), encoded(expected));
        auto* picker = dialog->findChild<editor::AssetPicker*>("postprocessShader");
        auto* missing = dialog->findChild<QLabel*>("postprocessMissingShader");
        auto* table = dialog->findChild<QTableWidget*>("postprocessDefines");
        auto* entry = dialog->findChild<QLineEdit*>("postprocessEntry");
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        ASSERT_NE(picker, nullptr);
        ASSERT_NE(missing, nullptr);
        ASSERT_NE(table, nullptr);
        ASSERT_NE(entry, nullptr);
        ASSERT_NE(buttons, nullptr);
        auto* ok = buttons->button(QDialogButtonBox::Ok);
        ASSERT_NE(ok, nullptr);
        EXPECT_EQ(picker->currentIndex(), -1);
        EXPECT_FALSE(missing->isHidden());
        picker->reload();
        ASSERT_EQ(table->rowCount(), 2);
        ASSERT_NE(table->item(1, 0), nullptr);
        table->item(1, 0)->setText("FIRST");
        EXPECT_FALSE(ok->isEnabled());
        table->item(1, 0)->setText("SECOND");
        entry->setText("invalid entry");
        EXPECT_FALSE(ok->isEnabled());
        entry->setText("UpdatedPS");
        ASSERT_TRUE(ok->isEnabled());
        close.dismiss();
        ok->click();
    });
    view->activated(index(*view, "Effects.postprocess.json"));
    ASSERT_TRUE(opened);
    EXPECT_EQ(fallbackOpens, 0);
    expected.options.entryPoint = "UpdatedPS";
    engine::PostProcessDefinition saved;
    ASSERT_TRUE(engine::LoadPostProcessDefinition(file, saved, error)) << error;
    EXPECT_EQ(encoded(saved), encoded(expected));
    std::string metaAfter, textBefore, textAfter;
    ASSERT_TRUE(engine::ReadTextFile(engine::AssetMetadataPath(file), metaAfter, error)) << error;
    EXPECT_EQ(metaAfter, metaBefore);
    engine::AssetMetadata metadataAfter;
    ASSERT_TRUE(engine::ReadAssetMetadata(file, metadataAfter, error)) << error;
    EXPECT_EQ(metadataAfter.id, metadataBefore.id);
    ASSERT_TRUE(engine::ReadTextFile(file, textBefore, error)) << error;

    opened = false;
    QTimer::singleShot(0, &browser, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto close = qScopeGuard([&] { if (modal) modal->reject(); });
        auto* dialog = dynamic_cast<content::PostProcessDialog*>(modal);
        ASSERT_NE(dialog, nullptr);
        opened = true;
        EXPECT_EQ(encoded(dialog->definition()), encoded(expected));
        auto* entry = dialog->findChild<QLineEdit*>("postprocessEntry");
        ASSERT_NE(entry, nullptr);
        entry->setText("UnsavedPS");
    });
    view->activated(index(*view, "Effects.postprocess.json"));
    ASSERT_TRUE(opened);
    ASSERT_TRUE(engine::ReadTextFile(file, textAfter, error)) << error;
    EXPECT_EQ(textAfter, textBefore);
    ASSERT_TRUE(engine::ReadTextFile(engine::AssetMetadataPath(file), metaAfter, error)) << error;
    EXPECT_EQ(metaAfter, metaBefore);
}

TEST_F(RenderPipelineEditor, NewMenuCreatesPostProcessMetadataAndDuplicatesKeepTheCompoundSuffix) {
    content::ContentBrowser browser(context);
    QAction* create = nullptr;
    for (auto* button : browser.findChildren<QToolButton*>()) {
        if (!button->menu()) continue;
        for (auto* action : button->menu()->actions())
            if (action->text() == "Postprocess...") create = action;
    }
    ASSERT_NE(create, nullptr);
    ASSERT_TRUE(create->isEnabled());
    bool opened = false;
    QTimer::singleShot(0, &browser, [&] {
        auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        auto close = qScopeGuard([&] { if (modal) modal->reject(); });
        auto* dialog = dynamic_cast<content::PostProcessDialog*>(modal);
        ASSERT_NE(dialog, nullptr);
        opened = true;
        EXPECT_EQ(dialog->windowTitle(), "New Postprocess");
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        auto* picker = dialog->findChild<editor::AssetPicker*>("postprocessShader");
        ASSERT_NE(buttons, nullptr);
        ASSERT_NE(picker, nullptr);
        auto* ok = buttons->button(QDialogButtonBox::Ok);
        ASSERT_NE(ok, nullptr);
        EXPECT_FALSE(ok->isEnabled());
        ASSERT_TRUE(picker->selectId(shaderId));
        ASSERT_TRUE(ok->isEnabled());
        close.dismiss();
        ok->click();
    });
    create->trigger();
    ASSERT_TRUE(opened);
    const auto file = project.ContentDir() / "New Postprocess.postprocess.json";
    engine::AssetMetadata metadata;
    engine::PostProcessDefinition saved;
    std::string error;
    ASSERT_TRUE(engine::ReadAssetMetadata(file, metadata, error)) << error;
    EXPECT_EQ(metadata.kind, engine::CatalogAssetKind::PostProcess);
    ASSERT_TRUE(engine::LoadPostProcessDefinition(file, saved, error)) << error;
    EXPECT_EQ(saved.shader.id, shaderId);
    EXPECT_EQ(saved.options.entryPoint, "PSMain");
    EXPECT_TRUE(saved.options.defines.empty());
    EXPECT_NE(host.GetAssets().GetCatalog().Find(metadata.id, engine::CatalogAssetKind::PostProcess), nullptr);
    content::AssetOperations operations(context, &window);
    const auto copies = operations.duplicate({editor::toQString(file)});
    ASSERT_EQ(copies.size(), 1);
    EXPECT_TRUE(copies.front().endsWith("New Postprocess (1).postprocess.json"));
    engine::AssetMetadata copyMetadata;
    ASSERT_TRUE(engine::ReadAssetMetadata(editor::toPath(copies.front()), copyMetadata, error)) << error;
    EXPECT_NE(copyMetadata.id, metadata.id);
    EXPECT_EQ(copyMetadata.kind, metadata.kind);
}
} // namespace
