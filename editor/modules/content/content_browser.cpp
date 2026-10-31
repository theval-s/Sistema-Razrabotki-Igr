#include "content_browser.hpp"

#include <QAction>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <editor/asset_icons.hpp>
#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/scene_document.hpp>

#include "asset_dialogs.hpp"
#include "asset_filter_model.hpp"
#include "scene_placement.hpp"

namespace content {

namespace {

const QString IconSizeKey = QStringLiteral("content/iconSize");
const QString ListModeKey = QStringLiteral("content/listMode");
const QString ImportDirectoryKey = QStringLiteral("content/importDirectory");

constexpr engine::CatalogAssetKind FilterKinds[] = {
    engine::CatalogAssetKind::Scene,    engine::CatalogAssetKind::Mesh,
    engine::CatalogAssetKind::Texture,  engine::CatalogAssetKind::Material,
    engine::CatalogAssetKind::MaterialInstance, engine::CatalogAssetKind::Shader,
    engine::CatalogAssetKind::Script,
};

/// Files dragged in from outside the editor are copied, whatever the drag proposed: reporting
/// a move back would let Explorer delete the originals.
template <class View>
class AssetView : public View {
public:
    using View::View;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override {
        View::dragEnterEvent(event);
        copyExternal(event);
    }
    void dragMoveEvent(QDragMoveEvent* event) override {
        View::dragMoveEvent(event);
        copyExternal(event);
    }
    void dropEvent(QDropEvent* event) override {
        View::dropEvent(event);
        copyExternal(event);
    }

private:
    static void copyExternal(QDropEvent* event) {
        if (event->source() == nullptr && event->isAccepted() && (event->possibleActions() & Qt::CopyAction)) {
            event->setDropAction(Qt::CopyAction);
        }
    }
};

/// Inline rename that preselects the name without its asset suffix, as Explorer does.
class RenameDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        QStyledItemDelegate::setEditorData(editor, index);
        auto* line = qobject_cast<QLineEdit*>(editor);
        if (line == nullptr) {
            return;
        }
        const bool dir = QFileInfo(index.data(QFileSystemModel::FilePathRole).toString()).isDir();
        const auto stem = static_cast<int>(splitAssetName(line->text(), dir).first.size());
        // The view selects all of the text once the editor is open.
        QTimer::singleShot(0, line, [line, stem] { line->setSelection(0, stem); });
    }
};

void setUpDragAndDrop(QAbstractItemView& view) {
    //TODO: figure out drag and drop for viewport, probably needs mime data handling
    view.setDragDropMode(QAbstractItemView::DragDrop);
    view.setDefaultDropAction(Qt::MoveAction);  // Ctrl copies
    view.setDragDropOverwriteMode(true);        // drop onto folders, not between items
    view.setDropIndicatorShown(true);
}

}  // namespace

ContentBrowser::ContentBrowser(editor::EditorContext& context, QWidget* parent)
    : QWidget(parent), context_(context), operations_(context, this) {
    adopt_timer_ = new QTimer(this);
    adopt_timer_->setSingleShot(true);
    adopt_timer_->setInterval(500);
    connect(adopt_timer_, &QTimer::timeout, this, [this] {
        operations_.adoptNewFiles();
        refreshBanner();
    });

    trees_ = new QStackedWidget;
    items_ = new AssetView<QListView>;
    items_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    items_->setUniformItemSizes(true);
    items_->setTextElideMode(Qt::ElideMiddle);
    items_->setEditTriggers(QAbstractItemView::EditKeyPressed);
    items_->setItemDelegate(new RenameDelegate(items_));
    items_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(items_, &QAbstractItemView::activated, this, &ContentBrowser::activate);
    connect(items_, &QWidget::customContextMenuRequested, this, &ContentBrowser::showItemMenu);

    addRoot(tr("Content"), QStringLiteral("game"), cleanPath(editor::toQString(context.project().ContentDir())), false);
    addRoot(tr("Engine"), QStringLiteral("engine"),
            cleanPath(editor::toQString(engine::AssetPathResolver::GetRoot() / "engine")), true);

    auto* splitter = new QSplitter;
    splitter->addWidget(trees_);
    splitter->addWidget(items_);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({200, 600});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    layout->addWidget(buildTopBar());
    layout->addWidget(buildBanner());
    layout->addWidget(splitter, 1);

    buildActions();
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ContentBrowser::updateActions);
    applyViewMode();
    navigate(0, roots_[0].path);

    // Rescans happen elsewhere too (saving a new scene), and the first scan only runs once
    // the engine starts, after the modules: poll the catalog's error.
    // TODO: maybe figure out an event driven way to do this
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &ContentBrowser::refreshBanner);
    timer->start(1000);
    refreshBanner();
}

