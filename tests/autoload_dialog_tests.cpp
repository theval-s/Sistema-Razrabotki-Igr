#include <editor/autoload_dialog.hpp>

#include <QApplication>
#include <QPushButton>
#include <QTableWidget>

#include <engine/engine.hpp>
#include <engine/project/Project.hpp>
#include <engine/scripting/LuaRuntime.hpp>

#include <gtest/gtest.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

engine::Project SampleProject() {
    engine::Project project;
    project.autoloads = {
        {"First", {engine::NewUuid(), "game/first.lua"}, true},
        {"Second", {engine::NewUuid(), "game/second.lua"}, false},
    };
    return project;
}

TEST(AutoloadDialog, CancelPreservesProjectAndReorderingPreservesEditsAndReferences) {
    engine::Engine engine;
    const auto project = SampleProject();
    editor::AutoloadDialog dialog(engine, project);
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("AutoloadTable"));
    auto* down = dialog.findChild<QPushButton*>(QStringLiteral("MoveAutoloadDown"));
    ASSERT_NE(table, nullptr);
    ASSERT_NE(down, nullptr);
    ASSERT_EQ(table->rowCount(), 2);
    table->item(0, 1)->setText(QStringLiteral("Renamed"));
    table->item(0, 0)->setCheckState(Qt::Unchecked);
    table->selectRow(0);
    down->click();
    const auto edited = dialog.definitions();
    ASSERT_EQ(edited.size(), 2u);
    EXPECT_EQ(edited[0].name, "Second");
    EXPECT_EQ(edited[1].name, "Renamed");
    EXPECT_FALSE(edited[1].enabled);
    EXPECT_EQ(edited[1].script.id, project.autoloads[0].script.id);
    dialog.reject();
    EXPECT_EQ(dialog.result(), QDialog::Rejected);
    EXPECT_EQ(project.autoloads[0].name, "First");
    EXPECT_TRUE(project.autoloads[0].enabled);
}

TEST(AutoloadDialog, RemoveAndSaveOnlyChangeTheDialogCopy) {
    engine::Engine engine;
    const auto project = SampleProject();
    editor::AutoloadDialog dialog(engine, project);
    auto* table = dialog.findChild<QTableWidget*>(QStringLiteral("AutoloadTable"));
    auto* remove = dialog.findChild<QPushButton*>(QStringLiteral("RemoveAutoload"));
    ASSERT_NE(table, nullptr);
    ASSERT_NE(remove, nullptr);
    table->selectRow(0);
    remove->click();
    dialog.accept();
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    const auto edited = dialog.definitions();
    ASSERT_EQ(edited.size(), 1u);
    EXPECT_EQ(edited[0].name, "Second");
    EXPECT_EQ(edited[0].script.id, project.autoloads[1].script.id);
    EXPECT_EQ(project.autoloads.size(), 2u);
}

TEST(AutoloadLogging, LuaUsesTheSharedDefaultLogger) {
    std::vector<std::string> messages;
    const auto logger = spdlog::default_logger();
    const auto previousLevel = logger->level();
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg& message) {
        messages.emplace_back(message.payload.data(), message.payload.size());
    });
    struct RestoreLogger {
        std::shared_ptr<spdlog::logger> logger;
        std::shared_ptr<spdlog::sinks::sink> sink;
        spdlog::level::level_enum level;
        ~RestoreLogger() { std::erase(logger->sinks(), sink); logger->set_level(level); }
    } restore{logger, sink, previousLevel};
    logger->sinks().push_back(sink);
    logger->set_level(spdlog::level::info);
    engine::LuaRuntime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Initialize(error)) << error;
    ASSERT_TRUE(runtime.ExecuteString("Log.info('Hello world')", "logging-test", error)) << error;
    EXPECT_EQ(std::count(messages.begin(), messages.end(), "[Lua] Hello world"), 1);
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
