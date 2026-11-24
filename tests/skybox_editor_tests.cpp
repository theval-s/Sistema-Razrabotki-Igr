#include "asset_dialogs.hpp"
#include "asset_operations.hpp"
#include "environment_panel.hpp"

#include <editor/asset_icons.hpp>
#include <editor/asset_picker.hpp>
#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/scene_history.hpp>
#include <editor/viewport_tool_bar.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <DockManager.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMimeData>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <memory>

namespace {
QDoubleSpinBox* number(QWidget& panel, const QString& label) {
    for (auto* form : panel.findChildren<QFormLayout*>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            const auto* item = form->itemAt(row, QFormLayout::LabelRole);
            const auto* text = item ? qobject_cast<QLabel*>(item->widget()) : nullptr;
            if (text && text->text() == label) {
                auto* field = form->itemAt(row, QFormLayout::FieldRole);
                return field ? qobject_cast<QDoubleSpinBox*>(field->widget()) : nullptr;
            }
        }
    }
    return nullptr;
}

class SkyboxEditorTests : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        previousRoot = engine::AssetPathResolver::GetGameRoot();
        project.file = editor::toPath(directory.filePath("skybox.sriproject"));
        ASSERT_TRUE(QDir().mkpath(editor::toQString(project.ContentDir())));
        engine::AssetPathResolver::SetGameRoot(project.ContentDir());
    }
    void TearDown() override { engine::AssetPathResolver::SetGameRoot(previousRoot); }

    QTemporaryDir directory;
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    engine::Project project;
    QMainWindow window;
    ads::CDockManager docks{&window};
    QWidget viewport;
    editor::ViewportToolBar toolbar;
    editor::EditorContext context{host, project, window, docks, viewport, toolbar};
    std::filesystem::path previousRoot;
};

