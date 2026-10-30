#include <editor/editor_context.hpp>
#include <editor/editor_module.hpp>

#include "content_browser.hpp"

namespace {

class ContentModule final : public editor::IEditorModule {
public:
    [[nodiscard]] std::string_view name() const override {
        return "Content";
    }

    void startup(editor::EditorContext& context) override {
        auto* browser = new content::ContentBrowser(context);
        context.addPanel(QStringLiteral("ContentBrowser"), QObject::tr("Content"), browser, Qt::BottomDockWidgetArea);
    }
};

}  // namespace

SRI_EDITOR_MODULE(ContentModule)
