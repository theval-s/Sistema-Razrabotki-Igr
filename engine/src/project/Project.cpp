#include <engine/project/Project.hpp>

#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include <engine/core/FileSystem.hpp>
#include <engine/core/PathUtils.hpp>

namespace engine {
namespace {

constexpr int FormatVersion = 1;

bool IsValidName(const std::string_view name) {
    return !name.empty() && name.find_first_of("<>:\"/\\|?*") == std::string_view::npos &&
           name.back() != '.' && name.back() != ' ';
}

}  // namespace

std::filesystem::path Project::Root() const {
    return file.parent_path();
}

std::filesystem::path Project::ContentDir() const {
    return Root() / "content";
}

std::optional<Project> Project::Load(const std::filesystem::path& file, std::string& error) {
    std::string text;
    if (!ReadTextFile(file, text, error)) {
        return std::nullopt;
    }
    try {
        const nlohmann::json data = nlohmann::json::parse(text);
        if (const int version = data.value("version", 0); version > FormatVersion) {
            error = ToUtf8(file) + " was made by a newer editor (format version " + std::to_string(version) + ")";
            return std::nullopt;
        }
        Project project;
        project.file = std::filesystem::absolute(file).lexically_normal();
        project.name = data.value("name", ToUtf8(file.stem()));
        project.startupScene = data.value("startupScene", std::string{});
        return project;
    } catch (const nlohmann::json::exception& e) {
        error = ToUtf8(file) + " is not a valid project file: " + e.what();
        return std::nullopt;
    }
}

bool Project::Save(std::string& error) const {
    const nlohmann::json data = {
        {"version", FormatVersion},
        {"name", name},
        {"startupScene", startupScene},
    };
    return WriteFileAtomic(file, data.dump(2) + '\n', error);
}

std::optional<Project> Project::Create(const std::filesystem::path& directory, std::string name, std::string& error) {
    if (!IsValidName(name)) {
        error = "'" + name + "' is not a valid project name";
        return std::nullopt;
    }
    Project project;
    project.file = std::filesystem::absolute(directory / FromUtf8(name + FileExtension)).lexically_normal();
    project.name = std::move(name);
    if (std::filesystem::exists(project.file)) {
        error = ToUtf8(project.file) + " already exists";
        return std::nullopt;
    }
    std::error_code ec;
    std::filesystem::create_directories(project.ContentDir() / "scenes", ec);
    if (ec) {
        error = "Cannot create " + ToUtf8(project.ContentDir()) + ": " + ec.message();
        return std::nullopt;
    }
    if (!project.Save(error)) {
        return std::nullopt;
    }
    return project;
}

}  // namespace engine
