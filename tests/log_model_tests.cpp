#include <editor/log_model.hpp>

#include <QColor>
#include <QGuiApplication>
#include <QStringList>

#include <gtest/gtest.h>
#include <spdlog/logger.h>

#include <chrono>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

editor::LogEntry Entry(const QString& text, spdlog::level::level_enum level = spdlog::level::info) {
    return {std::chrono::system_clock::now(), level, {}, text};
}

std::vector<editor::LogEntry> Entries(std::initializer_list<const char*> texts) {
    std::vector<editor::LogEntry> entries;
    for (const char* text : texts) entries.push_back(Entry(QString::fromUtf8(text)));
    return entries;
}

QString Text(const editor::LogModel& model, int row) {
    return model.index(row, editor::LogModel::MessageColumn).data(editor::LogModel::TextRole).toString();
}

TEST(LogSink, KeepsLevelSourceAndTextUntilTaken) {
    auto sink = std::make_shared<editor::LogSink>();
    spdlog::logger logger("Lua", sink);
    logger.set_level(spdlog::level::trace);
    logger.warn("missing {}", "Script");
    logger.error("boom");

    const std::vector<editor::LogEntry> entries = sink->takePending();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].level, spdlog::level::warn);
    EXPECT_EQ(entries[0].source, QStringLiteral("Lua"));
    EXPECT_EQ(entries[0].text, QStringLiteral("missing Script"));
    EXPECT_EQ(entries[1].level, spdlog::level::err);
    EXPECT_TRUE(sink->takePending().empty());
}

TEST(LogModel, ShowsFirstLineAndKeepsWholeText) {
    editor::LogModel model;
    model.append({Entry(QStringLiteral("error: bad\r\nstack traceback:\n  main.lua:3"), spdlog::level::err)});

    const QModelIndex index = model.index(0, editor::LogModel::MessageColumn);
    const QString display = index.data(Qt::DisplayRole).toString();
    EXPECT_EQ(model.index(0, editor::LogModel::LevelColumn).data(Qt::DisplayRole).toString(), QStringLiteral("error"));
    EXPECT_TRUE(display == QStringLiteral("error: bad \u2026")) << display.toStdString();
    EXPECT_FALSE(display.contains(QStringLiteral("traceback")));
    EXPECT_EQ(Text(model, 0), QStringLiteral("error: bad\r\nstack traceback:\n  main.lua:3"));
    EXPECT_EQ(index.data(editor::LogModel::LevelRole).toInt(), static_cast<int>(spdlog::level::err));
    EXPECT_TRUE(editor::LogModel::format(model.entry(0)).contains(QStringLiteral("main.lua:3")));
}

TEST(LogModel, ColorsOnlyLevelsOtherThanInfo) {
    editor::LogModel model;
    model.append({Entry(QStringLiteral("a")), Entry(QStringLiteral("b"), spdlog::level::warn),
                  Entry(QStringLiteral("c"), spdlog::level::err), Entry(QStringLiteral("d"), spdlog::level::debug)});

    EXPECT_FALSE(model.index(0, 0).data(Qt::ForegroundRole).isValid());
    const QColor warning = model.index(1, 0).data(Qt::ForegroundRole).value<QColor>();
    const QColor error = model.index(2, 0).data(Qt::ForegroundRole).value<QColor>();
    EXPECT_TRUE(warning.isValid());
    EXPECT_TRUE(error.isValid());
    EXPECT_NE(warning, error);
    EXPECT_TRUE(model.index(3, 0).data(Qt::ForegroundRole).value<QColor>().isValid());
}

TEST(LogModel, DropsOldestPastCapacity) {
    editor::LogModel model(3);
    model.append(Entries({"1", "2"}));
    model.append(Entries({"3", "4"}));

    ASSERT_EQ(model.rowCount(), 3);
    EXPECT_EQ(Text(model, 0), QStringLiteral("2"));
    EXPECT_EQ(Text(model, 2), QStringLiteral("4"));
}

TEST(LogModel, KeepsTailOfBatchBiggerThanCapacity) {
    editor::LogModel model(2);
    model.append(Entries({"old"}));
    model.append(Entries({"1", "2", "3"}));

    ASSERT_EQ(model.rowCount(), 2);
    EXPECT_EQ(Text(model, 0), QStringLiteral("2"));
    EXPECT_EQ(Text(model, 1), QStringLiteral("3"));
}

