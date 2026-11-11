#include <editor/editor_context.hpp>
#include <editor/editor_module.hpp>
#include <editor/play_session.hpp>

#include "content_browser.hpp"
#include "scene_placement.hpp"

namespace {

class ContentModule final : public editor::IEditorModule {
public:
    [[nodiscard]] std::string_view name() const override {
        return "Content";
    }

    void startup(editor::EditorContext& context) override {
        auto* browser = new content::ContentBrowser(context);
        context.addPanel(QStringLiteral("ContentBrowser"), QObject::tr("Content"), browser, Qt::BottomDockWidgetArea);
        // Goes with the browser, which the context deletes before it's gone itself.
        new content::ViewportDropTarget(context, browser);
        // Play loads assets from the project as they were when it started.
        QObject::connect(&context.playSession(), &editor::PlaySession::playingChanged, browser, &QWidget::setDisabled);
    }
};

}  // namespace

SRI_EDITOR_MODULE(ContentModule)
