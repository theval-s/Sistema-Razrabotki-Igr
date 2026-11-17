#include "audio_panel.hpp"
#include <editor/editor_context.hpp>
#include <editor/paths.hpp>
#include <editor/viewport_tool_bar.hpp>
#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/core/FileSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>
#include <DockManager.h>
#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>
#include <chrono>
#include <vector>

namespace {
class AudioEditorTests : public testing::Test {
protected:
    static std::string Wave(unsigned channels = 1, unsigned frames = 4800) {
        std::string wave;
        const auto u16 = [&](unsigned n) { wave += char(n & 255); wave += char((n >> 8) & 255); };
        const auto u32 = [&](unsigned n) { u16(n & 65535); u16(n >> 16); };
        const auto bytes = frames * channels * 2;
        wave += "RIFF"; u32(36 + bytes); wave += "WAVEfmt "; u32(16);
        u16(1); u16(channels); u32(48000); u32(48000 * channels * 2);
        u16(channels * 2); u16(16); wave += "data"; u32(bytes);
        for (unsigned i = 0; i < frames; ++i)
            for (unsigned channel = 0; channel < channels; ++channel) u16(i % 96 < 48 ? 4096 : unsigned(-4096));
        return wave;
    }
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        previousRoot = engine::AssetPathResolver::GetGameRoot();
        project.file = editor::toPath(directory.filePath("audio.sriproject"));
        std::filesystem::create_directories(project.ContentDir());
        engine::AssetPathResolver::SetGameRoot(project.ContentDir());
        file = project.ContentDir() / "tone.wav";
        ASSERT_TRUE(engine::CreateNewFile(file, Wave(), error)) << error;
        ASSERT_TRUE(engine::WriteAssetMetadata(file, {engine::NewUuid(), engine::CatalogAssetKind::AudioClip}, error)) << error;
        audio = host.AddSubsystem<engine::AudioSubsystem>(engine::AudioConfig{.output = engine::AudioOutputMode::Offline});
        host.SetEditorMode();
        ASSERT_TRUE(host.Initialize());
        ASSERT_TRUE(host.GetAssets().RescanCatalog(error)) << error;
        ASSERT_TRUE(audio->IsAvailable());
        QSettings().remove("audio/auditionVolume");
    }
    void TearDown() override {
        host.Shutdown();
        engine::AssetPathResolver::SetGameRoot(previousRoot);
        QSettings().remove("audio/auditionVolume");
    }
    QTemporaryDir directory;
    engine::Engine host{engine::EngineConfig{.taskThreads = 1}};
    engine::Project project;
    QMainWindow window;
    ads::CDockManager docks{&window};
    QWidget viewport;
    editor::ViewportToolBar toolbar;
    editor::EditorContext context{host, project, window, docks, viewport, toolbar};
    engine::AudioSubsystem* audio = nullptr;
    std::filesystem::path previousRoot, file;
    std::string error;
};

TEST_F(AudioEditorTests, AsyncMetadataWorksWithoutDeviceAndOldPanelCannotStopNewAudition) {
    auto first = std::make_unique<audio_editor::AudioPanel>(context);
    first->open(file);
    auto* play = first->findChild<QPushButton*>("AuditionPlay");
    ASSERT_NE(play, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return play->isEnabled(); }));
    auto* label = first->findChild<QLabel*>("AudioMetadata");
    ASSERT_NE(label, nullptr);
    EXPECT_TRUE(label->text().contains("48000"));
    EXPECT_TRUE(label->text().contains("1 channels"));
    play->click();
    ASSERT_EQ(audio->GetVoiceCount(), 1u);
    audio_editor::AudioPanel second(context);
    second.open(file);
    auto* otherPlay = second.findChild<QPushButton*>("AuditionPlay");
    ASSERT_TRUE(QTest::qWaitFor([&] { return otherPlay->isEnabled(); }));
    otherPlay->click();
    first.reset(); // Delayed old-form destruction must target only its stale token.
    EXPECT_EQ(audio->GetVoiceCount(), 1u);
    second.show(); QCoreApplication::processEvents(); second.hide();
    EXPECT_EQ(audio->GetVoiceCount(), 0u);
}

TEST_F(AudioEditorTests, AuditionPreferenceIsSeparateFromTransientPolledGameMixer) {
    {
        audio_editor::AudioPanel panel(context);
        panel.findChild<QSlider*>("AuditionVolume")->setValue(37);
        panel.findChild<QSlider*>("GameGain0")->setValue(20);
        EXPECT_FLOAT_EQ(audio->GetMasterVolume(), .2f);
        audio->SetMasterVolume(.64f);
        ASSERT_TRUE(QTest::qWaitFor([&] { return panel.findChild<QSlider*>("GameGain0")->value() == 64; }));
    }
    audio_editor::AudioPanel replacement(context);
    EXPECT_EQ(replacement.findChild<QSlider*>("AuditionVolume")->value(), 37);
    EXPECT_EQ(replacement.findChild<QSlider*>("GameGain0")->value(), 64);
    EXPECT_EQ(QSettings().value("audio/auditionVolume").toInt(), 37);
}

TEST_F(AudioEditorTests, SamePathReplacementsInvalidateMetadataAndStopOwnedAudition) {
    audio_editor::AudioPanel panel(context);
    panel.open(file);
    auto* play = panel.findChild<QPushButton*>("AuditionPlay");
    auto* label = panel.findChild<QLabel*>("AudioMetadata");
    ASSERT_NE(play, nullptr);
    ASSERT_NE(label, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return play->isEnabled(); }));
    play->click();
    std::vector<float> samples(128);
    ASSERT_TRUE(audio->MixOffline(samples, error)) << error;
    ASSERT_EQ(audio->GetVoiceCount(), 1u);

    // Same byte count, changed mtime: the new native channel count must be inspected.
    const auto originalStamp = audio_editor::FileStamp::read(file);
    ASSERT_TRUE(engine::WriteFileAtomic(file, Wave(2, 2400), error)) << error;
    std::filesystem::last_write_time(file, originalStamp.modified + std::chrono::seconds(2));
    EXPECT_EQ(audio_editor::FileStamp::read(file).bytes, originalStamp.bytes);
    ASSERT_TRUE(QTest::qWaitFor([&] { return audio->GetVoiceCount() == 0 && label->text().contains("2 channels"); }));
    ASSERT_TRUE(play->isEnabled());
    play->click();
    ASSERT_TRUE(audio->MixOffline(samples, error)) << error;
    ASSERT_EQ(audio->GetVoiceCount(), 1u);

    // Changed byte count, unchanged mtime: size alone must also invalidate.
    const auto stereoStamp = audio_editor::FileStamp::read(file);
    ASSERT_TRUE(engine::WriteFileAtomic(file, Wave(1, 2400), error)) << error;
    std::filesystem::last_write_time(file, stereoStamp.modified);
    ASSERT_TRUE(QTest::qWaitFor([&] { return audio->GetVoiceCount() == 0 && label->text().contains("1 channels") && label->text().contains("4844 bytes"); }));
    EXPECT_EQ(audio_editor::FileStamp::read(file).modified, stereoStamp.modified);
}
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("SRI-tests");
    QCoreApplication::setApplicationName("audio-editor");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
