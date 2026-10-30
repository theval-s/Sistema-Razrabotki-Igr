#include "asset_picker.hpp"

#include <QCompleter>
#include <QLineEdit>

#include "asset_icons.hpp"

namespace content {

AssetPicker::AssetPicker(const engine::AssetCatalog& catalog, const engine::CatalogAssetKind kind,
                         const QString& none, QWidget* parent)
    : QComboBox(parent) {
    if (!none.isEmpty()) {
        addItem(none);
    }
    const QIcon icon = assetIcon(kind);
    for (const engine::CatalogEntry* entry : catalog.List(kind)) {
        addItem(icon, QString::fromStdString(entry->path), QString::fromStdString(entry->metadata.id.ToString()));
    }

    // Every path starts with game/ or engine/, so plain type-ahead wouldn't get far: match anywhere.
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    completer()->setFilterMode(Qt::MatchContains);
    completer()->setCompletionMode(QCompleter::PopupCompletion);
    // Text that names no asset goes back to the chosen one.
    connect(lineEdit(), &QLineEdit::editingFinished, this, [this] {
        const int index = findText(currentText(), Qt::MatchFixedString);
        if (index >= 0) {
            setCurrentIndex(index);
        } else {
            setEditText(itemText(currentIndex()));
        }
    });
    setMinimumContentsLength(32);
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
}

std::optional<engine::AssetReference> AssetPicker::reference() const {
    const QString id = currentData().toString();
    if (id.isEmpty()) {
        return std::nullopt;
    }
    return engine::AssetReference{engine::AssetUuid(id.toStdString()), itemText(currentIndex()).toStdString()};
}

bool AssetPicker::selectPath(const QString& path) {
    const int index = findText(path, Qt::MatchFixedString);
    if (index < 0) {
        return false;
    }
    setCurrentIndex(index);
    return true;
}

}  // namespace content
