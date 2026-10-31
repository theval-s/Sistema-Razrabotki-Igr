#include <editor/asset_picker.hpp>

#include <QCompleter>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QSignalBlocker>

#include <engine/assets/AssetPathResolver.hpp>

#include <editor/asset_icons.hpp>
#include <editor/paths.hpp>

namespace editor {

AssetPicker::AssetPicker(const engine::AssetCatalog& catalog, const engine::CatalogAssetKind kind,
                         const QString& none, QWidget* parent)
    : QComboBox(parent), catalog_(catalog), kind_(kind), none_(none) {
    populate();

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
    // Drops go to the picker: the line edit would paste the path as filter text.
    setAcceptDrops(true);
    lineEdit()->setAcceptDrops(false);
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

bool AssetPicker::selectId(const engine::AssetUuid& id) {
    const int index = findData(QString::fromStdString(id.ToString()));
    if (index < 0) {
        return false;
    }
    setCurrentIndex(index);
    return true;
}

void AssetPicker::reload() {
    const int previous = currentIndex();
    const QString chosen = currentData().toString();
    const QSignalBlocker blocker(this);
    clear();
    populate();
    if (!chosen.isEmpty()) {
        setCurrentIndex(findData(chosen));  // -1 if it's gone
    } else {
        setCurrentIndex(previous < count() ? previous : -1);  // the "none" entry, or nothing
    }
}

void AssetPicker::showPopup() {
    reload();
    QComboBox::showPopup();
}

void AssetPicker::dragEnterEvent(QDragEnterEvent* event) {
    if (droppedEntry(event->mimeData()) != nullptr) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }
}

void AssetPicker::dropEvent(QDropEvent* event) {
    const engine::CatalogEntry* entry = droppedEntry(event->mimeData());
    if (entry == nullptr) {
        return;
    }
    reload();  // it may have been imported since the list was read
    selectId(entry->metadata.id);  // not quietly: this is the user's choice
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

const engine::CatalogEntry* AssetPicker::droppedEntry(const QMimeData* mime) const {
    if (!mime->hasFormat(AssetPathsMimeType)) {
        return nullptr;
    }
    const QStringList paths =
        QString::fromUtf8(mime->data(AssetPathsMimeType)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (paths.size() != 1) {
        return nullptr;
    }
    const engine::CatalogEntry* entry = catalog_.FindPath(engine::AssetPathResolver::ToName(toPath(paths.front())));
    return entry != nullptr && entry->metadata.kind == kind_ ? entry : nullptr;
}

void AssetPicker::populate() {
    if (!none_.isEmpty()) {
        addItem(none_);
    }
    const QIcon icon = assetIcon(kind_);
    for (const engine::CatalogEntry* entry : catalog_.List(kind_)) {
        addItem(icon, QString::fromStdString(entry->path), QString::fromStdString(entry->metadata.id.ToString()));
    }
}

}  // namespace editor
