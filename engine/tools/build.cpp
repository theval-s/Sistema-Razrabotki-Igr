#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetPipeline.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>
#include <engine/core/Utf.hpp>
#include <engine/project/Project.hpp>
#include <engine/project/RuntimeSettings.hpp>
#include <engine/scene/SceneDocument.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

    namespace fs = std::filesystem;

    constexpr const wchar_t* BuildStampName = L".sri-build";
    constexpr std::string_view BuildStampText =
        "Made by sri-build. The next build deletes and replaces this whole folder.\n";

    struct Options {
        fs::path project;
        fs::path out;
        fs::path runtime;

        engine::RuntimeSettings settings;
    };

    void Info(const std::string& message) {
        std::cout << "info: " << message << '\n';
    }

    void Warning(const std::string& message) {
        std::cout << "warning: " << message << '\n';
    }

    void Error(const std::string& message) {
        std::cout << "error: " << message << '\n';
    }

    bool ParsePositiveInteger(
        std::wstring_view text,
        std::uint32_t& result
    ) {
        const auto value = engine::utf::ToUtf8(text);

        if (!value || value->empty()) {
            return false;
        }

        std::uint32_t parsed = 0;

        const auto [end, ec] = std::from_chars(
            value->data(),
            value->data() + value->size(),
            parsed
        );

        if (ec != std::errc{} ||
            end != value->data() + value->size() ||
            parsed == 0) {
            return false;
        }

        result = parsed;
        return true;
    }

    void PrintUsage() {
        std::cout
            << "Usage:\n"
            << "  sri-build --project <file> --out <dir> "
            << "[--runtime <dir>] [--title T] [--width N] [--height N] "
            << "[--fullscreen] [--capture-mouse]\n";
    }

    bool ParseOptions(
        int argc,
        wchar_t* argv[],
        Options& options,
        const fs::path& toolRoot,
        std::string& error
    ) {
        options.runtime = toolRoot / L"runtime";

        for (int i = 1; i < argc; ++i) {
            const std::wstring_view argument(argv[i]);

            const auto ReadPathArgument = [&](fs::path& destination) -> bool {
                if (i + 1 >= argc || argv[i + 1][0] == L'\0') {
                    error = "Missing value after " + engine::ToUtf8(fs::path(argument));
                    return false;
                }

                destination = fs::path(argv[++i]);
                return true;
                };

            const auto ReadStringArgument = [&](std::string& destination) -> bool {
                if (i + 1 >= argc) {
                    error = "Missing value after " + engine::ToUtf8(fs::path(argument));
                    return false;
                }

                auto text = engine::utf::ToUtf8(std::wstring_view(argv[++i]));

                if (!text) {
                    error = "Could not convert command-line text to UTF-8";
                    return false;
                }

                destination = std::move(*text);
                return true;
                };

            if (argument == L"--project") {
                if (!ReadPathArgument(options.project)) {
                    return false;
                }
            }
            else if (argument == L"--out") {
                if (!ReadPathArgument(options.out)) {
                    return false;
                }
            }
            else if (argument == L"--runtime") {
                if (!ReadPathArgument(options.runtime)) {
                    return false;
                }
            }
            else if (argument == L"--title") {
                if (!ReadStringArgument(options.settings.title)) {
                    return false;
                }
            }
            else if (argument == L"--width") {
                if (i + 1 >= argc ||
                    !ParsePositiveInteger(argv[i + 1], options.settings.width)) {
                    error = "Invalid value for --width";
                    return false;
                }
                ++i;
            }
            else if (argument == L"--height") {
                if (i + 1 >= argc || !ParsePositiveInteger(argv[i + 1], options.settings.height)) {
                    error = "Invalid value for --height";
                    return false;
                }
                ++i;
            }
            else if (argument == L"--fullscreen") {
                options.settings.fullscreen = true;
            }
            else if (argument == L"--capture-mouse") {
                options.settings.captureMouse = true;
            }
            else {
                error = "Unknown argument: " + engine::ToUtf8(fs::path(argument));
                return false;
            }
        }

        if (options.project.empty()) {
            error = "Required argument --project was not specified";
            return false;
        }

        if (options.out.empty()) {
            error = "Required argument --out was not specified";
            return false;
        }

        options.project = fs::absolute(options.project).lexically_normal();
        options.out = fs::absolute(options.out).lexically_normal();
        options.runtime = fs::absolute(options.runtime).lexically_normal();

        return true;
    }

    bool IsSameOrInside(const fs::path& candidate, const fs::path& base) {
        const fs::path normalizedCandidate = fs::weakly_canonical(candidate);
        const fs::path normalizedBase = fs::weakly_canonical(base);
        const fs::path relative = normalizedCandidate.lexically_relative(normalizedBase);

        if (relative.empty()) return false;
        const auto first = relative.begin();
        return *first != L"..";
    }

    bool PathsOverlap(const fs::path& first, const fs::path& second) {
        return IsSameOrInside(first, second) || IsSameOrInside(second, first);
    }

    bool Validate(
        const engine::Project& project,
        std::vector<std::string>& errors,
        std::vector<std::string>& warnings
    ) {
        engine::AssetPathResolver::SetGameRoot(project.ContentDir());

        engine::AssetCatalog catalog;
        std::string error;

        const fs::path engineRoot =
            engine::AssetPathResolver::GetRoot() / L"engine";

        if (!catalog.Scan(project.ContentDir(), engineRoot, error)) {
            errors.push_back("asset catalog: " + error);
            return false;
        }

        const auto* start = catalog.FindPath(project.startupScene);

        if (!start ||
            start->metadata.kind != engine::CatalogAssetKind::Scene) {
            errors.push_back("startup scene '" + project.startupScene + "' is not in the catalog");
        }

        for (const auto* scene :
            catalog.List(engine::CatalogAssetKind::Scene)) {

            std::string text;
            engine::SceneDocument document;

            error.clear();

            if (!engine::ReadTextFile(scene->source, text, error) ||
                !engine::ParseSceneDocument(text, document, error)) {

                errors.push_back(scene->path + ": " + error);
                continue;
            }
            if (scene == start && !document.activeCamera) {
                warnings.push_back(scene->path + ": no active camera, the game would show nothing");
            }
        }

        for (const auto& autoload : project.autoloads) {
            if (!autoload.enabled) {
                continue;
            }

            if (!catalog.Find(
                autoload.script.id,
                engine::CatalogAssetKind::Script)) {

                errors.push_back("autoload '" + autoload.name + "': script " + autoload.script.id.ToString() + " not found");
            }
        }

        engine::MaterialImporter materialImporter;

        for (const auto* material :
            catalog.List(engine::CatalogAssetKind::Material)) {

            engine::AssetDesc desc;
            desc.name = material->path;
            desc.path = material->source;
            desc.catalog = &catalog;

            engine::CpuPayload payload;

            error.clear();

            if (!materialImporter.ReadCpu(
                desc,
                nullptr,
                payload,
                error
            )) {

                errors.push_back(material->path + ": " + error);
            }
        }

        return errors.empty();
    }

    bool Stage(
        const Options& options,
        const engine::Project& project,
        std::string& error
    ) {

        if (!fs::is_directory(options.runtime)) {
            error = "runtime directory does not exist: " + engine::ToUtf8(options.runtime);
            return false;
        }

        const fs::path runtimeExecutable = options.runtime / L"sri_runtime.exe";

        if (!fs::is_regular_file(runtimeExecutable)) {
            error = "runtime executable not found: " + engine::ToUtf8(runtimeExecutable);
            return false;
        }

        if (!fs::is_directory(project.ContentDir())) {
            error = "project content directory does not exist: " + engine::ToUtf8(project.ContentDir());
            return false;
        }

        if (PathsOverlap(options.out, options.runtime)) {
            error = "output directory must not overlap the runtime directory";
            return false;
        }

        if (PathsOverlap(options.out, project.Root())) {
            error = "output directory must not overlap the project directory";
            return false;
        }

        const fs::path projectName = engine::FromUtf8(project.name);

        if (projectName.empty() ||
            projectName == L"." ||
            projectName == L".." ||
            projectName.has_parent_path() ||
            projectName.filename() != projectName) {

            error = "invalid project name for the output executable";
            return false;
        }

        // Only folders carrying this stamp are ever deleted: game.json alone is too common a name.
        const fs::path stamp = options.out / BuildStampName;

        if (fs::exists(options.out)) {
            if (!fs::is_directory(options.out)) {
                error = "output path exists and is not a directory: " + engine::ToUtf8(options.out);
                return false;
            }

            if (!fs::is_empty(options.out) && !fs::is_regular_file(stamp)) {
                error = "refusing to overwrite '" + engine::ToUtf8(options.out) + "': not empty and not made by sri-build";
                return false;
            }
        }

        std::string json;
        if (!engine::SerializeRuntimeSettings(options.settings, json, error)) return false;
        if (engine::RuntimeSettings check; !engine::ParseRuntimeSettings(json, check, error)) {
            error = "invalid runtime settings: " + error;
            return false;
        }

        fs::remove_all(options.out);
        fs::create_directories(options.out);

        // Stamp first, so the next build can still replace a half-finished one.
        if (!engine::WriteFileAtomic(stamp, BuildStampText, error) ||
            !engine::WriteFileAtomic(options.out / engine::RuntimeSettings::FileName, json, error)) {
            return false;
        }

        fs::copy(
            options.runtime,
            options.out,
            fs::copy_options::recursive |
            fs::copy_options::overwrite_existing
        );

        fs::rename(
            options.out / L"sri_runtime.exe",
            options.out / (projectName.native() + L".exe")
        );

        fs::copy(project.ContentDir(), options.out / L"game", fs::copy_options::recursive);
        fs::copy_file(project.file, options.out / L"game.sriproject");

        return true;
    }

} // namespace

