#pragma once

#include <QString>

#include <filesystem>

namespace editor {

[[nodiscard]] inline std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

[[nodiscard]] inline QString toQString(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

}  // namespace editor
