#include "tree_panel.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <utility>

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/UndoCommands.hpp>

#include <engine/ai/BehaviorTreeSource.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/core/FileSystem.hpp>

#include <editor/asset_picker.hpp>
#include <editor/paths.hpp>

#include "parameter_editor.hpp"

namespace behavior {

namespace {

using engine::ai::NodeKind;
using NodeId = TreeGraph::NodeId;
using QtNodes::InvalidNodeId;

constexpr auto ProblemStyle = "color: rgb(211, 47, 47);";
constexpr double Gap = 30.0;     // between siblings
constexpr double RowGap = 60.0;  // between a parent and its children

QString kindName(const NodeKind kind) {
    return QString::fromLatin1(engine::ai::NodeKindName(kind));
}

QLabel* valueLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

/// What QtNodes would paste: its own format, or else JSON text.
QJsonObject clipboardGraph() {
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    const QString format = QStringLiteral("application/qt-nodes-graph");
    if (mime == nullptr) {
        return {};
    }
    if (mime->hasFormat(format)) {
        return QJsonDocument::fromJson(mime->data(format)).object();
    }
    return mime->hasText() ? QJsonDocument::fromJson(mime->text().toUtf8()).object() : QJsonObject();
}

/// Holds back QtNodes' editing commands where they'd break the graph.
class TreeView final : public QtNodes::GraphicsView {
public:
    TreeView(QtNodes::BasicGraphicsScene* scene, const TreeGraph& graph) : GraphicsView(scene), graph_(graph) {}

    void onDeleteSelectedObjects() override {
        if (graph_.editable()) {
            GraphicsView::onDeleteSelectedObjects();
        }
    }
    void onDuplicateSelectedObjects() override {
        // With no node to copy, it would paste whatever the clipboard holds.
        if (graph_.editable() && selectInnerLinks()) {
            GraphicsView::onDuplicateSelectedObjects();
        }
    }
    void onCopySelectedObjects() override {
        selectInnerLinks();
        GraphicsView::onCopySelectedObjects();
    }
    void onPasteObjects() override {
        if (graph_.editable() && graph_.pastable(clipboardGraph())) {
            GraphicsView::onPasteObjects();
        }
    }

private:
    /// QtNodes copies only selected links: these keep a copied branch whole. False if no node is selected.
    bool selectInnerLinks() {
        std::set<TreeGraph::NodeId> nodes;
        for (QGraphicsItem* item : scene()->selectedItems()) {
            if (const auto* node = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
                nodes.insert(node->nodeId());
            }
        }
        for (const TreeGraph::NodeId id : nodes) {
            for (const QtNodes::ConnectionId link : graph_.connections(id, QtNodes::PortType::Out, 0)) {
                if (nodes.contains(link.inNodeId)) {
                    nodeScene()->connectionGraphicsObject(link)->setSelected(true);
                }
            }
        }
        return !nodes.empty();
    }

    const TreeGraph& graph_;
};

/// The graph and the selected node, before or after one of the panel's edits.
struct State {
    TreeGraph::Snapshot graph;
    NodeId selected = InvalidNodeId;
};

/// One of the panel's edits, undone and redone by bringing back the whole graph.
class SnapshotCommand final : public QUndoCommand {
public:
    using Restore = std::function<void(const State&)>;

    SnapshotCommand(const QString& text, State before, State after, Restore restore)
        : QUndoCommand(text), before_(std::move(before)), after_(std::move(after)), restore_(std::move(restore)) {}

