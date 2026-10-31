#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <engine/engine_export.hpp>
#include <engine/scripting/AutoloadDefinition.hpp>

namespace engine {

struct Project {
    static constexpr const char* FileExtension = ".sriproject";

    /// Absolute path of the .sriproject file.
    std::filesystem::path file;
    std::string name;
    std::string startupScene;
    std::vector<AutoloadDefinition> autoloads;

    [[nodiscard]] ENGINE_API std::filesystem::path Root() const;
    [[nodiscard]] ENGINE_API std::filesystem::path ContentDir() const;

    /// Paths and error messages are UTF-8 because russian happens on windows sometimes.
    [[nodiscard]] ENGINE_API static std::optional<Project> Load(const std::filesystem::path& file, std::string& error);
    ENGINE_API bool Save(std::string& error) const;
    /// Writes <directory>/<name>.sriproject and creates <directory>/content/scenes.
    /// Fails if that project file already exists.
    [[nodiscard]] ENGINE_API static std::optional<Project> Create(const std::filesystem::path& directory,
                                                                 std::string name, std::string& error);
};

}  // namespace engine