TEST(LogModel, Clears) {
    editor::LogModel model;
    model.append(Entries({"1", "2"}));
    model.clear();
    EXPECT_EQ(model.rowCount(), 0);
}

TEST(LogModel, CountsLevelsThroughDropsAndClear) {
    editor::LogModel model(3);
    int changes = 0;
    QObject::connect(&model, &editor::LogModel::countsChanged, [&changes] { ++changes; });
    model.append({Entry(QStringLiteral("1"), spdlog::level::err), Entry(QStringLiteral("2")),
                  Entry(QStringLiteral("3"), spdlog::level::warn)});
    EXPECT_EQ(model.count(spdlog::level::err), 1);
    EXPECT_EQ(model.count(spdlog::level::info), 1);

    model.append({Entry(QStringLiteral("4"), spdlog::level::warn)});
    EXPECT_EQ(model.count(spdlog::level::err), 0);
    EXPECT_EQ(model.count(spdlog::level::warn), 2);

    model.clear();
    EXPECT_EQ(model.count(spdlog::level::warn), 0);
    EXPECT_EQ(changes, 3);
}

TEST(LogFilterModel, HidesLevelsAndMatchesTextIgnoringCase) {
    editor::LogModel model;
    editor::LogFilterModel filter;
    filter.setSourceModel(&model);
    editor::LogEntry lua = Entry(QStringLiteral("script reloaded"));
    lua.source = QStringLiteral("Lua");
    model.append({Entry(QStringLiteral("Loading scene"), spdlog::level::info),
                  Entry(QStringLiteral("missing mesh"), spdlog::level::warn),
                  Entry(QStringLiteral("scene failed"), spdlog::level::err), std::move(lua)});
    ASSERT_EQ(filter.rowCount(), 4);

    filter.setLevelShown(spdlog::level::info, false);
    ASSERT_EQ(filter.rowCount(), 2);
    EXPECT_EQ(filter.index(0, 0).data(editor::LogModel::TextRole).toString(), QStringLiteral("missing mesh"));

    filter.setLevelShown(spdlog::level::info, true);
    filter.setSearchText(QStringLiteral("SCENE"));
    ASSERT_EQ(filter.rowCount(), 2);
    EXPECT_EQ(filter.index(1, 0).data(editor::LogModel::TextRole).toString(), QStringLiteral("scene failed"));

    filter.setSearchText(QStringLiteral("lua"));
    ASSERT_EQ(filter.rowCount(), 1);

    filter.setSearchText({});
    model.append({Entry(QStringLiteral("late"), spdlog::level::info)});
    EXPECT_EQ(filter.rowCount(), 5);
}

editor::LogEntry FromSource(const char* source, const char* text) {
    editor::LogEntry entry = Entry(QString::fromUtf8(text));
    entry.source = QString::fromUtf8(source);
    return entry;
}

TEST(LogModel, ListsSourcesSortedAndKeepsThemThroughClear) {
    editor::LogModel model;
    int changes = 0;
    QObject::connect(&model, &editor::LogModel::sourcesChanged, [&changes] { ++changes; });
    model.append({FromSource("Render", "a"), FromSource("Lua", "b"), FromSource("Render", "c")});
    model.append({FromSource("Lua", "d")});
    EXPECT_EQ(model.sources(), QStringList({QStringLiteral("Lua"), QStringLiteral("Render")}));
    EXPECT_EQ(changes, 1);

    model.clear();
    EXPECT_EQ(model.sources().size(), 2);
}

TEST(LogFilterModel, HidesSourcesIncludingOnesNotSeenYet) {
    editor::LogModel model;
    editor::LogFilterModel filter;
    filter.setSourceModel(&model);
    filter.setSourceShown(QStringLiteral("Physics"), false);
    model.append({FromSource("Lua", "a"), FromSource("Physics", "b"), FromSource("", "c")});
    ASSERT_EQ(filter.rowCount(), 2);

    filter.setSourceShown(QStringLiteral(""), false);
    ASSERT_EQ(filter.rowCount(), 1);
    EXPECT_EQ(filter.index(0, 0).data(editor::LogModel::SourceRole).toString(), QStringLiteral("Lua"));

    filter.setSourceShown(QStringLiteral("Physics"), true);
    EXPECT_EQ(filter.rowCount(), 2);
    EXPECT_EQ(filter.hiddenSources(), QStringList({QString()}));
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
