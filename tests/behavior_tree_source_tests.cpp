#include <gtest/gtest.h>

#include <engine/ai/BehaviorTreeSource.hpp>
#include <engine/assets/AssetCatalog.hpp>
#include <engine/core/Uuid.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

namespace {

using engine::ai::NodeKind;
using engine::ai::NodeSource;
using engine::ai::TreeSource;

TreeSource pulse() {
    return {"main",
            {{"main", NodeKind::Selector, {"pulse", "idle"}, {}, {}},
             {"pulse", NodeKind::Sequence, {"enabled", "wait"}, {}, {}},
             {"enabled", NodeKind::Condition, {}, "game/ai/enabled.condition.lua", {}},
             {"wait", NodeKind::Action, {}, "game/ai/pulse.action.lua", {{"seconds", 1.0}}},
             {"idle", NodeKind::Action, {}, "game/ai/idle.action.lua", {}}}};
}

std::string readError(std::string_view source) {
    TreeSource tree;
    std::string error;
    EXPECT_FALSE(engine::ai::ReadTreeSource(source, tree, error)) << source;
    EXPECT_TRUE(tree.nodes.empty());
    return error;
}

TEST(BehaviorTreeSource, ReadsTheDocumentedExample) {
    constexpr auto source = R"(
-- pulse.bt.lua
return {
    root = "main",
    nodes = {
        { id = "main", kind = "Selector", children = {"pulse", "idle"} },
        { id = "pulse", kind = "Sequence", children = {"enabled", "wait"} },
        { id = "enabled", kind = "Condition", script = "game/ai/enabled.condition.lua" },
        {
            id = "wait", kind = "Action", script = "game/ai/pulse.action.lua",
            parameters = { seconds = 1 },
        },
        { id = "idle", kind = "Action", script = "game/ai/idle.action.lua", note = "ignored" },
    },
}
)";
    TreeSource tree;
    std::string error;
    ASSERT_TRUE(engine::ai::ReadTreeSource(source, tree, error)) << error;
    EXPECT_EQ(tree, pulse());
}

TEST(BehaviorTreeSource, WritingThenReadingGivesTheTreeBack) {
    TreeSource tree = pulse();
    tree.nodes[3].parameters = {{"seconds", 0.25}, {"label", "say \"hi\"\n\ttab\\ \x01 \xd0\xb9"},
                                {"loud", true}, {"not an identifier", -3.0}, {"end", 1e21}};
    const std::string text = engine::ai::WriteTreeSource(tree);
    TreeSource read;
    std::string error;
    ASSERT_TRUE(engine::ai::ReadTreeSource(text, read, error)) << error << '\n' << text;
    EXPECT_EQ(read, tree);
    EXPECT_EQ(engine::ai::WriteTreeSource(read), text);
}

TEST(BehaviorTreeSource, WritesOneLinePerNode) {
    EXPECT_EQ(engine::ai::WriteTreeSource(pulse()),
              "-- Saved by the behavior tree editor, which rewrites this file: comments and formatting are not kept.\n"
              "return {\n"
              "    root = \"main\",\n"
              "    nodes = {\n"
              "        { id = \"main\", kind = \"Selector\", children = { \"pulse\", \"idle\" } },\n"
              "        { id = \"pulse\", kind = \"Sequence\", children = { \"enabled\", \"wait\" } },\n"
              "        { id = \"enabled\", kind = \"Condition\", script = \"game/ai/enabled.condition.lua\" },\n"
              "        { id = \"wait\", kind = \"Action\", script = \"game/ai/pulse.action.lua\", parameters = { seconds = 1 } },\n"
              "        { id = \"idle\", kind = \"Action\", script = \"game/ai/idle.action.lua\" },\n"
              "    },\n"
              "}\n");
}

TEST(BehaviorTreeSource, NonFiniteParametersFailOnReadInsteadOfVanishing) {
    TreeSource tree = pulse();
    tree.nodes[3].parameters["seconds"] = std::numeric_limits<double>::infinity();
    EXPECT_NE(readError(engine::ai::WriteTreeSource(tree)).find("seconds"), std::string::npos);
}

TEST(BehaviorTreeSource, RejectsDefinitionsThatAreNotTrees) {
    EXPECT_NE(readError("return 1").find("must be a table"), std::string::npos);
    EXPECT_NE(readError("return { nodes = {} }").find("root"), std::string::npos);
    EXPECT_NE(readError(R"(return { root = "a", nodes = { [1] = {}, [3] = {} } })").find("without gaps"),
              std::string::npos);
    EXPECT_NE(readError(R"(return { root = "a", nodes = { { id = "a", kind = "Parallel" } } })").find("Parallel"),
              std::string::npos);
    EXPECT_NE(readError(R"(return { root = "a", nodes = { { kind = "Action" } } })").find("Node 1: id"),
              std::string::npos);
    EXPECT_NE(readError(R"(return { root = "a", nodes = { { id = "a", kind = "Action", parameters = { t = {} } } } })")
                  .find("'t'"),
              std::string::npos);
    EXPECT_NE(readError("return {").find("definition:1:"), std::string::npos);
}

TEST(BehaviorTreeSource, RunsWithoutLibrariesAndWithinLimits) {
    EXPECT_NE(readError(R"(return require("x"))").find("require"), std::string::npos);
    EXPECT_NE(readError(R"(os.remove("x"))").find("os"), std::string::npos);
    EXPECT_NE(readError("while true do end").find("too long"), std::string::npos);
    EXPECT_NE(readError(R"(local s = "x" for i = 1, 40 do s = s .. s end)").find("memory"), std::string::npos);
    EXPECT_NE(readError("\x1bLua").find("binary"), std::string::npos);
}

/// A game root holding the pulse demo's three leaf scripts, cataloged.
struct Scripts {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("sri-bt-source-" + engine::NewUuid());
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

TEST(BehaviorTreeSource, ValidatesWhatTheRuntimeWouldLoad) {
    Scripts scripts;
    std::string error;
    EXPECT_TRUE(engine::ai::ValidateTreeSource(pulse(), scripts.catalog, error)) << error;
    const auto rejects = [&](const TreeSource& tree, const std::string_view expected) {
        EXPECT_FALSE(engine::ai::ValidateTreeSource(tree, scripts.catalog, error)) << expected;
        EXPECT_NE(error.find(expected), std::string::npos) << error;
    };

    TreeSource tree = pulse();
    tree.nodes[3].script = "game/ai/missing.action.lua";
    rejects(tree, "node 'wait': no script asset 'game/ai/missing.action.lua'");
    tree = pulse();
    tree.nodes[4].script.clear();
    rejects(tree, "node 'idle': no script");
    tree = pulse();
    tree.nodes[0].children.push_back("ghost");
    rejects(tree, "node 'main': no node 'ghost'");
    tree = pulse();
    tree.root = "nobody";
    rejects(tree, "root: no node 'nobody'");
    tree = pulse();
    tree.nodes[0].children = {"pulse"};
    rejects(tree, "unreachable");

    // The runtime also takes a script by UUID.
    tree = pulse();
    tree.nodes[4].script = scripts.catalog.FindPath("game/ai/idle.action.lua")->metadata.id.ToString();
    EXPECT_TRUE(engine::ai::ValidateTreeSource(tree, scripts.catalog, error)) << error;
}

}  // namespace