TEST_F(SkyboxEditorTests, CubemapChoicesAndDropsAreDdsOnlyAndUnresolvedReferencesSurviveEditing) {
    std::string error;
    const auto dds = project.ContentDir() / "sky.DDS";
    const auto png = project.ContentDir() / "flat.png";
    const auto ddsId = engine::NewUuid();
    ASSERT_TRUE(engine::CreateNewFile(dds, "shape validated by engine loader", error)) << error;
    ASSERT_TRUE(engine::CreateNewFile(png, "not a cubemap", error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(dds, {ddsId, engine::CatalogAssetKind::Texture}, error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(png, {engine::NewUuid(), engine::CatalogAssetKind::Texture}, error)) << error;
    engine::AssetCatalog catalog;
    ASSERT_TRUE(catalog.Scan(project.ContentDir(), {}, error)) << error;
    content::SkyboxDialog dialog(catalog, editor::toQString(project.ContentDir()), "Sky", nullptr);
    auto* picker = dialog.findChild<editor::AssetPicker*>("skyboxCubemap");
    ASSERT_NE(picker, nullptr);
    EXPECT_EQ(picker->count(), 1);
    EXPECT_TRUE(picker->itemText(0).endsWith(".DDS"));

    QMimeData mime;
    mime.setData(editor::AssetPathsMimeType, editor::toQString(png).toUtf8());
    QDragEnterEvent rejected(QPoint(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(picker, &rejected);
    EXPECT_FALSE(rejected.isAccepted());
    mime.setData(editor::AssetPathsMimeType, editor::toQString(dds).toUtf8());
    QDragEnterEvent accepted(QPoint(2, 2), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(picker, &accepted);
    EXPECT_TRUE(accepted.isAccepted());

    const engine::SkyboxDefinition missing{1, {engine::NewUuid(), "game/missing.dds"}};
    dialog.load(missing);
    EXPECT_EQ(picker->currentIndex(), -1);
    EXPECT_EQ(dialog.definition(), missing);
    picker->reload();
    EXPECT_EQ(dialog.definition(), missing);
    ASSERT_TRUE(picker->selectId(ddsId));
    EXPECT_EQ(dialog.definition().cubemap.id, ddsId);
    EXPECT_EQ(content::splitAssetName("Sky.skybox.json", false).first, "Sky");
    EXPECT_FALSE(editor::assetIcon(engine::CatalogAssetKind::Skybox).isNull());
    EXPECT_EQ(editor::assetKindName(engine::CatalogAssetKind::Skybox), "Skyboxes");
}

TEST_F(SkyboxEditorTests, MaterialEmissionOverridesPreserveSparseClearAndMissingReferences) {
    engine::AssetCatalog catalog;
    content::MaterialInstanceDialog dialog(catalog, editor::toQString(project.ContentDir()), "Instance", nullptr);
    engine::MaterialInstanceDefinition source;
    source.version = 2;
    source.parameters.emission = DirectX::SimpleMath::Vector3(4.25f, 2.f, 0.5f);
    source.parameters.lightingWeight = 0.25f;
    source.parameters.emissionTexture = "game/missing.png";
    source.parameters.emissionReference = engine::AssetReference{engine::NewUuid(), "game/missing.png"};
    dialog.load(source);
    auto result = dialog.definition();
    ASSERT_TRUE(result.parameters.emission);
    EXPECT_EQ(*result.parameters.emission, *source.parameters.emission);
    EXPECT_EQ(result.parameters.lightingWeight, source.parameters.lightingWeight);
    ASSERT_TRUE(result.parameters.emissionReference);
    EXPECT_EQ(result.parameters.emissionReference->id, source.parameters.emissionReference->id);
    auto* mode = dialog.findChild<QComboBox*>("emissionTextureMode");
    auto* emission = dialog.findChild<QCheckBox*>("overrideEmission");
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(emission, nullptr);
    mode->setCurrentIndex(2);
    emission->setChecked(false);
    result = dialog.definition();
    ASSERT_TRUE(result.parameters.emissionTexture);
    EXPECT_TRUE(result.parameters.emissionTexture->empty());
    EXPECT_FALSE(result.parameters.emissionReference);
    EXPECT_FALSE(result.parameters.emission);
    mode->setCurrentIndex(0);
    EXPECT_FALSE(dialog.definition().parameters.emissionTexture);

    content::MaterialDialog material(catalog, editor::toQString(project.ContentDir()), "Material", nullptr);
    engine::MaterialDefinition defaults;
    defaults.version = 1;
    defaults.defaults.emissionTexture = "";
    defaults.defaults.emission = DirectX::SimpleMath::Vector3(3.f, 0.f, 1.f);
    material.load(defaults);
    const auto saved = material.definition();
    EXPECT_EQ(saved.version, 1);
    EXPECT_EQ(saved.defaults.emissionTexture, defaults.defaults.emissionTexture);
    ASSERT_TRUE(saved.defaults.emission);
    EXPECT_EQ(*saved.defaults.emission, *defaults.defaults.emission);
}

TEST_F(SkyboxEditorTests, EnvironmentEditsPreserveMissingUuidAndHdrAndParticipateInHistory) {
    engine::EnvironmentSettings original;
    original.skybox = {engine::NewUuid(), "game/absent.skybox.json"};
    original.tint = {4.25f, 2.5f, 1.75f};
    original.fallbackColor = {0.125f, 0.25f, 3.0f};
    std::string error;
    ASSERT_TRUE(context.scene().SetEnvironment(original, error)) << error;
    context.history().clear();
    std::unique_ptr<QWidget> panel(createEnvironmentPanel(context));
    auto* brightness = number(*panel, "Brightness");
    auto* yaw = number(*panel, "Yaw (degrees)");
    auto* picker = panel->findChild<editor::AssetPicker*>("environmentSkybox");
    ASSERT_NE(brightness, nullptr);
    ASSERT_NE(yaw, nullptr);
    ASSERT_NE(picker, nullptr);
    EXPECT_EQ(picker->currentIndex(), -1);
    brightness->setValue(2.5);
    EXPECT_EQ(context.scene().GetEnvironment().skybox, original.skybox);
    EXPECT_EQ(context.scene().GetEnvironment().tint, original.tint);
    EXPECT_EQ(context.scene().GetEnvironment().fallbackColor, original.fallbackColor);
    EXPECT_FLOAT_EQ(context.scene().GetEnvironment().brightness, 2.5f);
    context.history().stack().undo();
    EXPECT_EQ(context.scene().GetEnvironment(), original);
    context.history().stack().redo();
    EXPECT_FLOAT_EQ(context.scene().GetEnvironment().brightness, 2.5f);

    yaw->setValue(90);
    EXPECT_NEAR(context.scene().GetEnvironment().yawRadians, DirectX::XM_PIDIV2, 1e-6f);
    picker->setCurrentIndex(0);
    EXPECT_FALSE(context.scene().GetEnvironment().skybox.id.IsValid());
    context.history().stack().undo();
    EXPECT_EQ(context.scene().GetEnvironment().skybox, original.skybox);

    const auto beforeDisabled = context.scene().GetEnvironment();
    context.history().setEnabled(false);
    brightness->setValue(17);
    yaw->setValue(180);
    picker->setCurrentIndex(-1);
    picker->setCurrentIndex(0);
    EXPECT_EQ(context.scene().GetEnvironment(), beforeDisabled);
    context.history().setEnabled(true);
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("SRI-tests");
    QCoreApplication::setApplicationName("skybox-editor");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
