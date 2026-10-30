#pragma once

#include <QComboBox>
#include <QString>

#include <optional>

#include <engine/assets/AssetCatalog.hpp>

class QWidget;

namespace content {

/// Chooses an asset of one kind from the catalog, listed by path. Typing filters the list.
class AssetPicker : public QComboBox {
    Q_OBJECT

public:
    /// `none` is the text of an extra first entry that stands for no asset; empty for no such entry.
    AssetPicker(const engine::AssetCatalog& catalog, engine::CatalogAssetKind kind, const QString& none = {},
                QWidget* parent = nullptr);

    /// The chosen asset; nothing for the "none" entry or an empty list.
    [[nodiscard]] std::optional<engine::AssetReference> reference() const;
    /// Chooses the asset at this catalog path (case-insensitive). False if it isn't listed.
    bool selectPath(const QString& path);
};

}  // namespace content