    void undo() override {
        restore_(before_);
    }
    void redo() override {
        if (!std::exchange(first_, false)) {  // the first time, the edit has just happened
            restore_(after_);
        }
    }

private:
    State before_;
    State after_;
    Restore restore_;
    bool first_ = true;
};

/// Whether `text` is just what the editor would write for it, so saving over it loses nothing.
bool writtenByEditor(std::string text) {
    std::erase(text, '\r');
    engine::ai::TreeSource tree;
    std::string error;
    return engine::ai::ReadTreeSource(text, tree, error) && engine::ai::WriteTreeSource(tree) == text;
}

}  // namespace

TreePanel::TreePanel(const engine::AssetCatalog& catalog, QWidget* parent)
    : QWidget(parent), catalog_(catalog), scene_(std::make_unique<QtNodes::BasicGraphicsScene>(graph_)) {
    scene_->setOrientation(Qt::Vertical);
    QUndoStack& history = scene_->undoStack();

    title_ = new QLabel(tr("Double-click a behavior tree in Content to open it here."));
    auto* tool_bar = new QToolBar;
    save_ = tool_bar->addAction(tr("Save"), this, &TreePanel::save);
    save_->setToolTip(
        tr("Save the tree (%1)").arg(QKeySequence(QKeySequence::Save).toString(QKeySequence::NativeText)));
    undo_ = tool_bar->addAction(tr("Undo"), &history, &QUndoStack::undo);
    redo_ = tool_bar->addAction(tr("Redo"), &history, &QUndoStack::redo);
    connect(&history, &QUndoStack::canUndoChanged, undo_, &QAction::setEnabled);
    connect(&history, &QUndoStack::canRedoChanged, redo_, &QAction::setEnabled);
    connect(&history, &QUndoStack::undoTextChanged, undo_,
            [this](const QString& text) { undo_->setToolTip(tr("Undo %1").arg(text)); });
    connect(&history, &QUndoStack::redoTextChanged, redo_,
            [this](const QString& text) { redo_->setToolTip(tr("Redo %1").arg(text)); });
    undo_->setEnabled(false);
    redo_->setEnabled(false);
    tool_bar->addSeparator();
    reload_ = tool_bar->addAction(tr("Reload"), this, [this] {
        if (!dirty_ || QMessageBox::question(this, tr("Reload"),
                                             tr("Reload %1 and lose your unsaved changes?")
                                                 .arg(editor::toQString(source_.filename()))) == QMessageBox::Yes) {
            reload();
        }
    });
    arrange_ = tool_bar->addAction(tr("Arrange"), this, &TreePanel::arrange);
    fit_ = tool_bar->addAction(tr("Fit"), this, &TreePanel::fit);
    open_source_ = tool_bar->addAction(tr("Open Lua Source"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(editor::toQString(source_)));
    });
    auto* header = new QHBoxLayout;
    header->addWidget(title_, 1);
    header->addWidget(tool_bar);

    problems_ = new QLabel;
    problems_->setWordWrap(true);
    problems_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    problems_->setStyleSheet(QString::fromLatin1(ProblemStyle));
    problems_->hide();

    view_ = new TreeView(scene_.get(), graph_);
    // QtNodes binds Escape and Undo window-wide, where they'd clash with the editor's own.
    for (QAction* action : view_->actions()) {
        action->setShortcutContext(Qt::WidgetShortcut);
    }
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QWidget::customContextMenuRequested, this, &TreePanel::showMenu);

    auto* details = new QWidget;
    details_ = new QFormLayout(details);
    auto* scroll = new QScrollArea;
    scroll->setWidget(details);
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(240);

    game_box_ = new QGroupBox(tr("Play"));
    agents_ = new QComboBox;
    agents_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    agents_->setMinimumContentsLength(8);
    connect(agents_, &QComboBox::activated, this, &TreePanel::showGame);
    agent_status_ = valueLabel({});
    blackboard_ = new QTableWidget(0, 2);
    blackboard_->setHorizontalHeaderLabels({tr("Blackboard"), tr("Value")});
    blackboard_->verticalHeader()->hide();
    blackboard_->horizontalHeader()->setStretchLastSection(true);
    blackboard_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    auto* game = new QFormLayout(game_box_);
    game->addRow(tr("Entity"), agents_);
    game->addRow(tr("Status"), agent_status_);
    game->addRow(blackboard_);
    game_box_->hide();
    game_timer_ = new QTimer(this);
    game_timer_->setInterval(100);
    connect(game_timer_, &QTimer::timeout, this, &TreePanel::showGame);

    auto* side = new QSplitter(Qt::Vertical);
    side->addWidget(game_box_);
    side->addWidget(scroll);

    auto* splitter = new QSplitter;
    splitter->addWidget(view_);
    splitter->addWidget(side);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({900, 340});

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(header);
    layout->addWidget(problems_);
    layout->addWidget(splitter, 1);

    watcher_ = new QFileSystemWatcher(this);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, &TreePanel::fileChanged);
    connect(scene_.get(), &QGraphicsScene::selectionChanged, this, &TreePanel::scheduleDetails);
    connect(scene_.get(), &QtNodes::BasicGraphicsScene::nodeDoubleClicked, this, &TreePanel::openScript);
    connect(&graph_, &TreeGraph::changed, this, &TreePanel::updateState);
    connect(&history, &QUndoStack::indexChanged, this, &TreePanel::updateState);
    updateState();
    showDetails();
}

