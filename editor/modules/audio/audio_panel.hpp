#pragma once
#include "audio_probe.hpp"
#include <QWidget>
#include <array>
#include <optional>

class QLabel;
class QSlider;
class QCheckBox;
class QComboBox;
class QPushButton;
class QHideEvent;
namespace editor { class EditorContext; }

namespace audio_editor {
class AudioPanel final : public QWidget {
public:
    explicit AudioPanel(editor::EditorContext& context);
    ~AudioPanel() override;
    void open(const std::filesystem::path& path);
    void stopOwned();
protected:
    void hideEvent(QHideEvent* event) override;
private:
    engine::AudioSubsystem* audio() const;
    void refresh();
    editor::EditorContext& context_;
    std::optional<engine::AssetReference> clip_;
    engine::AudioVoiceHandle voice_;
    std::shared_ptr<Probe> probe_;
    std::filesystem::path source_;
    FileStamp stamp_;
    std::string error_;
    QLabel *title_, *metadata_, *status_, *time_, *diagnostic_;
    QPushButton *play_, *pause_, *stop_;
    QCheckBox* loop_;
    QComboBox* loading_;
    QSlider *volume_, *seek_;
    std::array<QSlider*, 4> gains_{};
};
}