ContentBrowser::~ContentBrowser() = default;

void ContentBrowser::addRoot(const QString& title, const QString& name, const QString& path, const bool read_only) {
    if (!read_only) {
        QDir().mkpath(path);  // a fresh project may not have content yet
    }
    Root root;
    root.title = title;
    root.name = name;
    root.path = path;
    root.read_only = read_only;
    root.model = new QFileSystemModel(this);
    // Renames and drops through the model would move a file without its .meta: the filter
    // models hand them to us instead.
    root.model->setReadOnly(true);
    root.model->setRootPath(path);
    root.folders = new AssetFilterModel(*root.model, AssetFilterModel::Mode::Folders, this);
    root.contents = new AssetFilterModel(*root.model, AssetFilterModel::Mode::Contents, this);

    root.tree = new AssetView<QTreeView>;
    root.tree->setModel(root.folders);
    root.tree->setRootIndex(root.folders->mapFromSource(root.model->index(path)));
    root.tree->setHeaderHidden(true);
    for (int column = 1; column < root.folders->columnCount(); ++column) {
        root.tree->hideColumn(column);
    }
    root.tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
    root.tree->setAutoExpandDelay(500);  // while dragging over a folder
    root.tree->setContextMenuPolicy(Qt::CustomContextMenu);
    setUpDragAndDrop(*root.tree);

    const std::size_t index = roots_.size();
    connect(root.tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this, index](const QModelIndex& current) {
                if (navigating_ || !current.isValid()) {
                    return;
                }
                const Root& shown = roots_[index];
                navigate(index, shown.model->filePath(shown.folders->mapToSource(current)));
            });
    connect(root.tree, &QWidget::customContextMenuRequested, this,
            [this, index](const QPoint& pos) { showFolderMenu(index, pos); });
    // The shown folder may be deleted or moved from outside.
    connect(root.model, &QAbstractItemModel::rowsRemoved, this, [this, index] {
        if (index == root_) {
            leaveMissingFolder();
        }
    });

    if (!read_only) {
        for (AssetFilterModel* model : {root.folders, root.contents}) {
            model->setWritable(true);
            // Queued: the operations show dialogs, which shouldn't run inside a view's drop or editor commit.
            connect(model, &AssetFilterModel::renameRequested, this, &ContentBrowser::renameItem, Qt::QueuedConnection);
            connect(model, &AssetFilterModel::dropRequested, this,
                    [this, index](const QStringList& paths, const QString& folder, const Qt::DropAction action) {
                        dropItems(index, paths, folder, action);
                    }, Qt::QueuedConnection);
        }
        connect(root.model, &QAbstractItemModel::rowsInserted, adopt_timer_, qOverload<>(&QTimer::start));
        connect(root.model, &QAbstractItemModel::rowsRemoved, adopt_timer_, qOverload<>(&QTimer::start));
    }
    trees_->addWidget(root.tree);
    roots_.push_back(root);
}

