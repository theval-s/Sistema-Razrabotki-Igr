#pragma once

#include <QCollator>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QStringList>

#include <optional>

#include <engine/assets/AssetCatalog.hpp>

class QFileSystemModel;
class QMimeData;

namespace content {

/// To not show .meta, folders first, asset icons by kind, and filtered by name and kind.
///
/// Renames and drops = signals, not QFileSystemModel, because we need .meta files :(
/// Drags carry their own MIME type, not file URLs, so no drag out, but yes drag in
class AssetFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    enum class Mode {
        /// Directories only, for the folder tree.
        Folders,
        /// Everything in one folder, for the item view; only that folder's children are filtered.
        Contents,
    };

    AssetFilterModel(QFileSystemModel& files, Mode mode, QObject* parent = nullptr);

    /// Contents mode: the folder (a source index) whose children the filters apply to.
    void setFolder(const QModelIndex& folder);
    /// Case-insensitive wildcard matched anywhere in the name; empty shows everything.
    void setNameFilter(const QString& pattern);
    /// Files of one kind only, plus the folders; nullopt shows every file.
    void setKindFilter(std::optional<engine::CatalogAssetKind> kind);
    /// Lets views rename items and drop onto folders. Off by default: items can still be dragged and copied.
    void setWritable(bool writable);

    /// The paths a drag carries, from this model or as file URLs from elsewhere.
    [[nodiscard]] static QStringList droppedPaths(const QMimeData* data);

    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] bool canDropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                                       const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column,
                      const QModelIndex& parent) override;

signals:
    /// An item's name was edited in a view. Absolute path.
    void renameRequested(const QString& path, const QString& name);
    /// `paths` were dropped onto `folder` (absolute), to be moved or copied.
    void dropRequested(const QStringList& paths, const QString& folder, Qt::DropAction action);

protected:
    [[nodiscard]] bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;
    [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    QFileSystemModel& files_;
    Mode mode_;
    QPersistentModelIndex folder_;
    QRegularExpression name_;
    std::optional<engine::CatalogAssetKind> kind_;
    QCollator collator_;
    bool writable_ = false;
};

}  // namespace content