int wmain(int argc, wchar_t* argv[]) {

    std::cout << std::unitbuf;
    SetConsoleOutputCP(CP_UTF8);

    try {
        const fs::path toolRoot = engine::AssetPathResolver::GetRoot();

        Options options;
        std::string error;

        if (!ParseOptions(
            argc,
            argv,
            options,
            toolRoot,
            error
        )) {

            PrintUsage();
            Error(error);
            return 1;
        }

        Info("Project: " + engine::ToUtf8(options.project));
        Info("Output: " + engine::ToUtf8(options.out));
        Info("Runtime: " + engine::ToUtf8(options.runtime));

        auto project = engine::Project::Load(options.project,error);

        if (!project) {
            Error(error);
            return 1;
        }

        Info("Loaded project '" + project->name + "'");

        std::vector<std::string> errors;
        std::vector<std::string> warnings;

        if (!Validate(*project, errors, warnings)) {
            for (const auto& warning : warnings) {
                Warning(warning);
            }

            for (const auto& validationError : errors) {
                Error(validationError);
            }

            return 1;
        }

        for (const auto& warning : warnings) {
            Warning(warning);
        }

        Info("Project validation passed");

        if (!Stage(options, *project, error)) {
            Error(error);
            return 1;
        }

        Info("Build completed successfully");
        Info("Game output: " + engine::ToUtf8(options.out));

        return 0;
    }
    catch (const fs::filesystem_error& e) {
        Error(std::string("filesystem error: ") + e.what());
        return 1;
    }
    catch (const std::exception& e) {
        Error(std::string("build failed: ") + e.what());
        return 1;
    }
    catch (...) {
        Error("build failed: unknown exception");
        return 1;
    }
}