QWidget* ContentBrowser::buildTopBar() {
    auto* bar = new QWidget;
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    const auto add_separator = [layout] {
        auto* separator = new QFrame;
        separator->setFrameShape(QFrame::VLine);
        separator->setFrameShadow(QFrame::Sunken);
        layout->addWidget(separator);
    };

    // Their actions and menu come in buildActions().
    import_button_ = new QToolButton;
    import_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    import_button_->setAutoRaise(true);
    layout->addWidget(import_button_);
    new_button_ = new QToolButton;
    new_button_->setText(tr("New"));
    new_button_->setPopupMode(QToolButton::InstantPopup);
    new_button_->setAutoRaise(true);
    layout->addWidget(new_button_);
    add_separator();

    auto* group = new QButtonGroup(bar);
    for (std::size_t i = 0; i < roots_.size(); ++i) {
        auto* button = new QToolButton;
        button->setText(roots_[i].title);
        button->setCheckable(true);
        button->setAutoRaise(true);
        group->addButton(button);
        connect(button, &QToolButton::clicked, this, [this, i] { navigate(i, roots_[i].path); });
        layout->addWidget(button);
        roots_[i].button = button;
    }
    add_separator();

    breadcrumb_ = new QHBoxLayout;
    breadcrumb_->setSpacing(0);
    layout->addLayout(breadcrumb_);
    read_only_label_ = new QLabel(tr("(read-only)"));
    read_only_label_->setEnabled(false);
    layout->addWidget(read_only_label_);
    layout->addStretch();

    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter"));
    filter_->setToolTip(tr("Shows items whose name contains this; * and ? are wildcards"));
    filter_->setClearButtonEnabled(true);
    filter_->setMaximumWidth(220);
    connect(filter_, &QLineEdit::textChanged, this, &ContentBrowser::applyFilters);
    layout->addWidget(filter_);

    kind_ = new QComboBox;
    kind_->addItem(tr("All types"), -1);
    for (const engine::CatalogAssetKind kind : FilterKinds) {
        kind_->addItem(editor::assetIcon(kind), editor::assetKindName(kind), static_cast<int>(kind));
    }
    connect(kind_, &QComboBox::currentIndexChanged, this, &ContentBrowser::applyFilters);
    layout->addWidget(kind_);

    const QSettings settings;
    list_mode_ = new QToolButton;
    list_mode_->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
    list_mode_->setToolTip(tr("Show as a list"));
    list_mode_->setCheckable(true);
    list_mode_->setAutoRaise(true);
    list_mode_->setChecked(settings.value(ListModeKey, false).toBool());
    connect(list_mode_, &QToolButton::toggled, this, &ContentBrowser::applyViewMode);
    layout->addWidget(list_mode_);

    icon_size_ = new QSlider(Qt::Horizontal);
    icon_size_->setRange(32, 160);
    icon_size_->setValue(settings.value(IconSizeKey, 64).toInt());
    icon_size_->setFixedWidth(100);
    icon_size_->setToolTip(tr("Icon size"));
    connect(icon_size_, &QSlider::valueChanged, this, &ContentBrowser::applyViewMode);
    layout->addWidget(icon_size_);

    auto* rescan = new QToolButton;
    rescan->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    rescan->setToolTip(tr("Rescan asset files"));
    rescan->setAutoRaise(true);
    connect(rescan, &QToolButton::clicked, this, &ContentBrowser::rescan);
    layout->addWidget(rescan);
    return bar;
}

QWidget* ContentBrowser::buildBanner() {
    banner_ = new QFrame;
    banner_->setObjectName(QStringLiteral("CatalogBanner"));
    // Translucent amber reads as a warning on light and dark palettes alike.
    banner_->setStyleSheet(QStringLiteral("#CatalogBanner { background: rgba(230, 150, 20, 70); border-radius: 3px; }"));
    auto* layout = new QHBoxLayout(banner_);
    layout->setContentsMargins(6, 3, 3, 3);
    auto* icon = new QLabel;
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(16, 16));
    layout->addWidget(icon);
    banner_text_ = new QLabel;
    banner_text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(banner_text_, 1);
    auto* rescan = new QPushButton(tr("Rescan"));
    connect(rescan, &QPushButton::clicked, this, &ContentBrowser::rescan);
    layout->addWidget(rescan);
    banner_->hide();
    return banner_;
}

