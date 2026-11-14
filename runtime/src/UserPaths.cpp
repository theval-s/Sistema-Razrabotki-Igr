#include <filesystem>
#include <string>

#include <ShlObj.h>
#include <objbase.h>

#include <engine/core/PathUtils.hpp>

std::filesystem::path UserDataDirectory(const std::string& gameName) {
    std::filesystem::path base;
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) base = raw;
    CoTaskMemFree(raw);
    if (base.empty()) base = std::filesystem::temp_directory_path();
    return base / engine::FromUtf8(gameName);        // project names are valid folder names (Project::Create)
}