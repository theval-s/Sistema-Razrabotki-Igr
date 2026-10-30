#pragma once

#include <QComboBox>
#include <QString>

#include <optional>

#include <engine/assets/AssetCatalog.hpp>

#include <editor/editor_core_export.hpp>

class QWidget;

namespace editor {

/// Chooses an asset of one kind from the catalog, listed by path. Typing filters the list.
/// The list is read again from the catalog each time it drops down, so assets imported meanwhile
/// show up; `catalog` must outlive the picker.
class EDITOR_CORE_API AssetPicker : public QComboBox {
    Q_OBJECT

public:
    /// `none` is the text of an extra first entry that stands for no asset; empty for no such entry.
    AssetPicker(const engine::AssetCatalog& catalog, engine::CatalogAssetKind kind, const QString& none = {},
                QWidget* parent = nullptr);

    /// The chosen asset; nothing for the "none" entry or an empty list.
    [[nodiscard]] std::optional<engine::AssetReference> reference() const;
    /// Chooses the asset at this catalog path (case-insensitive). False if it isn't listed.
    bool selectPath(const QString& path);
    /// Chooses the asset with this ID. False if it isn't listed.
    bool selectId(const engine::AssetUuid& id);

    /// Lists the catalog's assets again, keeping the chosen one if it's still there.
    /// Quietly: no currentIndexChanged().
    void reload();

    void showPopup() override;

private:
    void populate();

    const engine::AssetCatalog& catalog_;
    engine::CatalogAssetKind kind_;
    QString none_;
};

}  // namespace editor