TreePanel::~TreePanel() {
    game_timer_->stop();
    // The scene goes before the panel's widgets; tearing it down must not update them.
    disconnect(scene_.get(), nullptr, this, nullptr);
    disconnect(&scene_->undoStack(), nullptr, this, nullptr);
    disconnect(&graph_, nullptr, this, nullptr);
    scene_.reset();
}

void TreePanel::open(const std::filesystem::path& source) {
    if (source == source_ && dirty_) {
        return;  // already open, with changes to keep
    }
    if (source != source_ && !maybeSave(this)) {
        return;
    }
    if (!watcher_->files().isEmpty()) {
        watcher_->removePaths(watcher_->files());
    }
    source_ = source;
    title_->setToolTip(editor::toQString(source));
    reload();
}

bool TreePanel::maybeSave(QWidget* parent) {
    if (!dirty_) {
        return true;
    }
    const auto answer =
        QMessageBox::question(parent, tr("Unsaved Changes"),
                              tr("Save changes to %1?").arg(editor::toQString(source_.filename())),
                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Save) {
        return save();
    }
    return answer == QMessageBox::Discard;
}

bool TreePanel::save() {
    if (!dirty_) {
        return true;
    }
    const QString name = editor::toQString(source_.filename());
    if (!disk_.empty() && !writtenByEditor(disk_)) {
        const auto answer = QMessageBox::warning(
            this, tr("Save Behavior Tree"),
            tr("%1 wasn't written by this editor. Saving rewrites it: its comments and formatting are lost, "
               "along with anything the graph doesn't show.")
                .arg(name),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Save) {
            return false;
        }
    }
    const std::string text = engine::ai::WriteTreeSource(graph_.tree());
    std::string error;
    if (!engine::WriteFileAtomic(source_, text, error)) {
        QMessageBox::warning(this, tr("Save Behavior Tree"),
                             tr("Can't save %1: %2").arg(name, QString::fromStdString(error)));
        return false;
    }
    disk_ = text;
    baseline_ = text;
    graph_.clearFileProblems();
    watch();
    updateState();
    return true;
}

bool TreePanel::event(QEvent* event) {
    // Ctrl+S saves the tree while the panel has focus, rather than the scene.
    if (event->type() == QEvent::ShortcutOverride &&
        static_cast<QKeyEvent*>(event)->matches(QKeySequence::Save) && !source_.empty()) {
        event->accept();
        return true;
    }
    return QWidget::event(event);
}

