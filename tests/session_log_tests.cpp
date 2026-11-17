#include <engine/core/Log.hpp>
#include <editor/session_log.hpp>
#include "../editor/src/session_log_detail.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef SRI_ENGINE_VERSION
#error session_log_tests must receive the same SRI_ENGINE_VERSION as the editor
#endif

namespace {
class RecordingSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> messages;
private:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        messages.emplace_back(msg.payload.data(), msg.payload.size());
    }
    void flush_() override {}
};
}

TEST(SessionLog, NativeUnicodePathRotatesAndFlushesErrors) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto directory = std::filesystem::path(temp.path().toStdWString()) / std::filesystem::path(L"\u0436\u0443\u0440\u043d\u0430\u043b");
    const auto path = directory / L"editor.log";
    ASSERT_TRUE(engine::log::AddFileSink(path, 160, 2));
    auto sink = engine::log::Sinks()->sinks().back();

    auto& logger = engine::log::Channel("SessionLogTest");
    for (int i = 0; i < 20; ++i) logger.error("rotation marker {} with enough text to fill a small file", i);

    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(directory / L"editor.1.log"));
    EXPECT_TRUE(std::filesystem::exists(directory / L"editor.2.log"));
    EXPECT_FALSE(std::filesystem::exists(directory / L"editor.3.log"));
    std::ifstream file(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("rotation marker 19"), std::string::npos);
    file.close();
    engine::log::Sinks()->remove_sink(sink);
    sink.reset();
}

TEST(SessionLog, RejectsUnusableFilePath) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto directory = std::filesystem::path(temp.path().toStdWString());
    const auto blocker = directory / L"not-a-directory";
    { std::ofstream file(blocker); file << "block"; }
    EXPECT_FALSE(engine::log::AddFileSink(blocker / L"editor.log"));
}

TEST(SessionLog, RotationFailureReportsOnceAndPreservesDownstreamMessages) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const auto directory = std::filesystem::path(temp.path().toStdWString());
    const auto path = directory / L"editor.log";
    ASSERT_TRUE(engine::log::AddFileSink(path, 128, 1));
    auto fileSink = engine::log::Sinks()->sinks().back();
    auto downstream = std::make_shared<RecordingSink>();
    engine::log::Sinks()->add_sink(downstream);

    auto& logger = engine::log::Channel("SessionLogFailureTest");
    logger.info("initial entry");
    const auto blocker = directory / L"editor.1.log";
    std::filesystem::create_directory(blocker);
    { std::ofstream child(blocker / L"child"); child << "block removal"; }

    testing::internal::CaptureStderr();
    logger.error("first failure {}", std::string(200, 'x'));
    logger.error("second message after failure");
    const std::string report = testing::internal::GetCapturedStderr();
    EXPECT_NE(report.find("File logging disabled:"), std::string::npos);
    EXPECT_EQ(report.find("File logging disabled:", report.find("File logging disabled:") + 1), std::string::npos);
    ASSERT_EQ(downstream->messages.size(), 3u);
    EXPECT_NE(downstream->messages[1].find("first failure"), std::string::npos);
    EXPECT_EQ(downstream->messages[2], "second message after failure");

    engine::log::Sinks()->remove_sink(fileSink);
    engine::log::Sinks()->remove_sink(downstream);
    fileSink.reset();
}

TEST(SessionLog, InitializationFallsBackOnceAndRecordsSessionDetails) {
    int argc = 1;
    char name[] = "session_log_tests";
    char* argv[] = {name, nullptr};
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString blocker = QDir(temp.path()).filePath(QStringLiteral("blocker"));
    QFile blockingFile(blocker);
    ASSERT_TRUE(blockingFile.open(QIODevice::WriteOnly));
    blockingFile.close();
    const QString preferred = blocker + QStringLiteral("/editor.log");
    const QString fallback = QDir(temp.path()).filePath(QStringLiteral("fallback/\u0436\u0443\u0440\u043d\u0430\u043b.log"));
    const auto before = engine::log::Sinks()->sinks().size();
    editor::detail::initializeSessionLog(preferred, fallback);
    EXPECT_EQ(editor::activeLogFile(), fallback);
    EXPECT_TRUE(editor::fileLogError().contains(preferred));
    EXPECT_TRUE(editor::fileLogError().contains(fallback));
    ASSERT_EQ(engine::log::Sinks()->sinks().size(), before + 1);
    auto fileSink = engine::log::Sinks()->sinks().back();

    editor::initializeSessionLog();
    editor::detail::initializeSessionLog(preferred, fallback);
    EXPECT_EQ(engine::log::Sinks()->sinks().size(), before + 1);
    EXPECT_EQ(editor::activeLogFile(), fallback);
    const QString project = QStringLiteral("C:/\u0438\u0433\u0440\u0430/project.sriproject");
    editor::logSession(project);
    QFile log(fallback);
    ASSERT_TRUE(log.open(QIODevice::ReadOnly));
    const QByteArray text = log.readAll();
    EXPECT_EQ(text.count("========== Editor session started =========="), 1);
    EXPECT_EQ(text.count("Engine version: " SRI_ENGINE_VERSION), 1);
    EXPECT_TRUE(text.contains(QCoreApplication::applicationFilePath().toUtf8()));
    EXPECT_TRUE(text.contains(project.toUtf8()));
    log.close();
    engine::log::Sinks()->remove_sink(fileSink);
    fileSink.reset();
}
