#include "content_browser.hpp"

#include <QButtonGroup>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileSystemModel>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <optional>
#include <string>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/scene_document.hpp>

#include "asset_filter_model.hpp"
#include "asset_icons.hpp"

namespace content {

namespace {

const QString IconSizeKey = QStringLiteral("content/iconSize");
const QString ListModeKey = QStringLiteral("content/listMode");

constexpr engine::CatalogAssetKind FilterKinds[] = {
    engine::CatalogAssetKind::Scene,    engine::CatalogAssetKind::Mesh,
    engine::CatalogAssetKind::Texture,  engine::CatalogAssetKind::Material,
    engine::CatalogAssetKind::MaterialInstance, engine::CatalogAssetKind::Shader,
};

QString cleanPath(const std::filesystem::path& path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(editor::toQString(path)));
}

}  // namespace

ContentBrowser::ContentBrowser(editor::EditorContext& context, QWidget* parent) : QWidget(parent), context_(context) {
    trees_ = new QStackedWidget;
    items_ = new QListView;
    items_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    items_->setUniformItemSizes(true);
    items_->setTextElideMode(Qt::ElideMiddle);
    connect(items_, &QAbstractItemView::activated, this, &ContentBrowser::activate);

    addRoot(tr("Content"), QStringLiteral("game"), cleanPath(context.project().ContentDir()), false);
    addRoot(tr("Engine"), QStringLiteral("engine"), cleanPath(engine::AssetPathResolver::GetRoot() / "engine"), true);

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
    // Renames and drops through the model would move a file without its .meta.
    root.model->setReadOnly(true);
    root.model->setRootPath(path);
    root.folders = new AssetFilterModel(*root.model, AssetFilterModel::Mode::Folders, this);
    root.contents = new AssetFilterModel(*root.model, AssetFilterModel::Mode::Contents, this);

    root.tree = new QTreeView;
    root.tree->setModel(root.folders);
    root.tree->setRootIndex(root.folders->mapFromSource(root.model->index(path)));
    root.tree->setHeaderHidden(true);
    for (int column = 1; column < root.folders->columnCount(); ++column) {
        root.tree->hideColumn(column);
    }
    const std::size_t index = roots_.size();
    connect(root.tree->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this, index](const QModelIndex& current) {
                if (navigating_ || !current.isValid()) {
                    return;
                }
                const Root& shown = roots_[index];
                navigate(index, shown.model->filePath(shown.folders->mapToSource(current)));
            });
    trees_->addWidget(root.tree);
    roots_.push_back(root);
}

QWidget* ContentBrowser::buildTopBar() {
    auto* bar = new QWidget;
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);

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
    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

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
        kind_->addItem(assetIcon(kind), assetKindName(kind), static_cast<int>(kind));
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

void ContentBrowser::navigate(const std::size_t root, const QString& folder) {
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
        trees_->setCurrentWidget(shown.tree);
        shown.button->setChecked(true);
        read_only_label_->setVisible(shown.read_only);
    }
    items_->setRootIndex(shown.contents->mapFromSource(source));

    // The tree's root folder has no item of its own.
    navigating_ = true;
    const QModelIndex in_tree = folder == shown.path ? QModelIndex() : shown.folders->mapFromSource(source);
    shown.tree->setCurrentIndex(in_tree);
    if (in_tree.isValid()) {
        shown.tree->scrollTo(in_tree);  // also expands its parents
    }
    navigating_ = false;
    rebuildBreadcrumb();
}

void ContentBrowser::activate(const QModelIndex& index) {
    const Root& shown = roots_[root_];
    const QModelIndex source = shown.contents->mapToSource(index);
    const QString path = shown.model->filePath(source);
    if (shown.model->isDir(source)) {
        navigate(root_, path);
    } else if (engine::CatalogKindForFile(editor::toPath(path)) == engine::CatalogAssetKind::Scene) {
        context_.sceneDocument().openInteractive(window(), editor::toPath(path));
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
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
    // setViewMode() resets these.
    items_->setMovement(QListView::Static);
    items_->setResizeMode(QListView::Adjust);
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
    std::string error;  // the banner shows it, from the catalog
    (void)context_.engine().RescanAssetCatalogs(error);
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

}  // namespace content
