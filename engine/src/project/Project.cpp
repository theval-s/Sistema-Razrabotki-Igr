#include <engine/project/Project.hpp>

#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace engine {
namespace {

constexpr int FormatVersion = 1;

// god I hate encodings
// and I hope internet got it right
std::string ToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path FromUtf8(const std::string_view text) {
    return std::u8string(text.begin(), text.end());
}

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
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        error = "Cannot open " + ToUtf8(file);
        return std::nullopt;
    }
    try {
        const nlohmann::json data = nlohmann::json::parse(input);
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
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << data.dump(2) << '\n';
    if (!output) {
        error = "Cannot write " + ToUtf8(file);
        return false;
    }
    return true;
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
