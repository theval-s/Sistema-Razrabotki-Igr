#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDeadlineTimer>
#include <QGroupBox>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QUndoStack>

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/NodeStyle>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/UndoCommands.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <engine/ai/BehaviorTreeSource.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/core/Uuid.hpp>

#include "tree_graph.hpp"
#include "tree_panel.hpp"

namespace {

using behavior::TreeGraph;
using engine::ai::NodeKind;
using engine::ai::TreeSource;
using QtNodes::NodeRole;
using QtNodes::PortRole;
using QtNodes::PortType;

/// A game root holding the pulse demo's three leaf scripts, cataloged.
struct Scripts {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-bt-graph-" + engine::NewUuid());
    engine::AssetCatalog catalog;

    Scripts() {
        std::filesystem::create_directories(root / "game" / "ai");
        std::filesystem::create_directories(root / "engine");
        std::string error;
        for (const char* name : {"enabled.condition.lua", "pulse.action.lua", "idle.action.lua"}) {
            const auto file = root / "game" / "ai" / name;
            std::ofstream(file) << "return {}";
            EXPECT_TRUE(engine::WriteAssetMetadata(file, {engine::NewUuid(), engine::CatalogAssetKind::Script}, error))
                << error;
        }
        EXPECT_TRUE(catalog.Scan(root / "game", root / "engine", error)) << error;
    }
    ~Scripts() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

TreeSource pulse() {
    return {"main",
            {{"main", NodeKind::Selector, {"pulse", "idle"}, {}, {}},
             {"pulse", NodeKind::Sequence, {"enabled", "wait"}, {}, {}},
             {"enabled", NodeKind::Condition, {}, "game/ai/enabled.condition.lua", {}},
             {"wait", NodeKind::Action, {}, "game/ai/pulse.action.lua", {{"seconds", 1.0}}},
             {"idle", NodeKind::Action, {}, "game/ai/idle.action.lua", {}}}};
}

// Nodes get IDs in file order.
constexpr TreeGraph::NodeId Main = 0, Pulse = 1, Enabled = 2, Wait = 3, Idle = 4;

QString problemsOf(const TreeGraph& graph, const TreeGraph::NodeId id) {
    return graph.node(id)->problems.join(QLatin1Char('\n'));
}

TEST(BehaviorTreeGraph, ConnectsCompositesToTheirChildrenInRunOrder) {
    Scripts scripts;
    TreeGraph graph;
    graph.load(pulse(), scripts.catalog);

    EXPECT_TRUE(graph.problems().isEmpty()) << graph.problems().join(QLatin1Char('\n')).toStdString();
    EXPECT_EQ(graph.allNodeIds().size(), 5u);
    for (const auto id : graph.allNodeIds()) {
        EXPECT_TRUE(graph.node(id)->problems.isEmpty()) << problemsOf(graph, id).toStdString();
    }
    EXPECT_EQ(graph.node(Main)->children, (std::vector<TreeGraph::NodeId>{Pulse, Idle}));
    EXPECT_EQ(graph.node(Pulse)->children, (std::vector<TreeGraph::NodeId>{Enabled, Wait}));
    EXPECT_TRUE(graph.connectionExists({Pulse, 0, Wait, 0}));
    EXPECT_FALSE(graph.connectionExists({Main, 0, Wait, 0}));
    EXPECT_EQ(graph.allConnectionIds(Pulse).size(), 3u);

    // The root has nothing above it, and leaves nothing below.
    EXPECT_EQ(graph.nodeData<unsigned>(Main, NodeRole::InPortCount), 0u);
    EXPECT_EQ(graph.nodeData<unsigned>(Pulse, NodeRole::InPortCount), 1u);
    EXPECT_EQ(graph.nodeData<unsigned>(Pulse, NodeRole::OutPortCount), 1u);
    EXPECT_EQ(graph.nodeData<unsigned>(Wait, NodeRole::OutPortCount), 0u);
    EXPECT_EQ(graph.portData<QString>(Wait, PortType::In, 0, PortRole::Caption), QStringLiteral("2"));
    EXPECT_EQ(graph.nodeData<QString>(Wait, NodeRole::Caption), QStringLiteral("Action %1 wait").arg(QChar(0x00B7)));
    EXPECT_EQ(graph.node(Wait)->script, QStringLiteral("game/ai/pulse.action.lua"));
    EXPECT_FALSE(graph.nodeData(Wait, NodeRole::ValidationState).isValid());
}

TEST(BehaviorTreeGraph, ReportsWhatTheRuntimeWouldReject) {
    Scripts scripts;
    TreeGraph graph;
    graph.load({"main",
                {{"main", NodeKind::Selector, {"missing", "act", "act", "main"}, {}, {}},
                 {"act", NodeKind::Action, {"main"}, "game/ai/nope.lua", {}},
                 {"act", NodeKind::Condition, {}, "", {}},
                 {"empty", NodeKind::Sequence, {}, {}, {}}}},
               scripts.catalog);

    EXPECT_TRUE(graph.problems().isEmpty());
    const QString main = problemsOf(graph, 0);
    EXPECT_TRUE(main.contains(QStringLiteral("No node 'missing'"))) << main.toStdString();
    EXPECT_TRUE(main.contains(QStringLiteral("'act' already runs under 'main'"))) << main.toStdString();
    EXPECT_TRUE(main.contains(QStringLiteral("'main' is the root"))) << main.toStdString();
    const QString act = problemsOf(graph, 1);
    EXPECT_TRUE(act.contains(QStringLiteral("No script asset 'game/ai/nope.lua'"))) << act.toStdString();
    EXPECT_TRUE(act.contains(QStringLiteral("can't have children"))) << act.toStdString();
    const QString duplicate = problemsOf(graph, 2);
    EXPECT_TRUE(duplicate.contains(QStringLiteral("also called 'act'"))) << duplicate.toStdString();
    EXPECT_TRUE(duplicate.contains(QStringLiteral("Choose a script"))) << duplicate.toStdString();
    EXPECT_TRUE(duplicate.contains(QStringLiteral("never reaches"))) << duplicate.toStdString();
    const QString empty = problemsOf(graph, 3);
    EXPECT_TRUE(empty.contains(QStringLiteral("at least one child"))) << empty.toStdString();
    EXPECT_TRUE(graph.nodeData(3, NodeRole::ValidationState).isValid());
    EXPECT_EQ(graph.node(0)->children, (std::vector<TreeGraph::NodeId>{1}));

    graph.load({"gone", {{"a", NodeKind::Action, {}, "game/ai/idle.action.lua", {}}}}, scripts.catalog);
    EXPECT_TRUE(graph.problems().join(QLatin1Char('\n')).contains(QStringLiteral("'gone'")));
    EXPECT_TRUE(graph.node(0)->problems.isEmpty()) << "without a root, reachability isn't reported per node";
}

TEST(BehaviorTreeGraph, ArrangesTopDownWithChildrenLeftToRight) {
    Scripts scripts;
    TreeGraph graph;
    QtNodes::BasicGraphicsScene scene(graph);
    scene.setOrientation(Qt::Vertical);
    graph.load(pulse(), scripts.catalog);
    graph.arrange(scene.nodeGeometry());

    const auto position = [&](const TreeGraph::NodeId id) { return graph.node(id)->position; };
    const auto right = [&](const TreeGraph::NodeId id) { return position(id).x() + graph.node(id)->size.width(); };
    const auto centre = [&](const TreeGraph::NodeId id) { return position(id).x() + graph.node(id)->size.width() / 2.0; };
    EXPECT_LT(position(Main).y(), position(Pulse).y());
    EXPECT_EQ(position(Pulse).y(), position(Idle).y());
    EXPECT_LT(position(Pulse).y(), position(Enabled).y());
    EXPECT_EQ(position(Enabled).y(), position(Wait).y());
    EXPECT_LT(right(Enabled), position(Wait).x());
    EXPECT_LT(right(Wait), position(Idle).x()) << "the pulse branch sits left of idle";
    EXPECT_NEAR(centre(Pulse), (position(Enabled).x() + right(Wait)) / 2.0, 1.0);
    EXPECT_GT(graph.node(Main)->size.width(), 0);
    EXPECT_NE(scene.nodeGraphicsObject(Wait), nullptr);
}

TEST(BehaviorTreeGraph, WritesTheTreeItShows) {
    Scripts scripts;
    TreeGraph graph;
    graph.load(pulse(), scripts.catalog);
    EXPECT_EQ(graph.tree(), pulse());

    // Links the graph can't draw are left out, as its problems warn.
    graph.load({"main",
                {{"main", NodeKind::Selector, {"idle", "missing", "idle"}, {}, {}},
                 {"idle", NodeKind::Action, {"main"}, "game/ai/idle.action.lua", {}}}},
               scripts.catalog);
    EXPECT_TRUE(problemsOf(graph, 0).contains(QStringLiteral("saving drops it"))) << problemsOf(graph, 0).toStdString();
    const TreeSource written = graph.tree();
    EXPECT_EQ(written.nodes[0].children, (std::vector<std::string>{"idle"}));
    EXPECT_TRUE(written.nodes[1].children.empty());
    graph.clearFileProblems();
    EXPECT_TRUE(graph.node(0)->problems.isEmpty()) << problemsOf(graph, 0).toStdString();
}

TEST(BehaviorTreeGraph, LinksOnlyWhatTheRuntimeAccepts) {
    Scripts scripts;
    TreeGraph graph;
    graph.load(pulse(), scripts.catalog);
    EXPECT_FALSE(graph.connectionPossible({Main, 0, Wait, 0})) << "not editable yet";
    graph.setEditable(true);

    EXPECT_FALSE(graph.connectionPossible({Pulse, 0, Idle, 0})) << "idle already runs under main";
    EXPECT_FALSE(graph.connectionPossible({Wait, 0, Idle, 0})) << "actions have no children";
    EXPECT_FALSE(graph.connectionPossible({Pulse, 0, Main, 0})) << "the root runs under nothing";
    EXPECT_TRUE(graph.deleteConnection({Main, 0, Idle, 0}));
    EXPECT_TRUE(problemsOf(graph, Idle).contains(QStringLiteral("never reaches"))) << problemsOf(graph, Idle).toStdString();
    EXPECT_TRUE(graph.connectionPossible({Pulse, 0, Idle, 0}));

    // A node can't run its own parent.
    const auto inner = graph.create(NodeKind::Selector, {});
    graph.addConnection({Pulse, 0, inner, 0});
    EXPECT_TRUE(graph.connectionExists({Pulse, 0, inner, 0}));
    EXPECT_TRUE(graph.deleteConnection({Main, 0, Pulse, 0}));
    EXPECT_FALSE(graph.connectionPossible({inner, 0, Pulse, 0}));
    EXPECT_FALSE(graph.connectionPossible({inner, 0, inner, 0}));
}

TEST(BehaviorTreeGraph, RunsChildrenLeftToRight) {
    Scripts scripts;
    TreeGraph graph;
    graph.load(pulse(), scripts.catalog);
    graph.setEditable(true);
    graph.setNodeData(Enabled, NodeRole::Position, QPointF(0, 100));
    graph.setNodeData(Wait, NodeRole::Position, QPointF(200, 100));
    ASSERT_TRUE(graph.deleteConnection({Main, 0, Idle, 0}));

    graph.setNodeData(Idle, NodeRole::Position, QPointF(400, 100));
    graph.addConnection({Pulse, 0, Idle, 0});
    EXPECT_EQ(graph.node(Pulse)->children, (std::vector<TreeGraph::NodeId>{Enabled, Wait, Idle}));
    graph.setNodeData(Idle, NodeRole::Position, QPointF(-200, 100));
    EXPECT_EQ(graph.node(Pulse)->children, (std::vector<TreeGraph::NodeId>{Idle, Enabled, Wait}));
    EXPECT_EQ(graph.portData<QString>(Wait, PortType::In, 0, PortRole::Caption), QStringLiteral("3"));
    EXPECT_EQ(graph.tree().nodes[Pulse].children, (std::vector<std::string>{"idle", "enabled", "wait"}));
}

TEST(BehaviorTreeGraph, EditsNodesWithinTheRules) {
    Scripts scripts;
    TreeGraph graph;
    graph.load({}, scripts.catalog);
    graph.setEditable(true);

    const auto first = graph.create(NodeKind::Sequence, {});
    EXPECT_EQ(graph.root(), first) << "the first node of an empty tree is its root";
    const auto action = graph.create(NodeKind::Action, {});
    const auto other = graph.create(NodeKind::Action, {});
    EXPECT_EQ(graph.node(action)->source.id, "action");
    EXPECT_EQ(graph.node(other)->source.id, "action2");
    EXPECT_TRUE(problemsOf(graph, action).contains(QStringLiteral("Choose a script")));

    QString error;
    EXPECT_FALSE(graph.rename(other, "action", error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(graph.rename(other, "", error));
    EXPECT_TRUE(graph.rename(other, "idle", error));
    EXPECT_TRUE(graph.setScript(other, "game/ai/idle.action.lua"));
    EXPECT_EQ(graph.node(other)->script, QStringLiteral("game/ai/idle.action.lua"));
    EXPECT_TRUE(graph.setParameters(other, {{"speed", 2.5}}));
    EXPECT_FALSE(graph.setParameters(other, {{"speed", 2.5}})) << "nothing changed";

    graph.addConnection({first, 0, other, 0});
    error.clear();
    EXPECT_FALSE(graph.setKind(first, NodeKind::Condition, error)) << "it has a child";
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(graph.setKind(other, NodeKind::Selector, error));
    EXPECT_TRUE(graph.node(other)->source.script.empty()) << "composites run no script";
    EXPECT_EQ(graph.nodeData<unsigned>(other, NodeRole::OutPortCount), 1u);

    EXPECT_FALSE(graph.setRoot(other)) << "it runs under the root";
    EXPECT_TRUE(graph.setRoot(action));
    EXPECT_EQ(graph.nodeData<unsigned>(first, NodeRole::InPortCount), 1u);
    EXPECT_EQ(graph.nodeData<unsigned>(action, NodeRole::InPortCount), 0u);

    const TreeGraph::Snapshot before = graph.snapshot();
    EXPECT_TRUE(graph.rename(action, "renamed", error));
    graph.restore(before);
    EXPECT_EQ(graph.node(action)->source.id, "action");
    EXPECT_EQ(graph.root(), action);
}

TEST(BehaviorTreeGraph, BringsDeletedNodesBackForUndoAndPaste) {
    Scripts scripts;
    TreeGraph graph;
    graph.load(pulse(), scripts.catalog);
    graph.setEditable(true);

    // As QtNodes' delete and its undo do it.
    const QJsonObject wait = graph.saveNode(Wait);
    EXPECT_TRUE(graph.deleteNode(Wait));
    EXPECT_FALSE(graph.nodeExists(Wait));
    EXPECT_EQ(graph.node(Pulse)->children, (std::vector<TreeGraph::NodeId>{Enabled}));
    graph.loadNode(wait);
    graph.addConnection({Pulse, 0, Wait, 0});
    EXPECT_EQ(graph.tree(), pulse());

    const QJsonObject main = graph.saveNode(Main);
    EXPECT_TRUE(graph.deleteNode(Main));
    EXPECT_EQ(graph.root(), QtNodes::InvalidNodeId);
    EXPECT_FALSE(graph.problems().isEmpty());
    graph.loadNode(main);
    EXPECT_EQ(graph.root(), Main);

    // A pasted copy gets a new ID and a name of its own.
    QJsonObject copy = wait;
    const auto pasted = graph.newNodeId();
    copy["id"] = static_cast<qint64>(pasted);
    graph.loadNode(copy);
    EXPECT_EQ(graph.node(pasted)->source.id, "wait2");
    EXPECT_EQ(graph.node(pasted)->source.parameters, (engine::ScriptPropertyMap{{"seconds", 1.0}}));
}

TEST(BehaviorTreeGraph, PastesOnlyWhatItCanLink) {
    Scripts scripts;
    TreeGraph graph;
    graph.load(pulse(), scripts.catalog);
    const auto link = [](const TreeGraph::NodeId parent, const TreeGraph::NodeId child) {
        return QJsonObject{{"outNodeId", static_cast<qint64>(parent)}, {"outPortIndex", 0},
                           {"inNodeId", static_cast<qint64>(child)}, {"inPortIndex", 0}};
    };
    const auto clip = [&](const QJsonArray& links) {
        return QJsonObject{{"nodes", QJsonArray{graph.saveNode(Main), graph.saveNode(Pulse), graph.saveNode(Wait)}},
                           {"connections", links}};
    };
    EXPECT_TRUE(graph.pastable(clip({link(Main, Pulse), link(Pulse, Wait)})));
    EXPECT_FALSE(graph.pastable(clip({link(Wait, Pulse)}))) << "actions have no children";
    EXPECT_FALSE(graph.pastable(clip({link(Pulse, Main)}))) << "the root runs under nothing";
    EXPECT_FALSE(graph.pastable(clip({link(Main, Wait), link(Pulse, Wait)}))) << "one parent each";
    EXPECT_FALSE(graph.pastable(clip({link(Main, Idle)}))) << "idle isn't in the clip";
    EXPECT_FALSE(graph.pastable({}));
}

TEST(BehaviorTreeGraph, WorksWithQtNodesCommands) {
    Scripts scripts;
    TreeGraph graph;
    QtNodes::BasicGraphicsScene scene(graph);
    graph.load(pulse(), scripts.catalog);
    graph.setEditable(true);
    graph.arrange(scene.nodeGeometry());
    QUndoStack& history = scene.undoStack();
    const auto select = [&](std::initializer_list<TreeGraph::NodeId> ids) {
        scene.clearSelection();
        for (const auto id : ids) {
            scene.nodeGraphicsObject(id)->setSelected(true);
        }
    };

    select({Pulse});
    history.push(new QtNodes::DeleteCommand(&scene));
    EXPECT_FALSE(graph.nodeExists(Pulse));
    EXPECT_EQ(graph.node(Enabled)->parent, QtNodes::InvalidNodeId);
    history.undo();
    EXPECT_EQ(graph.tree(), pulse());
    history.redo();
    history.undo();
    EXPECT_EQ(graph.tree(), pulse());

    // The panel's view selects the links between copied nodes too.
    select({Pulse, Enabled, Wait});
    scene.connectionGraphicsObject({Pulse, 0, Enabled, 0})->setSelected(true);
    scene.connectionGraphicsObject({Pulse, 0, Wait, 0})->setSelected(true);
    history.push(new QtNodes::CopyCommand(&scene));
    ASSERT_TRUE(graph.pastable(QJsonDocument::fromJson(
                                   QApplication::clipboard()->mimeData()->data(QStringLiteral("application/qt-nodes-graph")))
                                   .object()));
    history.push(new QtNodes::PasteCommand(&scene, QPointF(0, 500)));
    const TreeSource pasted = graph.tree();
    ASSERT_EQ(pasted.nodes.size(), 8u);
    const auto named = [&](const std::string& id) {
        return *std::ranges::find(pasted.nodes, id, &engine::ai::NodeSource::id);
    };
    EXPECT_EQ(named("pulse2").children, (std::vector<std::string>{"enabled2", "wait2"}));
    EXPECT_EQ(named("wait2").script, "game/ai/pulse.action.lua");
    EXPECT_EQ(named("wait2").parameters, (engine::ScriptPropertyMap{{"seconds", 1.0}}));
    EXPECT_EQ(named("pulse").children, (std::vector<std::string>{"enabled", "wait"}));
    history.undo();
    EXPECT_EQ(graph.tree(), pulse());

    // Moving a child past its sibling changes the run order, and undo changes it back.
    select({Enabled});
    const double past = graph.node(Wait)->position.x() + 10.0 - graph.node(Enabled)->position.x();
    history.push(new QtNodes::MoveNodeCommand(&scene, QPointF(past, 0)));
    EXPECT_EQ(graph.node(Pulse)->children, (std::vector<TreeGraph::NodeId>{Wait, Enabled}));
    history.undo();
    EXPECT_EQ(graph.node(Pulse)->children, (std::vector<TreeGraph::NodeId>{Enabled, Wait}));
}

/// Runs the event loop until `done`, as the file watcher and the panel's queued updates need.
template <class Done>
bool waitFor(Done done) {
    const QDeadlineTimer deadline(5000);
    while (!done() && !deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return done();
}

bool showsLabel(const QWidget& widget, const QString& text) {
    const auto labels = widget.findChildren<QLabel*>();
    return std::ranges::any_of(labels, [&](const QLabel* label) { return label->text().startsWith(text); });
}

QLineEdit* field(const QWidget& widget, const QString& text) {
    const auto fields = widget.findChildren<QLineEdit*>();
    const auto found = std::ranges::find(fields, text, &QLineEdit::text);
    return found != fields.end() ? *found : nullptr;
}

/// Clicks `button` on the message box the next call opens.
void answerNextMessage(const QMessageBox::StandardButton button) {
    QTimer::singleShot(0, [button] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            box->button(button)->click();
        }
    });
}

/// Chooses `text` in the next menu that opens.
void chooseNextMenuItem(const QString& text) {
    QTimer::singleShot(0, [text] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            const auto actions = menu->actions();
            if (const auto found = std::ranges::find(actions, text, &QAction::text); found != actions.end()) {
                (*found)->trigger();
            }
            menu->close();
        }
    });
}

std::string readFile(const std::filesystem::path& file) {
    std::string text, error;
    EXPECT_TRUE(engine::ReadTextFile(file, text, error)) << error;
    return text;
}

QtNodes::BasicGraphicsScene& sceneOf(const behavior::TreePanel& panel) {
    return *static_cast<QtNodes::BasicGraphicsScene*>(panel.findChild<QtNodes::GraphicsView*>()->scene());
}

TreeGraph& graphOf(const behavior::TreePanel& panel) {
    return static_cast<TreeGraph&>(sceneOf(panel).graphModel());
}

TEST(BehaviorTreePanel, EditsUndoesSavesAndReloads) {
    Scripts scripts;
    const auto file = scripts.root / "game" / "ai" / "pulse.bt.lua";
    std::string error;
    ASSERT_TRUE(engine::WriteFileAtomic(file, engine::ai::WriteTreeSource(pulse()), error)) << error;
    behavior::TreePanel panel(scripts.catalog);
    panel.open(file);
    TreeGraph& graph = graphOf(panel);
    QtNodes::BasicGraphicsScene& scene = sceneOf(panel);
    EXPECT_EQ(graph.tree(), pulse());
    EXPECT_TRUE(showsLabel(panel, QStringLiteral("pulse.bt.lua")));

    // Renamed in the details, as the user would.
    scene.nodeGraphicsObject(Wait)->setSelected(true);
    QLineEdit* id = nullptr;
    ASSERT_TRUE(waitFor([&] { return (id = field(panel, QStringLiteral("wait"))) != nullptr; }));
    id->setText(QStringLiteral("pause"));
    Q_EMIT id->editingFinished();
    EXPECT_EQ(graph.node(Wait)->source.id, "pause");
    EXPECT_TRUE(showsLabel(panel, QStringLiteral("pulse.bt.lua *")));
    scene.undoStack().undo();
    EXPECT_EQ(graph.node(Wait)->source.id, "wait");
    EXPECT_FALSE(showsLabel(panel, QStringLiteral("pulse.bt.lua *")));
    scene.undoStack().redo();
    EXPECT_EQ(graph.node(Wait)->source.id, "pause");

    EXPECT_TRUE(panel.save());
    TreeSource saved = pulse();
    saved.nodes[Wait].id = "pause";
    saved.nodes[Pulse].children[1] = "pause";
    EXPECT_EQ(readFile(file), engine::ai::WriteTreeSource(saved));
    EXPECT_FALSE(showsLabel(panel, QStringLiteral("pulse.bt.lua *")));

    // Changed elsewhere, it reloads.
    ASSERT_TRUE(engine::WriteFileAtomic(file, engine::ai::WriteTreeSource(pulse()), error)) << error;
    EXPECT_TRUE(waitFor([&] { return graph.node(Wait) != nullptr && graph.node(Wait)->source.id == "wait"; }));
}

TEST(BehaviorTreePanel, AsksBeforeRewritingAHandWrittenFile) {
    Scripts scripts;
    const auto file = scripts.root / "game" / "ai" / "pulse.bt.lua";
    const std::string hand = "-- Pulses while enabled.\n" + engine::ai::WriteTreeSource(pulse());
    std::string error;
    ASSERT_TRUE(engine::WriteFileAtomic(file, hand, error)) << error;
    behavior::TreePanel panel(scripts.catalog);
    panel.open(file);
    TreeGraph& graph = graphOf(panel);
    QString refused;
    ASSERT_TRUE(graph.rename(Wait, "pause", refused)) << refused.toStdString();

    answerNextMessage(QMessageBox::Cancel);
    EXPECT_FALSE(panel.save());
    EXPECT_EQ(readFile(file), hand);
    answerNextMessage(QMessageBox::Save);
    EXPECT_TRUE(panel.save());
    EXPECT_NE(readFile(file), hand);
    EXPECT_TRUE(panel.maybeSave(&panel)) << "nothing left to save";
}

TEST(BehaviorTreePanel, PastesWhereTheMenuOpened) {
    Scripts scripts;
    const auto file = scripts.root / "game" / "ai" / "pulse.bt.lua";
    std::string error;
    ASSERT_TRUE(engine::WriteFileAtomic(file, engine::ai::WriteTreeSource(pulse()), error)) << error;
    behavior::TreePanel panel(scripts.catalog);
    panel.resize(1200, 800);
    panel.open(file);
    TreeGraph& graph = graphOf(panel);
    auto* view = panel.findChild<QtNodes::GraphicsView*>();
    sceneOf(panel).nodeGraphicsObject(Idle)->setSelected(true);
    view->onCopySelectedObjects();

    const QPoint click(10, 10);
    ASSERT_EQ(view->itemAt(click), nullptr) << "the click must land on empty space";
    QCursor::setPos(view->viewport()->mapToGlobal(QPoint(300, 200)));  // on the menu's item, by then
    chooseNextMenuItem(QStringLiteral("Paste"));
    Q_EMIT view->customContextMenuRequested(click);
    const auto ids = graph.allNodeIds();
    const auto copy = std::ranges::find_if(ids, [&](const TreeGraph::NodeId id) { return graph.node(id)->source.id == "idle2"; });
    ASSERT_NE(copy, ids.end());
    const QPointF at = view->mapToScene(click);
    EXPECT_NEAR(graph.node(*copy)->position.x(), at.x(), 0.01);
    EXPECT_NEAR(graph.node(*copy)->position.y(), at.y(), 0.01);
}

TEST(BehaviorTreePanel, FollowsAnAgentWhileTheGameRuns) {
    Scripts scripts;
    const auto file = scripts.root / "game" / "ai" / "pulse.bt.lua";
    std::string error;
    ASSERT_TRUE(engine::WriteFileAtomic(file, engine::ai::WriteTreeSource(pulse()), error)) << error;
    ASSERT_TRUE(engine::WriteAssetMetadata(file, {engine::NewUuid(), engine::CatalogAssetKind::BehaviorTree}, error))
        << error;
    engine::AssetMetadata metadata;
    ASSERT_TRUE(engine::ReadAssetMetadata(file, metadata, error)) << error;
    behavior::TreePanel panel(scripts.catalog);
    panel.open(file);
    TreeGraph& graph = graphOf(panel);

    using State = engine::LuaRuntime::BehaviorAgentState;
    using Agent = behavior::TreePanel::Agent;
    const auto pulser = static_cast<entt::entity>(7);
    const auto other = static_cast<entt::entity>(9);
    std::map<entt::entity, State> states;
    states[other] = {metadata.id, "Running", "wait", "", {{"pulses", "3"}}};
    panel.setGame({[&](const engine::AssetUuid& tree) {
                       return tree == metadata.id ? std::vector<Agent>{{pulser, QStringLiteral("Pulser"), false},
                                                                       {other, QStringLiteral("Other"), true}}
                                                  : std::vector<Agent>();
                   },
                   [&](const entt::entity entity) {
                       const auto found = states.find(entity);
                       return found != states.end() ? std::optional(found->second) : std::nullopt;
                   }});

    // It watches the entity selected in the editor, and draws its node as if selected.
    EXPECT_EQ(graph.active(), Wait);
    const QtNodes::NodeStyle style(QJsonDocument::fromVariant(graph.nodeData(Wait, NodeRole::Style)).object());
    EXPECT_EQ(style.NormalBoundaryColor, style.SelectedBoundaryColor);
    EXPECT_TRUE(showsLabel(panel, QStringLiteral("Running at wait")));
    auto* blackboard = panel.findChild<QTableWidget*>();
    ASSERT_EQ(blackboard->rowCount(), 1);
    EXPECT_EQ(blackboard->item(0, 0)->text(), QStringLiteral("pulses"));
    EXPECT_EQ(blackboard->item(0, 1)->text(), QStringLiteral("3"));

    states[other] = {metadata.id, "Faulted", "enabled", "enabled.condition.lua:2: boom", {}};
    EXPECT_TRUE(waitFor([&] { return graph.active() == Enabled; }));
    EXPECT_TRUE(showsLabel(panel, QStringLiteral("Faulted at enabled: enabled.condition.lua:2: boom")));
    EXPECT_EQ(blackboard->rowCount(), 0);

    // Picked by hand, it stays on the other entity.
    auto* agents = panel.findChild<QComboBox*>();
    ASSERT_EQ(agents->currentText(), QStringLiteral("Other"));
    agents->setCurrentIndex(0);
    Q_EMIT agents->activated(0);
    EXPECT_EQ(graph.active(), QtNodes::InvalidNodeId);
    EXPECT_TRUE(showsLabel(panel, QStringLiteral("Waiting for its first update.")));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
    EXPECT_EQ(agents->currentText(), QStringLiteral("Pulser"));

    panel.setGame({});
    EXPECT_EQ(graph.active(), QtNodes::InvalidNodeId);
    EXPECT_TRUE(panel.findChild<QGroupBox*>()->isHidden());
}

TEST(BehaviorTreePanel, LeavesAFileItCantReadAlone) {
    Scripts scripts;
    const auto file = scripts.root / "game" / "ai" / "broken.bt.lua";
    std::string error;
    ASSERT_TRUE(engine::WriteFileAtomic(file, "return {", error)) << error;
    behavior::TreePanel panel(scripts.catalog);
    panel.open(file);
    EXPECT_FALSE(graphOf(panel).editable());
    EXPECT_TRUE(showsLabel(panel, QStringLiteral("Can't show this tree")));
    EXPECT_TRUE(panel.maybeSave(&panel));
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    // A message box that nothing answers would hang the run: fail the test instead.
    QTimer watchdog;
    QPointer<QMessageBox> waiting;
    QObject::connect(&watchdog, &QTimer::timeout, [&waiting] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box != nullptr && box == waiting) {
            ADD_FAILURE() << "Nothing answered: " << box->text().toStdString();
            box->reject();
        }
        waiting = box;
    });
    watchdog.start(2000);
    return RUN_ALL_TESTS();
}
