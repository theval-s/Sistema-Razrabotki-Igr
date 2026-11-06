#include <editor/module_loader.hpp>

#include <QDir>
#include <QLibrary>

#include <ranges>

#include <editor/editor_log.hpp>

#include <editor/editor_module.hpp>

namespace editor {

namespace {

using CreateModuleFn = IEditorModule* (*)();

}  // namespace

ModuleLoader::ModuleLoader() = default;

ModuleLoader::~ModuleLoader() {
    //using std::ranges and std::views is scary for me
    for (Loaded& loaded : std::views::reverse(modules_)) {
        editorLog().info("Shutting down editor module '{}'", loaded.module->name());
        loaded.module->shutdown();
        loaded.module.reset();
    }
    // QLibrary's destructor does not unload: the DLLs stay mapped until exit.
}

void ModuleLoader::loadAll(const QString& directory, EditorContext& context) {
    const QDir dir(directory);
    // By name: vcpkg copies the modules' dependency DLLs into the same directory.
    const QStringList files = dir.entryList({QStringLiteral("editor_module_*.dll")}, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        editorLog().warn("No editor modules found in '{}'", directory.toStdString());
        return;
    }

    for (const QString& file : files) {
        const std::string path = dir.filePath(file).toStdString();
        auto library = std::make_unique<QLibrary>(dir.filePath(file));
        if (!library->load()) {
            editorLog().error("Editor module '{}' failed to load: {}", path, library->errorString().toStdString());
            continue;
        }
        const auto create = reinterpret_cast<CreateModuleFn>(library->resolve(ModuleEntryPoint));
        if (create == nullptr) {
            editorLog().error("'{}' has no {}(), not an editor module", path, ModuleEntryPoint);
            continue;
        }
        std::unique_ptr<IEditorModule> module{create()};
        if (module == nullptr) {
            editorLog().error("Editor module '{}' returned no module", path);
            continue;
        }
        editorLog().info("Starting editor module '{}' from {}", module->name(), file.toStdString());
        module->startup(context);
        modules_.push_back({std::move(library), std::move(module)});
    }
}

}  // namespace editor
