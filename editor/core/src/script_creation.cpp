#include <editor/script_creation.hpp>

#include <QFileDialog>
#include <QMessageBox>

#include <exception>
#include <string_view>
#include <system_error>

#include <spdlog/spdlog.h>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <editor/paths.hpp>

namespace editor {

    bool createLuaScript(engine::Engine& engine,
        const std::filesystem::path& requested,
        std::filesystem::path& created,
        std::string& error) {
        std::filesystem::path target;

        try {
            const auto file = engine::AssetPathResolver::ToAbsolute(requested);
            QString name = toQString(file.filename());

            if (!name.endsWith(QLatin1String(".lua"), Qt::CaseInsensitive)) {
                name += QStringLiteral(".lua");
            }

            const auto base = file.parent_path() / toPath(name);

            if (engine::CatalogKindForFile(base) != engine::CatalogAssetKind::Script) {
                error = "Choose a .lua filename; .d.lua files are declarations.";
                return false;
            }

            if (!engine::AssetPathResolver::ToName(base).starts_with("game/")) {
                error = "Create scripts inside the project's content directory.";
                return false;
            }

            if (!std::filesystem::is_directory(base.parent_path())) {
                error = "The destination directory does not exist.";
                return false;
            }

            // Both the source and its sidecar must be available.
            for (std::size_t suffix = 0;; ++suffix) {
                target = suffix == 0
                    ? base
                    : base.parent_path() /
                    (base.stem().wstring() + std::to_wstring(suffix) +
                        base.extension().wstring());

                if (!std::filesystem::exists(target) &&
                    !std::filesystem::exists(engine::AssetMetadataPath(target))) {
                    break;
                }
            }
        }
        catch (const std::exception& exception) {
            error = exception.what();
            return false;
        }

        const engine::AssetMetadata metadata{
            engine::NewUuid(), engine::CatalogAssetKind::Script };

        constexpr std::string_view ScriptTemplate = R"(-- This behavior table is created separately for each object.
-- require is for modules shared throughout the current Play session.
-- local settings = require("shared.settings")

return {
    elapsed = 0.0,

    OnStart = function(self)
        -- Initialize this object's behavior.
    end,

    OnUpdate = function(self, deltaSeconds)
        self.elapsed = self.elapsed + deltaSeconds
    end,

    OnDestroy = function(self)
        -- Release this object's gameplay resources.
    end
}
)";

        if (!engine::CreateNewFile(target, ScriptTemplate, error)) {
            return false;
        }

        if (!engine::WriteAssetMetadata(target, metadata, error)) {
            std::error_code cleanup;
            std::filesystem::remove(target, cleanup);
            if (cleanup) {
                error += "\nFailed to remove the incomplete script: " +
                    cleanup.message();
            }
            return false;
        }

        // As with scene creation, retain successfully created files if scanning fails.
        std::string diagnostics;
        if (!engine.RescanAssetCatalogs(diagnostics)) {
            spdlog::warn("Asset catalog: {}", diagnostics);
        }

        created = target;
        error.clear();
        return true;
    }

    bool createLuaScriptInteractive(QWidget* parent,
        engine::Engine& engine,
        const engine::Project& project) {
        const QString chosen = QFileDialog::getSaveFileName(
            parent,
            QObject::tr("New Lua Script"),
            toQString(project.ContentDir() / "NewScript.lua"),
            QObject::tr("Lua Scripts (*.lua)"),
            nullptr,
            QFileDialog::DontConfirmOverwrite);

        if (chosen.isEmpty()) {
            return false;
        }

        std::filesystem::path created;
        std::string error;
        if (!createLuaScript(engine, toPath(chosen), created, error)) {
            QMessageBox::warning(
                parent,
                QObject::tr("New Lua Script"),
                QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size())));
            return false;
        }

        return true;
    }

}  // namespace editor
