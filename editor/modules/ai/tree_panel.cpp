#include "tree_panel.hpp"

#include <QAction>
#include <QDesktopServices>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <engine/ai/BehaviorTreeSource.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>

#include <editor/editor_context.hpp>
#include <editor/paths.hpp>

namespace behavior {

namespace {

constexpr auto ProblemStyle = "color: rgb(211, 47, 47);";

/// Viewing only for now: the editing commands QtNodes binds do nothing.
class TreeView final : public QtNodes::GraphicsView {
public:
    using GraphicsView::GraphicsView;
    void onDeleteSelectedObjects() override {}
    void onDuplicateSelectedObjects() override {}
    void onCopySelectedObjects() override {}
    void onPasteObjects() override {}
};

QString describe(const engine::StateValue& value) {
    if (const auto* text = std::get_if<std::string>(&value.value)) {
        return QStringLiteral("\"%1\"").arg(QString::fromStdString(*text));
    }
    if (const auto* boolean = std::get_if<bool>(&value.value)) {
        return *boolean ? QStringLiteral("true") : QStringLiteral("false");
    }
    return QString::number(value.Number());
}

QLabel* valueLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

}  // namespace

TreePanel::TreePanel(editor::EditorContext& context, QWidget* parent)
    : QWidget(parent), context_(context), scene_(std::make_unique<QtNodes::BasicGraphicsScene>(graph_)) {
    scene_->setOrientation(Qt::Vertical);

    title_ = new QLabel(tr("Double-click a behavior tree in Content to show it here."));
    auto* tool_bar = new QToolBar;
    reload_ = tool_bar->addAction(tr("Reload"), this, &TreePanel::reload);
    arrange_ = tool_bar->addAction(tr("Arrange"), this, [this] { graph_.arrange(scene_->nodeGeometry()); });
    fit_ = tool_bar->addAction(tr("Fit"), this, &TreePanel::fit);
    open_source_ = tool_bar->addAction(tr("Open Lua Source"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(editor::toQString(source_)));
    });
    for (QAction* action : {reload_, arrange_, fit_, open_source_}) {
        action->setEnabled(false);
    }
    auto* header = new QHBoxLayout;
    header->addWidget(title_, 1);
    header->addWidget(tool_bar);

    problems_ = new QLabel;
    problems_->setWordWrap(true);
    problems_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    problems_->setStyleSheet(QString::fromLatin1(ProblemStyle));
    problems_->hide();

    view_ = new TreeView(scene_.get());
    // QtNodes binds Escape and Undo window-wide, where they'd clash with the editor's own.
    for (QAction* action : view_->actions()) {
        action->setShortcutContext(Qt::WidgetShortcut);
    }

    auto* details = new QWidget;
    details_ = new QFormLayout(details);
    auto* scroll = new QScrollArea;
    scroll->setWidget(details);
    scroll->setWidgetResizable(true);

    auto* splitter = new QSplitter;
    splitter->addWidget(view_);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(header);
    layout->addWidget(problems_);
    layout->addWidget(splitter, 1);

    watcher_ = new QFileSystemWatcher(this);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, &TreePanel::reload);
    connect(scene_.get(), &QGraphicsScene::selectionChanged, this, &TreePanel::showDetails);
    connect(scene_.get(), &QtNodes::BasicGraphicsScene::nodeDoubleClicked, this, &TreePanel::openScript);
    showDetails();
}

TreePanel::~TreePanel() {
    // The scene goes before the panel's widgets; tearing it down must not update them.
    disconnect(scene_.get(), nullptr, this, nullptr);
    scene_.reset();
}

void TreePanel::open(const std::filesystem::path& source) {
    if (!watcher_->files().isEmpty()) {
        watcher_->removePaths(watcher_->files());
    }
    source_ = source;
    title_->setText(editor::toQString(source.filename()));
    title_->setToolTip(editor::toQString(source));
    for (QAction* action : {reload_, arrange_, fit_, open_source_}) {
        action->setEnabled(true);
    }
    reload();
}

