#include "asset_filter_model.hpp"

#include <QFileSystemModel>

#include <editor/paths.hpp>

#include "asset_icons.hpp"

namespace content {

namespace {

std::optional<engine::CatalogAssetKind> kindOf(const QFileSystemModel& files, const QModelIndex& index) {
    return files.isDir(index) ? std::nullopt : engine::CatalogKindForFile(editor::toPath(files.filePath(index)));
}

}  // namespace

AssetFilterModel::AssetFilterModel(QFileSystemModel& files, const Mode mode, QObject* parent)
    : QSortFilterProxyModel(parent), files_(files), mode_(mode) {
    collator_.setNumericMode(true);  // "cube 2" before "cube 10"
    collator_.setCaseSensitivity(Qt::CaseInsensitive);
    setSourceModel(&files);
    setDynamicSortFilter(true);
    sort(0);
}

void AssetFilterModel::setFolder(const QModelIndex& folder) {
    beginFilterChange();
    folder_ = folder;
    endFilterChange(Direction::Rows);
}

void AssetFilterModel::setNameFilter(const QString& pattern) {
    beginFilterChange();
    name_ = pattern.isEmpty() ? QRegularExpression()
                              : QRegularExpression::fromWildcard(pattern, Qt::CaseInsensitive,
                                                                 QRegularExpression::UnanchoredWildcardConversion);
    endFilterChange(Direction::Rows);
}

void AssetFilterModel::setKindFilter(const std::optional<engine::CatalogAssetKind> kind) {
    beginFilterChange();
    kind_ = kind;
    endFilterChange(Direction::Rows);
}

QVariant AssetFilterModel::data(const QModelIndex& index, const int role) const {
    if (role == Qt::DecorationRole && index.column() == 0) {
        if (const auto kind = kindOf(files_, mapToSource(index))) {
            return assetIcon(*kind);
        }
    }
    if (role == Qt::ToolTipRole && index.column() == 0) {
        return files_.fileName(mapToSource(index));  // icon views elide long names
    }
    return QSortFilterProxyModel::data(index, role);
}

bool AssetFilterModel::filterAcceptsRow(const int source_row, const QModelIndex& source_parent) const {
    const QModelIndex index = files_.index(source_row, 0, source_parent);
    const bool dir = files_.isDir(index);
    if (!dir && files_.fileName(index).endsWith(QLatin1String(".meta"), Qt::CaseInsensitive)) {
        return false;
    }
    if (mode_ == Mode::Folders) {
        return dir;
    }
    // Rows outside the shown folder include its ancestors, which must stay mappable.
    if (folder_ != source_parent) {
        return true;
    }
    if (!name_.pattern().isEmpty() && !name_.match(files_.fileName(index)).hasMatch()) {
        return false;
    }
    return dir || !kind_ || kindOf(files_, index) == kind_;
}

bool AssetFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    const bool left_dir = files_.isDir(left);
    if (left_dir != files_.isDir(right)) {
        return left_dir;
    }
    return collator_.compare(files_.fileName(left), files_.fileName(right)) < 0;
}

}  // namespace content