void ContentBrowser::buildActions() {
    // Shortcuts work while the item view has focus, so they don't clash with other panels'.
    const auto add = [this](const QString& text, const QKeySequence& shortcut, auto&& slot) {
        auto* action = new QAction(text, this);
        action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(action, &QAction::triggered, this, slot);
        items_->addAction(action);
        return action;
    };
    //TODO: extract shortcuts to another class for easier remapping??
    // (so user could remap on their own)
    open_ = add(tr("Open"), {}, [this] { activate(items_->currentIndex()); });
    add_to_scene_ = add(tr("Add to Scene"), {}, [this] { addSelectionToScene(); });
    add_to_scene_->setToolTip(tr("Places the selected meshes and models in front of the viewport camera"));
    new_folder_ = add(tr("New Folder"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N),
                      [this] { newFolder(root_, folder_); });
    rename_ = add(tr("Rename"), QKeySequence(Qt::Key_F2), [this] { items_->edit(items_->currentIndex()); });
    duplicate_ = add(tr("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D),
                     [this] { select(operations_.duplicate(selectedPaths())); });
    cut_ = add(tr("Cut"), QKeySequence::Cut, [this] { copyToClipboard(selectedPaths(), true); });
    copy_ = add(tr("Copy"), QKeySequence::Copy, [this] { copyToClipboard(selectedPaths(), false); });
    paste_ = add(tr("Paste"), QKeySequence::Paste, [this] { pasteInto(folder_); });
    delete_ = add(tr("Delete"), QKeySequence::Delete, [this] { deleteItems(selectedPaths()); });
    // Without a selection these apply to the shown folder.
    copy_path_ = add(tr("Copy Asset Path"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), [this] {
        const QStringList selected = selectedPaths();
        copyAssetPaths(selected.isEmpty() ? QStringList{folder_} : selected);
    });
    copy_uuid_ = add(tr("Copy UUID"), {}, [this] { copyUuids(selectedPaths()); });
    show_in_explorer_ = add(tr("Show in Explorer"), {}, [this] {
        const QStringList selected = selectedPaths();
        showInExplorer(selected.isEmpty() ? folder_ : selected.front());
    });
    open_->setShortcutVisibleInContextMenu(false);

    import_ = add(tr("Import..."), {}, [this] { importAssets(); });
    import_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    import_->setToolTip(tr("Copies images and models into the shown folder"));
    import_button_->setDefaultAction(import_);
    instance_from_texture_ = add(tr("Create Material Instance..."), {},
                                 [this] { newMaterialInstance(selectedPaths().value(0)); });

    new_menu_ = std::make_unique<QMenu>(tr("New"));
    new_menu_->addAction(style()->standardIcon(QStyle::SP_DirIcon), tr("Folder"), this,
                         [this] { newFolder(root_, folder_); });
    new_menu_->addSeparator();
    new_menu_->addAction(editor::assetIcon(engine::CatalogAssetKind::Material), tr("Material..."), this,
                         &ContentBrowser::newMaterial);
    new_menu_->addAction(editor::assetIcon(engine::CatalogAssetKind::MaterialInstance), tr("Material Instance..."), this,
                         [this] { newMaterialInstance(); });
    new_menu_->addAction(editor::assetIcon(engine::CatalogAssetKind::Scene), tr("Scene..."), this,
                         &ContentBrowser::newScene);
    new_menu_->addAction(editor::assetIcon(engine::CatalogAssetKind::Script), tr("Script..."), this,
                         &ContentBrowser::newScript);
    new_button_->setMenu(new_menu_.get());
}

void ContentBrowser::navigate(const std::size_t root, const QString& folder) {
    // Showing another root's tree moves focus into it, which gives it a current item: that
    // mustn't navigate again from in here.
    navigating_ = true;
    Root& shown = roots_[root];
    const QModelIndex source = shown.model->index(folder);
    root_ = root;
    folder_ = folder;
    shown.contents->setFolder(source);
    if (items_->model() != shown.contents) {
        // setModel() leaves the old selection model to us.
        QItemSelectionModel* old_selection = items_->selectionModel();
        items_->setModel(shown.contents);
        delete old_selection;
        connect(items_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ContentBrowser::updateActions);
        trees_->setCurrentWidget(shown.tree);
        shown.button->setChecked(true);
        read_only_label_->setVisible(shown.read_only);
    }
    items_->setRootIndex(shown.contents->mapFromSource(source));

    // The tree's root folder has no item of its own.
    const QModelIndex in_tree = folder == shown.path ? QModelIndex() : shown.folders->mapFromSource(source);
    shown.tree->setCurrentIndex(in_tree);
    if (in_tree.isValid()) {
        shown.tree->scrollTo(in_tree);  // also expands its parents
    }
    navigating_ = false;
    rebuildBreadcrumb();
    updateActions();
}

void ContentBrowser::leaveMissingFolder() {
    const Root& shown = roots_[root_];
    QString folder = folder_;
    while (folder != shown.path && isWithin(folder, shown.path) && !QFileInfo(folder).isDir()) {
        folder = QFileInfo(folder).absolutePath();
    }
    if (folder != folder_) {
        navigate(root_, folder);
    }
}

void ContentBrowser::activate(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    const Root& shown = roots_[root_];
    const QModelIndex source = shown.contents->mapToSource(index);
    const QString path = shown.model->filePath(source);
    if (shown.model->isDir(source)) {
        navigate(root_, path);
    } else if (engine::CatalogKindForFile(editor::toPath(path)) == engine::CatalogAssetKind::Scene) {
        context_.sceneDocument().openInteractive(window(), editor::toPath(path));
    } else if (const auto kind = engine::CatalogKindForFile(editor::toPath(path));
               kind == engine::CatalogAssetKind::Material || kind == engine::CatalogAssetKind::MaterialInstance) {
        editMaterial(path, *kind);
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void ContentBrowser::editMaterial(const QString& path, const engine::CatalogAssetKind kind) {
    if (roots_[root_].read_only) {
        QMessageBox::information(window(), tr("Read-only asset"), tr("Engine assets cannot be edited here."));
        return;
    }
    std::string text, error;
    if (!engine::ReadTextFile(editor::toPath(path), text, error, 1024 * 1024)) {
        QMessageBox::warning(window(), tr("Edit Material"), QString::fromStdString(error));
        return;
    }
    const QString name = splitAssetName(QFileInfo(path).fileName(), false).first;
    const QString folder = QFileInfo(path).absolutePath();
    if (kind == engine::CatalogAssetKind::Material) {
        engine::MaterialDefinition definition;
        if (!engine::ParseMaterialDefinition(text, definition, error)) {
            QMessageBox::warning(window(), tr("Edit Material"), QString::fromStdString(error));
            return;
        }
        MaterialDialog dialog(catalog(), folder, name, window(), true);
        dialog.load(definition);
        if (dialog.exec() == QDialog::Accepted) operations_.updateMaterial(path, dialog.definition());
    } else {
        engine::MaterialInstanceDefinition definition;
        if (!engine::ParseMaterialInstanceDefinition(text, definition, error)) {
            QMessageBox::warning(window(), tr("Edit Material Instance"), QString::fromStdString(error));
            return;
        }
        MaterialInstanceDialog dialog(catalog(), folder, name, window(), true);
        dialog.load(definition);
        if (dialog.exec() == QDialog::Accepted) operations_.updateMaterialInstance(path, dialog.definition());
    }
}

void ContentBrowser::rebuildBreadcrumb() {
    // deleteLater(): this may run from one of the buttons' own clicked().
    while (QLayoutItem* item = breadcrumb_->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    const Root& shown = roots_[root_];
    const auto add_crumb = [this](const QString& text, const QString& path, const bool current) {
        auto* button = new QToolButton;
        button->setText(text);
        button->setAutoRaise(true);
        if (current) {
            QFont font = button->font();
            font.setBold(true);
            button->setFont(font);
        }
        const std::size_t root = root_;
        connect(button, &QToolButton::clicked, this, [this, root, path] { navigate(root, path); });
        breadcrumb_->addWidget(button);
    };

    const QString relative = QDir(shown.path).relativeFilePath(folder_);
    const QStringList parts = relative == QLatin1String(".") ? QStringList() : relative.split(QLatin1Char('/'));
    add_crumb(shown.name, shown.path, parts.isEmpty());
    QString path = shown.path;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        path += QLatin1Char('/') + parts[i];
        breadcrumb_->addWidget(new QLabel(QStringLiteral("›")));
        add_crumb(parts[i], path, i + 1 == parts.size());
    }
}

void ContentBrowser::applyViewMode() {
    QSettings settings;
    settings.setValue(ListModeKey, list_mode_->isChecked());
    settings.setValue(IconSizeKey, icon_size_->value());

    if (list_mode_->isChecked()) {
        items_->setViewMode(QListView::ListMode);
        items_->setIconSize({20, 20});
        items_->setGridSize({});
        items_->setWordWrap(false);
    } else {
        const int size = icon_size_->value();
        items_->setViewMode(QListView::IconMode);
        items_->setIconSize({size, size});
        // Room for two lines of name under the icon.
        items_->setGridSize({size + 40, size + 2 * fontMetrics().height() + 12});
        items_->setWordWrap(true);
    }
    icon_size_->setEnabled(!list_mode_->isChecked());
    // setViewMode() resets these, and Static movement turns dragging off.
    items_->setMovement(QListView::Static);
    items_->setResizeMode(QListView::Adjust);
    setUpDragAndDrop(*items_);
}

void ContentBrowser::applyFilters() {
    const int kind = kind_->currentData().toInt();
    for (const Root& root : roots_) {
        root.contents->setNameFilter(filter_->text());
        root.contents->setKindFilter(kind < 0 ? std::nullopt
                                              : std::optional(static_cast<engine::CatalogAssetKind>(kind)));
    }
}

void ContentBrowser::rescan() {
    operations_.rescan();
    refreshBanner();
}

void ContentBrowser::refreshBanner() {
    const std::string& error = context_.engine().GetSceneAssets().GetCatalogError();
    banner_->setVisible(!error.empty());
    if (error.empty()) {
        return;
    }
    const QString text = QString::fromUtf8(error.data(), static_cast<qsizetype>(error.size())).trimmed();
    // First line in the banner, all of it in the tooltip.
    banner_text_->setText(tr("Asset catalog: %1").arg(text.section(QLatin1Char('\n'), 0, 0)));
    banner_text_->setToolTip(text);
}

QStringList ContentBrowser::selectedPaths() const {
    QStringList paths;
    if (items_->selectionModel() == nullptr) {
        return paths;
    }
    const Root& shown = roots_[root_];
    for (const QModelIndex& index : items_->selectionModel()->selectedIndexes()) {
        if (index.column() == 0) {
            paths << shown.model->filePath(shown.contents->mapToSource(index));
        }
    }
    return paths;
}

bool ContentBrowser::select(const QStringList& paths) {
    const Root& shown = roots_[root_];
    QItemSelection selection;
    QModelIndex first;
    for (const QString& path : paths) {
        // index() lists a new file right away; the model's file watcher would take a moment.
        const QModelIndex index = shown.contents->mapFromSource(shown.model->index(path));
        if (index.isValid() && index.parent() == items_->rootIndex()) {
            selection.select(index, index);
            if (!first.isValid()) {
                first = index;
            }
        }
    }
    if (!first.isValid()) {
        return false;
    }
    items_->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    items_->selectionModel()->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
    items_->scrollTo(first);
    items_->setFocus();
    return true;
}

void ContentBrowser::updateActions() {
    if (open_ == nullptr) {
        return;  // before buildActions()
    }
    const bool writable = !roots_[root_].read_only;
    const QStringList selected = selectedPaths();
    const bool any = !selected.isEmpty();
    const bool assets = std::ranges::any_of(selected, [](const QString& path) {
        return engine::CatalogKindForFile(editor::toPath(path)).has_value();
    });
    const QMimeData* clipboard = QGuiApplication::clipboard()->mimeData();

    open_->setEnabled(selected.size() == 1);
    if (selected.size() == 1) {
        const auto kind = engine::CatalogKindForFile(editor::toPath(selected.front()));
        open_->setText(kind == engine::CatalogAssetKind::Material || kind == engine::CatalogAssetKind::MaterialInstance
                           ? tr("Edit...") : tr("Open"));
    } else {
        open_->setText(tr("Open"));
    }
    // Engine assets too: its primitives are worth placing.
    add_to_scene_->setEnabled(std::ranges::any_of(
        selected, [this](const QString& path) { return placeableMesh(catalog(), path) != nullptr; }));
    new_folder_->setEnabled(writable);
    rename_->setEnabled(writable && selected.size() == 1);
    duplicate_->setEnabled(writable && any);
    cut_->setEnabled(writable && any);
    copy_->setEnabled(any);
    paste_->setEnabled(writable && clipboard != nullptr && !AssetFilterModel::droppedPaths(clipboard).isEmpty());
    delete_->setEnabled(writable && any);
    copy_uuid_->setEnabled(assets);
    import_->setEnabled(writable);
    new_menu_->setEnabled(writable);
    new_button_->setEnabled(writable);
    instance_from_texture_->setEnabled(
        writable && selected.size() == 1 &&
        engine::CatalogKindForFile(editor::toPath(selected.front())) == engine::CatalogAssetKind::Texture);
}

void ContentBrowser::showItemMenu(const QPoint& pos) {
    const QModelIndex index = items_->indexAt(pos);
    if (!index.isValid()) {
        items_->clearSelection();
    } else if (!items_->selectionModel()->isSelected(index)) {
        items_->setCurrentIndex(index);
    }
    updateActions();

    QMenu menu(window());
    if (index.isValid()) {
        menu.addAction(open_);
        if (add_to_scene_->isEnabled()) {
            menu.addAction(add_to_scene_);
        }
        if (instance_from_texture_->isEnabled()) {
            menu.addAction(instance_from_texture_);
        }
        menu.addSeparator();
        menu.addActions({cut_, copy_, paste_, duplicate_});
        menu.addSeparator();
        menu.addActions({rename_, delete_});
        menu.addSeparator();
        menu.addActions({copy_path_, copy_uuid_, show_in_explorer_});
    } else {
        menu.addMenu(new_menu_.get());
        menu.addAction(import_);
        menu.addSeparator();
        menu.addAction(paste_);
        menu.addSeparator();
        menu.addActions({copy_path_, show_in_explorer_});
    }
    menu.exec(items_->viewport()->mapToGlobal(pos));
}

void ContentBrowser::showFolderMenu(const std::size_t root, const QPoint& pos) {
    const Root& shown = roots_[root];
    const QModelIndex index = shown.tree->indexAt(pos);
    const QString folder = index.isValid() ? shown.model->filePath(shown.folders->mapToSource(index)) : shown.path;
    const bool writable = !shown.read_only;
    const bool top = folder == shown.path;  // the root folder itself can't be renamed or deleted
    const QMimeData* clipboard = QGuiApplication::clipboard()->mimeData();

    QMenu menu(window());  // see showItemMenu()
    menu.addAction(tr("New Folder"), this, [this, root, folder] { newFolder(root, folder); })->setEnabled(writable);
    menu.addAction(tr("Paste"), this, [this, folder] { pasteInto(folder); })
        ->setEnabled(writable && clipboard != nullptr && !AssetFilterModel::droppedPaths(clipboard).isEmpty());
    menu.addSeparator();
    menu.addAction(tr("Rename"), this, [&shown, index] { shown.tree->edit(index); })->setEnabled(writable && !top);
    menu.addAction(tr("Delete"), this, [this, folder] { deleteItems({folder}); })->setEnabled(writable && !top);
    menu.addSeparator();
    menu.addAction(tr("Copy Asset Path"), this, [this, folder] { copyAssetPaths({folder}); });
    menu.addAction(tr("Show in Explorer"), this, [this, folder] { showInExplorer(folder); });
    menu.exec(shown.tree->viewport()->mapToGlobal(pos));
}

void ContentBrowser::renameItem(const QString& path, const QString& name) {
    const QString renamed = operations_.rename(path, name);
    if (renamed.isEmpty()) {
        return;
    }
    if (isWithin(folder_, path)) {
        navigate(root_, renamed + folder_.mid(path.size()));  // renamed the shown folder or one above it
    } else {
        select({renamed});
    }
}

void ContentBrowser::dropItems(const std::size_t root, const QStringList& paths, const QString& folder,
                               const Qt::DropAction action) {
    // Files from outside the content are copied in, never taken from where they were.
    QStringList moves;
    QStringList copies;
    for (const QString& path : paths) {
        (action == Qt::MoveAction && isWithin(path, roots_[root].path) ? moves : copies) << path;
    }
    QStringList added = operations_.transfer(moves, folder, false);
    added << copyIn(copies, folder);
    leaveMissingFolder();
    select(added);
}

void ContentBrowser::newFolder(const std::size_t root, const QString& parent) {
    if (root != root_ || parent != folder_) {
        navigate(root, parent);
    }
    const QString path = AssetOperations::uniquePath(parent, tr("New Folder"), true);
    if (!QDir().mkdir(path)) {
        QMessageBox::warning(this, tr("New Folder"), tr("Could not create %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (select({path})) {  // not if the filter hides it
        items_->edit(items_->currentIndex());
    }
}

void ContentBrowser::deleteItems(const QStringList& paths) {
    operations_.remove(paths);
    leaveMissingFolder();
}

void ContentBrowser::copyToClipboard(const QStringList& paths, const bool cut) {
    if (paths.isEmpty()) {
        return;
    }
    // thank god copying files is not that hard
    // good luck pasting file://C:/Users/Asdfg/... into your messages folks
    QList<QUrl> urls;
    for (const QString& path : paths) {
        urls << QUrl::fromLocalFile(path);
    }
    auto* mime = new QMimeData;
    mime->setUrls(urls);
    QGuiApplication::clipboard()->setMimeData(mime);
    cut_paths_ = cut ? paths : QStringList();
}

void ContentBrowser::pasteInto(const QString& folder) {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime == nullptr) {
        return;
    }
    const QStringList paths = AssetFilterModel::droppedPaths(mime);
    // Anything else on the clipboard, such as files copied in Explorer, is copied in.
    const bool cut = !cut_paths_.isEmpty() && paths == cut_paths_;
    const QStringList added = cut ? operations_.transfer(paths, folder, false) : copyIn(paths, folder);
    if (cut) {
        cut_paths_.clear();
        QGuiApplication::clipboard()->clear();  // they're gone from where the URLs point
    }
    leaveMissingFolder();
    select(added);
}

void ContentBrowser::copyAssetPaths(const QStringList& paths) {
    QStringList names;
    for (const QString& path : paths) {
        names << assetName(path);
    }
    QGuiApplication::clipboard()->setText(names.join(QLatin1Char('\n')));
}

void ContentBrowser::copyUuids(const QStringList& paths) {
    QStringList ids;
    for (const QString& path : paths) {
        engine::AssetMetadata metadata;
        if (std::string error; engine::ReadAssetMetadata(editor::toPath(path), metadata, error)) {
            ids << QString::fromStdString(metadata.id.ToString());
        }
    }
    if (!ids.isEmpty()) {
        QGuiApplication::clipboard()->setText(ids.join(QLatin1Char('\n')));
    }
}

void ContentBrowser::showInExplorer(const QString& path) {
    if (path == folder_) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return;
    }
    //TODO: will we have other systems?... maybe there is a better way to do this?
    // explorer.exe wants "/select," and the path as separate arguments.
    QProcess::startDetached(QStringLiteral("explorer.exe"),
                            {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
}

void ContentBrowser::addSelectionToScene() {
    addToScene(context_, selectedPaths());
}

const engine::AssetCatalog& ContentBrowser::catalog() const {
    return context_.engine().GetSceneAssets().GetCatalog();
}

QStringList ContentBrowser::copyIn(const QStringList& paths, const QString& folder) {
    const auto external_model = [this](const QString& path) {
        const bool in_project =
            std::ranges::any_of(roots_, [&path](const Root& root) { return isWithin(path, root.path); });
        return !in_project && QFileInfo(path).isFile() &&
               engine::AssetPathResolver::IsModelFile(editor::toPath(path));
    };
    QStringList copies;
    QStringList models;
    for (const QString& path : paths) {
        (external_model(path) ? models : copies) << path;
    }
    QStringList added = operations_.transfer(copies, folder, true);
    for (const QString& model : models) {
        if (const QString imported = importModel(model, folder); !imported.isEmpty()) {
            added << imported;
        }
    }
    return added;
}

QString ContentBrowser::importModel(const QString& source, const QString& folder) {
    ImportModelDialog dialog(catalog(), source, folder, window());
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    return operations_.importModel(dialog.result(), folder);
}

void ContentBrowser::importAssets() {
    QSettings settings;
    const QStringList files = QFileDialog::getOpenFileNames(
        window(), tr("Import"), settings.value(ImportDirectoryKey).toString(), importFilter());
    if (files.isEmpty()) {
        return;
    }
    settings.setValue(ImportDirectoryKey, QFileInfo(files.front()).absolutePath());
    QStringList sources;
    for (const QString& file : files) {
        sources << cleanPath(file);
    }
    select(copyIn(sources, folder_));
}

void ContentBrowser::newMaterial() {
    MaterialDialog dialog(catalog(), folder_, tr("New Material"), window());
    if (dialog.exec() == QDialog::Accepted && operations_.createMaterial(dialog.path(), dialog.definition())) {
        select({dialog.path()});
    }
}

void ContentBrowser::newMaterialInstance(const QString& albedo) {
    QString name = tr("New Material Instance");
    if (!albedo.isEmpty()) {
        // Named after the texture, or after its model folder for a model's texture.png.
        const QFileInfo texture(albedo);
        name = splitAssetName(texture.fileName(), false).first;
        if (name.compare(QLatin1String("texture"), Qt::CaseInsensitive) == 0) {
            name = texture.dir().dirName();
        }
    }
    MaterialInstanceDialog dialog(catalog(), folder_, name, window());
    if (!albedo.isEmpty()) {
        dialog.setAlbedo(assetName(albedo));
    }
    if (dialog.exec() == QDialog::Accepted &&
        operations_.createMaterialInstance(dialog.path(), dialog.definition())) {
        select({dialog.path()});
    }
}

void ContentBrowser::newScene() {
    NewAssetDialog dialog(tr("New Scene"), folder_, QStringLiteral(".scene.json"), tr("New Scene"), window());
    if (dialog.exec() == QDialog::Accepted &&
        context_.sceneDocument().createInteractive(window(), editor::toPath(dialog.path()))) {
        select({dialog.path()});
    }
}

void ContentBrowser::newScript() {
    // No spaces by default: require() names map onto file names.
    NewAssetDialog dialog(tr("New Script"), folder_, QStringLiteral(".lua"), QStringLiteral("NewScript"), window());
    if (dialog.exec() == QDialog::Accepted && operations_.createScript(dialog.path())) {
        select({dialog.path()});
    }
}

}  // namespace content