void TreePanel::reload() {
    if (source_.empty()) {
        return;
    }
    // Editors that save by replacing the file drop it from the watcher.
    if (const QString path = editor::toQString(source_); !watcher_->files().contains(path) && QFile::exists(path)) {
        watcher_->addPath(path);
    }
    const engine::AssetCatalog& catalog = context_.engine().GetAssets().GetCatalog();
    QStringList problems;
    std::string text, error;
    engine::ai::TreeSource tree;
    if (engine::ReadTextFile(source_, text, error) && engine::ai::ReadTreeSource(text, tree, error)) {
        graph_.load(tree, catalog, problems);
    } else {
        QStringList ignored;
        graph_.load({}, catalog, ignored);
        problems << tr("Can't show this tree: %1").arg(QString::fromStdString(error));
    }
    graph_.arrange(scene_->nodeGeometry());

    const auto ids = graph_.allNodeIds();
    if (const auto broken = std::ranges::count_if(ids, [this](const auto id) { return !graph_.node(id)->problems.isEmpty(); });
        broken > 0) {
        problems << tr("%n node(s) can't run: hover one, or select it, to see why.", nullptr, static_cast<int>(broken));
    }
    problems_->setText(problems.join(QLatin1Char('\n')));
    problems_->setVisible(!problems.isEmpty());
    fit();
    showDetails();
}

void TreePanel::fit() {
    // Fit large trees, but don't blow small ones up.
    view_->zoomFitAll();
    if (view_->transform().m11() > 1.0) {
        view_->setupScale(1.0);
    }
    view_->centerOn(scene_->itemsBoundingRect().center());
}

void TreePanel::showDetails() {
    while (details_->rowCount() > 0) {
        details_->removeRow(0);
    }
    const QList<QGraphicsItem*> selected = scene_->selectedItems();
    auto* item = selected.size() == 1 ? qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(selected.front()) : nullptr;
    const TreeGraph::Node* node = item != nullptr ? graph_.node(item->nodeId()) : nullptr;
    if (node == nullptr) {
        details_->addRow(valueLabel(tr("Select a node to see its details.")));
        return;
    }
    const engine::ai::NodeSource& source = node->source;
    details_->addRow(tr("ID"), valueLabel(QString::fromStdString(source.id)));
    details_->addRow(tr("Kind"), valueLabel(QString::fromLatin1(engine::ai::NodeKindName(source.kind))));
    if (source.kind == engine::ai::NodeKind::Condition || source.kind == engine::ai::NodeKind::Action) {
        details_->addRow(tr("Script"), valueLabel(node->script.isEmpty() ? QString::fromStdString(source.script)
                                                                          : node->script));
        if (!node->script.isEmpty()) {
            auto* open = new QPushButton(tr("Open Script"));
            connect(open, &QPushButton::clicked, this, [this, id = item->nodeId()] { openScript(id); });
            details_->addRow({}, open);
        }
    }
    if (!source.parameters.empty()) {
        details_->addRow(new QLabel(tr("Parameters:")));
        for (const auto& [name, value] : source.parameters) {
            details_->addRow(QString::fromStdString(name), valueLabel(describe(value)));
        }
    }
    if (!node->problems.isEmpty()) {
        QLabel* label = valueLabel(node->problems.join(QLatin1Char('\n')));
        label->setStyleSheet(QString::fromLatin1(ProblemStyle));
        details_->addRow(tr("Problems"), label);
    }
}

void TreePanel::openScript(const TreeGraph::NodeId id) {
    const TreeGraph::Node* node = graph_.node(id);
    if (node == nullptr || node->script.isEmpty()) {
        return;
    }
    const engine::AssetCatalog& catalog = context_.engine().GetAssets().GetCatalog();
    if (const engine::CatalogEntry* entry = catalog.FindPath(node->script.toStdString())) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(editor::toQString(entry->source)));
    }
}

}  // namespace behavior
