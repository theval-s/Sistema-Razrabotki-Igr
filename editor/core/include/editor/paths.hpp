#pragma once

#include <QString>

#include <filesystem>

namespace editor {

/// Drag-and-drop format of asset files dragged from the Content Browser: absolute paths, one per line.
inline const QString AssetPathsMimeType = QStringLiteral("application/x-sri-asset-paths");

[[nodiscard]] inline std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

[[nodiscard]] inline QString toQString(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

}  // namespace editor
