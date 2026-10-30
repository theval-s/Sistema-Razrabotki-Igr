#pragma once

#include <QCollator>
#include <QPersistentModelIndex>
#include <QRegularExpression>
#include <QSortFilterProxyModel>

#include <optional>

#include <engine/assets/AssetCatalog.hpp>

class QFileSystemModel;

namespace content {

/// To not show .meta, folders first, asset icons by kind, and filtered by name and kind.
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

    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

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
};

}  // namespace content