void TreePanel::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Save) && !source_.empty()) {
        save();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TreePanel::reload() {
    if (source_.empty()) {
        return;
    }
    watch();
    std::string text, error;
    engine::ai::TreeSource tree;
    const bool read = engine::ReadTextFile(source_, text, error) && engine::ai::ReadTreeSource(text, tree, error);
    disk_ = std::move(text);
    load_error_ = read ? QString() : tr("Can't show this tree: %1").arg(QString::fromStdString(error));
    engine::AssetMetadata metadata;
    tree_id_ = engine::ReadAssetMetadata(source_, metadata, error) ? metadata.id : engine::AssetUuid();
    followed_ = entt::null;  // another tree's agents: follow the selection again
    // Editing an empty graph would save over the file we couldn't read.
    graph_.setEditable(read);
    graph_.load(tree, catalog_);
    graph_.arrange(scene_->nodeGeometry());
    baseline_ = engine::ai::WriteTreeSource(graph_.tree());
    scene_->undoStack().clear();
    updateState();
    fit();
    scheduleDetails();
}

void TreePanel::setGame(Game game) {
    game_ = std::move(game);
    followed_ = entt::null;
    if (game_.agents && game_.state) {
        if (game_box_->isHidden()) {
            game_box_->show();
            auto* side = static_cast<QSplitter*>(game_box_->parentWidget());
            side->setSizes({side->height() / 2, side->height() / 2});  // the node's details keep half
        }
        game_timer_->start();
        showGame();
        return;
    }
    game_timer_->stop();
    game_box_->hide();
    agent_list_.clear();
    agents_->clear();
    blackboard_->setRowCount(0);
    graph_.setActive(InvalidNodeId, false);
}

void TreePanel::showGame() {
    if (!game_.agents || !game_.state) {
        return;
    }
    std::vector<Agent> agents = tree_id_.IsValid() ? game_.agents(tree_id_) : std::vector<Agent>();
    // Watch the one picked here, until the editor selects another.
    const int picked = agents_->currentIndex();
    entt::entity watched = picked >= 0 && picked < std::ssize(agent_list_) ? agent_list_[picked].entity : entt::null;
    const auto selected = std::ranges::find_if(agents, &Agent::selected);
    if (const entt::entity selection = selected != agents.end() ? selected->entity : entt::null; selection != followed_) {
        followed_ = selection;
        watched = selection != entt::null ? selection : watched;
    }
    const auto same = [](const Agent& a, const Agent& b) { return a.entity == b.entity && a.name == b.name; };
    if (!std::ranges::equal(agents, agent_list_, same)) {
        const QSignalBlocker blocker(agents_);
        agents_->clear();
        for (const Agent& agent : agents) {
            agents_->addItem(agent.name);
        }
        agent_list_ = std::move(agents);
    }
    const auto found = std::ranges::find(agent_list_, watched, &Agent::entity);
    const int index = found != agent_list_.end() ? static_cast<int>(found - agent_list_.begin()) : agent_list_.empty() ? -1 : 0;
    agents_->setCurrentIndex(index);
    agents_->setEnabled(!agent_list_.empty());

    const auto state = index >= 0 ? game_.state(agent_list_[index].entity) : std::nullopt;
    const bool faulted = state && state->status == "Faulted";
    QString status;
    QString fault;
    if (agent_list_.empty()) {
        status = tr("No entity runs this tree. Give one a Behavior Tree component that uses it.");
    } else if (!state) {
        status = tr("Waiting for its first update.");
    } else {
        status = QString::fromStdString(state->status);
        if (!state->node.empty()) {
            status = tr("%1 at %2").arg(status, QString::fromStdString(state->node));
        }
        if (faulted) {
            fault = QString::fromStdString(state->fault);
            status = tr("%1: %2").arg(status, fault.section(QLatin1Char('\n'), 0, 0));  // the traceback goes in the tooltip
        }
    }
    agent_status_->setText(status);
    agent_status_->setToolTip(fault);
    if (const QString style = faulted ? QString::fromLatin1(ProblemStyle) : QString(); agent_status_->styleSheet() != style) {
        agent_status_->setStyleSheet(style);
    }
    graph_.setActive(state ? graph_.find(state->node) : InvalidNodeId, faulted);

    const auto entries = state ? state->blackboard : decltype(state->blackboard)();
    blackboard_->setRowCount(static_cast<int>(entries.size()));
    for (int row = 0; row < blackboard_->rowCount(); ++row) {
        const auto& [key, value] = entries[static_cast<std::size_t>(row)];
        for (const auto& [column, text] : {std::pair(0, &key), std::pair(1, &value)}) {
            if (QTableWidgetItem* item = blackboard_->item(row, column)) {
                item->setText(QString::fromStdString(*text));
            } else {
                blackboard_->setItem(row, column, new QTableWidgetItem(QString::fromStdString(*text)));
            }
        }
    }
}

void TreePanel::fileChanged() {
    if (asking_ || source_.empty()) {
        return;
    }
    watch();
    std::string text, error;
    const bool read = engine::ReadTextFile(source_, text, error);
    if (read && text == disk_) {
        return;  // our own save
    }
    if (dirty_) {
        const QScopedValueRollback guard(asking_, true);
        const auto answer = QMessageBox::question(this, tr("File Changed"),
                                                  tr("%1 changed on disk. Reload it and lose your unsaved changes?")
                                                      .arg(editor::toQString(source_.filename())));
        if (answer != QMessageBox::Yes) {
            disk_ = text;  // saving replaces what's there now
            return;
        }
    }
    reload();
}

void TreePanel::watch() {
    if (const QString path = editor::toQString(source_);
        !source_.empty() && !watcher_->files().contains(path) && QFile::exists(path)) {
        watcher_->addPath(path);
    }
}

void TreePanel::updateState() {
    dirty_ = graph_.editable() && engine::ai::WriteTreeSource(graph_.tree()) != baseline_;
    const bool open = !source_.empty();
    if (open) {
        title_->setText(editor::toQString(source_.filename()) + (dirty_ ? QStringLiteral(" *") : QString()));
    }
    save_->setEnabled(dirty_);
    for (QAction* action : {reload_, arrange_, fit_, open_source_}) {
        action->setEnabled(open);
    }

    QStringList problems = graph_.problems();
    if (!load_error_.isEmpty()) {
        problems.prepend(load_error_);
    }
    const auto ids = graph_.allNodeIds();
    if (const auto broken =
            std::ranges::count_if(ids, [this](const NodeId id) { return !graph_.node(id)->problems.isEmpty(); });
        broken > 0) {
        problems << tr("%n node(s) can't run: hover one, or select it, to see why.", nullptr,
                       static_cast<int>(broken));
    }
    problems_->setText(problems.join(QLatin1Char('\n')));
    problems_->setVisible(!problems.isEmpty());

    if (node_problems_ != nullptr) {
        const TreeGraph::Node* node = graph_.node(details_node_);
        const QString text = node != nullptr ? node->problems.join(QLatin1Char('\n')) : QString();
        node_problems_->setText(text);
        details_->setRowVisible(node_problems_.data(), !text.isEmpty());
    }
}

void TreePanel::fit() {
    // Fit large trees, but don't blow small ones up.
    view_->zoomFitAll();
    if (view_->transform().m11() > 1.0) {
        view_->setupScale(1.0);
    }
    view_->centerOn(scene_->itemsBoundingRect().center());
}

void TreePanel::arrange() {
    if (!edit(tr("Arrange"), [this] {
            graph_.arrange(scene_->nodeGeometry());
            return graph_.editable();
        })) {
        return;
    }
    fit();
}

void TreePanel::scheduleDetails() {
    if (!std::exchange(details_pending_, true)) {
        QTimer::singleShot(0, this, [this] {
            if (details_pending_) {
                showDetails();
            }
        });
    }
}

void TreePanel::showDetails() {
    details_pending_ = false;
    while (details_->rowCount() > 0) {
        details_->removeRow(0);
    }
    details_node_ = selectedNode();
    const TreeGraph::Node* node = graph_.node(details_node_);
    if (node == nullptr) {
        details_->addRow(valueLabel(graph_.editable() ? tr("Select a node to edit it. Right-click the graph to add one.")
                                                      : tr("Select a node to see its details.")));
        return;
    }
    const NodeId id = details_node_;
    const engine::ai::NodeSource& source = node->source;

    edit_error_ = valueLabel({});
    edit_error_->setStyleSheet(QString::fromLatin1(ProblemStyle));
    details_->addRow(edit_error_);
    details_->setRowVisible(edit_error_.data(), false);

    auto* name = new QLineEdit(QString::fromStdString(source.id));
    connect(name, &QLineEdit::editingFinished, this, [this, id, name] {
        QString error;
        const std::string text = name->text().trimmed().toStdString();
        edit(tr("Rename Node"), [&] { return graph_.rename(id, text, error); });
        if (const TreeGraph::Node* renamed = graph_.node(id)) {
            name->setText(QString::fromStdString(renamed->source.id));
        }
        showEditError(error);
    });
    details_->addRow(tr("ID"), name);

    auto* kind = new QComboBox;
    for (const NodeKind each : NodeKinds) {
        kind->addItem(kindName(each), static_cast<int>(each));
    }
    kind->setCurrentIndex(kind->findData(static_cast<int>(source.kind)));
    connect(kind, &QComboBox::activated, this, [this, id, kind](const int index) {
        QString error;
        const auto chosen = static_cast<NodeKind>(kind->itemData(index).toInt());
        if (edit(tr("Change Kind"), [&] { return graph_.setKind(id, chosen, error); })) {
            scheduleDetails();  // the rows below depend on the kind
        } else if (const TreeGraph::Node* unchanged = graph_.node(id)) {
            kind->setCurrentIndex(kind->findData(static_cast<int>(unchanged->source.kind)));
        }
        showEditError(error);
    });
    details_->addRow(tr("Kind"), kind);

    if (id == graph_.root()) {
        details_->addRow(tr("Root"), valueLabel(tr("The tree starts here.")));
    } else if (node->parent == InvalidNodeId) {
        auto* root = new QPushButton(tr("Set as Root"));
        connect(root, &QPushButton::clicked, this, [this, id] {
            if (edit(tr("Set Root"), [&] { return graph_.setRoot(id); })) {
                scheduleDetails();
            }
        });
        details_->addRow(tr("Root"), root);
    }

    if (isLeaf(source.kind)) {
        auto* script = new editor::AssetPicker(catalog_, engine::CatalogAssetKind::Script, tr("(None)"));
        // Long script paths mustn't widen the details past the panel.
        script->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        script->setMinimumContentsLength(8);
        script->selectPath(node->script);
        connect(script, &QComboBox::currentIndexChanged, this, [this, id, script] {
            const auto reference = script->reference();
            const std::string path = reference ? reference->path : std::string();
            if (edit(tr("Choose Script"), [&] { return graph_.setScript(id, path); })) {
                scheduleDetails();
            }
        });
        auto* open = new QPushButton(tr("Open"));
        open->setEnabled(!node->script.isEmpty());
        connect(open, &QPushButton::clicked, this, [this, id] { openScript(id); });
        auto* row = new QHBoxLayout;
        row->addWidget(script, 1);
        row->addWidget(open);
        details_->addRow(tr("Script"), row);
    }

    // Only leaves read parameters, but a composite's are shown so they can be removed.
    if (isLeaf(source.kind) || !source.parameters.empty()) {
        details_->addRow(new QLabel(tr("Parameters")));
        details_->addRow(new ParameterEditor(source.parameters, [this, id](engine::ScriptPropertyMap values) {
            edit(tr("Edit Parameters"), [&] { return graph_.setParameters(id, std::move(values)); });
        }));
    }

    node_problems_ = valueLabel(node->problems.join(QLatin1Char('\n')));
    node_problems_->setStyleSheet(QString::fromLatin1(ProblemStyle));
    details_->addRow(tr("Problems"), node_problems_);
    details_->setRowVisible(node_problems_.data(), !node->problems.isEmpty());
}

void TreePanel::showEditError(const QString& error) {
    if (edit_error_ != nullptr) {
        edit_error_->setText(error);
        details_->setRowVisible(edit_error_.data(), !error.isEmpty());
    }
}

void TreePanel::showMenu(const QPoint& position) {
    if (!graph_.editable()) {
        return;
    }
    QMenu menu(this);
    QGraphicsItem* item = view_->itemAt(position);
    if (auto* node_item = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
        const NodeId id = node_item->nodeId();
        if (!node_item->isSelected()) {
            select(id);
        }
        const TreeGraph::Node* node = graph_.node(id);
        if (!isLeaf(node->source.kind)) {
            QMenu* add = menu.addMenu(tr("Add Child"));
            for (const NodeKind kind : NodeKinds) {
                add->addAction(kindName(kind), this, [this, id, kind] { addChild(id, kind); });
            }
        }
        QAction* root = menu.addAction(tr("Set as Root"), this, [this, id] {
            if (edit(tr("Set Root"), [&] { return graph_.setRoot(id); })) {
                scheduleDetails();
            }
        });
        root->setEnabled(node->parent == InvalidNodeId && id != graph_.root());
        if (!node->script.isEmpty()) {
            menu.addAction(tr("Open Script"), this, [this, id] { openScript(id); });
        }
        menu.addSeparator();
        menu.addAction(tr("Delete"), view_, &QtNodes::GraphicsView::onDeleteSelectedObjects);
    } else if (auto* link = qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
        scene_->clearSelection();
        link->setSelected(true);
        menu.addAction(tr("Delete Link"), view_, &QtNodes::GraphicsView::onDeleteSelectedObjects);
    } else {
        const QPointF at = view_->mapToScene(position);
        for (const NodeKind kind : NodeKinds) {
            menu.addAction(tr("Add %1").arg(kindName(kind)), this, [this, kind, at] { addNode(kind, at); });
        }
        menu.addSeparator();
        // Where the menu opened: the view's own paste goes to the cursor, which is now on this item.
        QAction* paste = menu.addAction(tr("Paste"), this, [this, at] {
            if (graph_.pastable(clipboardGraph())) {
                scene_->undoStack().push(new QtNodes::PasteCommand(scene_.get(), at));
            }
        });
        paste->setEnabled(graph_.pastable(clipboardGraph()));
    }
    menu.exec(view_->viewport()->mapToGlobal(position));
}

void TreePanel::openScript(const NodeId id) {
    const TreeGraph::Node* node = graph_.node(id);
    if (node == nullptr || node->script.isEmpty()) {
        return;
    }
    if (const engine::CatalogEntry* entry = catalog_.FindPath(node->script.toStdString())) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(editor::toQString(entry->source)));
    }
}

