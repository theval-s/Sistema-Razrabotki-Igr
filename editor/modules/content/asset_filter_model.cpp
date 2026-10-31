#include "asset_filter_model.hpp"

#include <QFileInfo>
#include <QFileSystemModel>
#include <QMimeData>
#include <QUrl>

#include <editor/asset_icons.hpp>
#include <editor/paths.hpp>

#include "asset_operations.hpp"

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

void AssetFilterModel::setWritable(const bool writable) {
    writable_ = writable;
}

QStringList AssetFilterModel::droppedPaths(const QMimeData* data) {
    QStringList paths;
    if (data->hasFormat(editor::AssetPathsMimeType)) {
        paths = QString::fromUtf8(data->data(editor::AssetPathsMimeType)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    } else {
        for (const QUrl& url : data->urls()) {
            if (url.isLocalFile()) {
                paths << cleanPath(url.toLocalFile());
            }
        }
    }
    return paths;
}

QVariant AssetFilterModel::data(const QModelIndex& index, const int role) const {
    if (role == Qt::DecorationRole && index.column() == 0) {
        if (const auto kind = kindOf(files_, mapToSource(index))) {
            return editor::assetIcon(*kind);
        }
    }
    if (role == Qt::ToolTipRole && index.column() == 0) {
        return files_.fileName(mapToSource(index));  // icon views elide long names
    }
    return QSortFilterProxyModel::data(index, role);
}

Qt::ItemFlags AssetFilterModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags flags = QSortFilterProxyModel::flags(index);
    if (writable_ && index.isValid()) {
        if (index.column() == 0) {
            flags |= Qt::ItemIsEditable;
        }
        if (files_.isDir(mapToSource(index))) {
            flags |= Qt::ItemIsDropEnabled;
        }
    }
    return flags;
}

bool AssetFilterModel::setData(const QModelIndex& index, const QVariant& value, const int role) {
    // Empty: a view clearing dragged items after a move, not a rename.
    if (!writable_ || role != Qt::EditRole || index.column() != 0 || value.toString().isEmpty()) {
        return false;
    }
    emit renameRequested(files_.filePath(mapToSource(index)), value.toString());
    return false;  // the renamed file shows up through the model's file watcher
}

QStringList AssetFilterModel::mimeTypes() const {
    return {editor::AssetPathsMimeType, QStringLiteral("text/uri-list")};
}

QMimeData* AssetFilterModel::mimeData(const QModelIndexList& indexes) const {
    QStringList paths;
    for (const QModelIndex& index : indexes) {
        if (index.column() == 0) {
            paths << files_.filePath(mapToSource(index));
        }
    }
    auto* data = new QMimeData;
    data->setData(editor::AssetPathsMimeType, paths.join(QLatin1Char('\n')).toUtf8());
    return data;
}

Qt::DropActions AssetFilterModel::supportedDragActions() const {
    return writable_ ? Qt::CopyAction | Qt::MoveAction : Qt::CopyAction;
}

Qt::DropActions AssetFilterModel::supportedDropActions() const {
    return Qt::CopyAction | Qt::MoveAction;
}

bool AssetFilterModel::canDropMimeData(const QMimeData* data, const Qt::DropAction action, int /*row*/,
                                       int /*column*/, const QModelIndex& parent) const {
    // Dropped between items, `parent` is their folder; dropped on a folder, it's that folder.
    if (!writable_ || !parent.isValid() || (action != Qt::CopyAction && action != Qt::MoveAction)) {
        return false;
    }
    const QModelIndex folder = mapToSource(parent);
    if (!files_.isDir(folder)) {
        return false;
    }
    const QString target = files_.filePath(folder);
    const QStringList paths = droppedPaths(data);
    bool moves = false;
    for (const QString& path : paths) {
        if (isWithin(target, path)) {
            return false;  // onto itself or into its own subfolder
        }
        moves |= QFileInfo(path).absolutePath().compare(target, Qt::CaseInsensitive) != 0;
    }
    // Moving items into the folder they're already in does nothing.
    return !paths.isEmpty() && (action == Qt::CopyAction || moves);
}

bool AssetFilterModel::dropMimeData(const QMimeData* data, const Qt::DropAction action, const int row,
                                    const int column, const QModelIndex& parent) {
    if (!canDropMimeData(data, action, row, column, parent)) {
        return false;
    }
    emit dropRequested(droppedPaths(data), files_.filePath(mapToSource(parent)), action);
    return true;
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
