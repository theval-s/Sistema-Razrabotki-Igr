#include "audio_panel.hpp"
#include <editor/editor_context.hpp>
#include <editor/editor_module.hpp>
#include <engine/assets/AssetCatalog.hpp>

namespace {
class AudioModule final : public editor::IEditorModule {
public:
    std::string_view name() const override { return "Audio"; }
    void startup(editor::EditorContext& context) override {
        context_ = &context;
        panel_ = new audio_editor::AudioPanel(context);
        context.addPanel(QStringLiteral("AudioPanel"), QObject::tr("Audio"), panel_, Qt::BottomDockWidgetArea);
        context.setAssetEditor(engine::CatalogAssetKind::AudioClip, [&context, panel = panel_](const std::filesystem::path& path) {
            context.showPanel(panel); panel->open(path);
        });
    }
    void shutdown() override {
        context_->setAssetEditor(engine::CatalogAssetKind::AudioClip, {});
        panel_->stopOwned();
    }
private:
    editor::EditorContext* context_ = nullptr;
    audio_editor::AudioPanel* panel_ = nullptr;
};
}
SRI_EDITOR_MODULE(AudioModule)