bool TreePanel::edit(const QString& text, const std::function<bool()>& change) {
    State before{graph_.snapshot(), selectedNode()};
    if (!change()) {
        return false;
    }
    scene_->undoStack().push(new SnapshotCommand(text, std::move(before), {graph_.snapshot(), selectedNode()},
                                                 [this](const State& state) {
                                                     graph_.restore(state.graph);
                                                     select(state.selected);
                                                 }));
    return true;
}

void TreePanel::addNode(const NodeKind kind, const QPointF position) {
    edit(tr("Add %1").arg(kindName(kind)), [&] {
        select(graph_.create(kind, position));
        return true;
    });
}

void TreePanel::addChild(const NodeId parent, const NodeKind kind) {
    const TreeGraph::Node* node = graph_.node(parent);
    if (node == nullptr) {
        return;
    }
    // Right of its last sibling, so it runs last.
    QPointF at(node->position.x(), node->position.y() + node->size.height() + RowGap);
    if (!node->children.empty()) {
        const TreeGraph::Node* last = graph_.node(node->children.back());
        at = QPointF(last->position.x() + last->size.width() + Gap, last->position.y());
    }
    edit(tr("Add %1").arg(kindName(kind)), [&] {
        const NodeId child = graph_.create(kind, at);
        graph_.addConnection({parent, 0, child, 0});
        select(child);
        return true;
    });
}

NodeId TreePanel::selectedNode() const {
    const QList<QGraphicsItem*> selected = scene_->selectedItems();
    auto* item = selected.size() == 1 ? qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(selected.front()) : nullptr;
    return item != nullptr ? item->nodeId() : InvalidNodeId;
}

void TreePanel::select(const NodeId id) {
    scene_->clearSelection();
    if (QtNodes::NodeGraphicsObject* item = scene_->nodeGraphicsObject(id)) {
        item->setSelected(true);
    }
}

}  // namespace behavior
