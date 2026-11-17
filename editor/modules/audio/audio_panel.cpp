#include "audio_panel.hpp"
#include <editor/editor_context.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_events.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/scene/Scene.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace audio_editor {
engine::AudioSubsystem* AudioPanel::audio() const { return context_.engine().GetSubsystem<engine::AudioSubsystem>(); }
AudioPanel::AudioPanel(editor::EditorContext& context) : context_(context) {
    auto* layout = new QVBoxLayout(this);
    title_ = new QLabel(tr("Double-click an AudioClip in Content Browser")); layout->addWidget(title_);
    metadata_ = new QLabel; metadata_->setWordWrap(true); layout->addWidget(metadata_);
    metadata_->setObjectName("AudioMetadata");
    auto* form = new QFormLayout; layout->addLayout(form);
    loading_ = new QComboBox; loading_->addItems({tr("Buffered"), tr("Streamed")}); form->addRow(tr("Audition loading"), loading_);
    loop_ = new QCheckBox(tr("Loop audition")); form->addRow(loop_);
    volume_ = new QSlider(Qt::Horizontal); volume_->setRange(0, 100);
    volume_->setValue(qBound(0, QSettings().value("audio/auditionVolume", 100).toInt(), 100));
    form->addRow(tr("Audition volume"), volume_);
    volume_->setObjectName("AuditionVolume");
    connect(volume_, &QSlider::valueChanged, this, [this](int value) {
        QSettings().setValue("audio/auditionVolume", value);
        if (auto* a = audio()) a->SetVolume(voice_, value / 100.f);
    });
    connect(loop_, &QCheckBox::toggled, this, [this](bool value) { if (auto* a = audio()) a->SetLooping(voice_, value); });
    auto* transport = new QHBoxLayout; layout->addLayout(transport);
    play_ = new QPushButton(tr("Play")); pause_ = new QPushButton(tr("Pause")); stop_ = new QPushButton(tr("Stop"));
    play_->setObjectName("AuditionPlay"); pause_->setObjectName("AuditionPause"); stop_->setObjectName("AuditionStop");
    transport->addWidget(play_); transport->addWidget(pause_); transport->addWidget(stop_);
    connect(play_, &QPushButton::clicked, this, [this] {
        if (auto* a = audio(); a && clip_ && !context_.engine().IsPlaying()) {
            engine::AudioPreviewOptions options;
            options.volume = volume_->value() / 100.f; options.looping = loop_->isChecked();
            options.loading = loading_->currentIndex() ? engine::AudioLoadingMode::Streamed : engine::AudioLoadingMode::Buffered;
            voice_ = a->Preview(*clip_, options, error_);
        }
        refresh();
    });
    connect(pause_, &QPushButton::clicked, this, [this] {
        if (auto* a = audio()) {
            if (a->GetStatus(voice_) == engine::AudioVoiceStatus::Paused) a->Resume(voice_); else a->Pause(voice_);
        }
        refresh();
    });
    connect(stop_, &QPushButton::clicked, this, [this] { stopOwned(); refresh(); });
    seek_ = new QSlider(Qt::Horizontal); seek_->setRange(0, 10000); layout->addWidget(seek_);
    time_ = new QLabel; layout->addWidget(time_);
    connect(seek_, &QSlider::sliderReleased, this, [this] {
        if (auto* a = audio()) if (const auto p = a->GetPlaybackPosition(voice_))
            a->Seek(voice_, p->durationSeconds * seek_->value() / 10000., error_);
    });
    status_ = new QLabel; status_->setWordWrap(true); layout->addWidget(status_);
    auto* mixerTitle = new QLabel(tr("Game mixer — transient runtime controls (not saved)")); layout->addWidget(mixerTitle);
    auto* mixer = new QFormLayout; layout->addLayout(mixer);
    const QString names[] = {tr("Master"), tr("Music"), tr("SFX"), tr("UI")};
    for (int i = 0; i < 4; ++i) {
        gains_[i] = new QSlider(Qt::Horizontal); gains_[i]->setRange(0, 100); mixer->addRow(names[i], gains_[i]);
        gains_[i]->setObjectName(QStringLiteral("GameGain%1").arg(i));
        connect(gains_[i], &QSlider::valueChanged, this, [this, i](int value) {
            if (auto* a = audio()) {
                if (!i) a->SetMasterVolume(value / 100.f);
                else a->SetBusVolume(static_cast<engine::AudioBus>(i - 1), value / 100.f);
            }
        });
    }
    diagnostic_ = new QLabel; diagnostic_->setWordWrap(true); layout->addWidget(diagnostic_);
    auto* retry = new QPushButton(tr("RetryDevice")); layout->addWidget(retry);
    connect(retry, &QPushButton::clicked, this, [this] { if (auto* a = audio()) (void)a->RetryDevice(error_); refresh(); });
    connect(&context.playSession(), &editor::PlaySession::playingChanged, this, [this](bool) { stopOwned(); refresh(); });
    auto* timer = new QTimer(this); timer->setInterval(100); connect(timer, &QTimer::timeout, this, [this] { refresh(); }); timer->start();
    layout->addStretch(); refresh();
}
AudioPanel::~AudioPanel() { stopOwned(); }
void AudioPanel::hideEvent(QHideEvent* event) { stopOwned(); QWidget::hideEvent(event); }
void AudioPanel::stopOwned() { if (auto* a = audio()) a->Stop(voice_); voice_ = {}; error_.clear(); }
void AudioPanel::open(const std::filesystem::path& path) {
    stopOwned(); clip_.reset();
    const auto& catalog = context_.engine().GetAssets().GetCatalog();
    for (const auto* entry : catalog.List(engine::CatalogAssetKind::AudioClip)) {
        if (entry->source == path) {
            clip_ = engine::AssetReference{entry->metadata.id, entry->path}; break;
        }
    }
    title_->setText(QString::fromStdWString(path.filename().wstring()));
    source_ = path;
    stamp_ = FileStamp::read(source_);
    probe_ = Probe::start(path); refresh();
}
void AudioPanel::refresh() {
    if (!source_.empty()) {
        const auto stamp = FileStamp::read(source_);
        if (stamp != stamp_) {
            // A catalog UUID/path can stay unchanged while an external editor replaces the file.
            // Cancel only this panel's old audition before exposing fresh metadata/Play controls.
            stopOwned();
            stamp_ = stamp;
            probe_ = Probe::start(source_);
        }
    }
    if (probe_) {
        if (!probe_->complete()) metadata_->setText(tr("Inspecting audio…"));
        else if (!probe_->error.empty()) metadata_->setText(tr("Invalid audio: %1").arg(QString::fromStdString(probe_->error)));
        else { const auto& i = probe_->info;
            metadata_->setText(tr("%1 s · %2 channels · %3 Hz · %4 bytes").arg(i.durationSeconds, 0, 'f', 3).arg(i.channels).arg(i.sampleRate).arg(qulonglong(i.fileBytes)));
        }
    }
    auto* a = audio();
    const auto status = a ? a->GetStatus(voice_) : engine::AudioVoiceStatus::Invalid;
    const bool ready = status == engine::AudioVoiceStatus::Playing || status == engine::AudioVoiceStatus::Paused;
    play_->setEnabled(a && a->IsAvailable() && clip_ && !context_.engine().IsPlaying() && probe_ && probe_->complete() && probe_->error.empty());
    pause_->setEnabled(ready); pause_->setText(status == engine::AudioVoiceStatus::Paused ? tr("Resume") : tr("Pause"));
    stop_->setEnabled(ready || status == engine::AudioVoiceStatus::Pending);
    const auto position = a ? a->GetPlaybackPosition(voice_) : std::nullopt;
    seek_->setEnabled(position && position->durationSeconds > 0);
    if (position) {
        time_->setText(tr("%1 / %2 s").arg(position->timeSeconds, 0, 'f', 2).arg(position->durationSeconds, 0, 'f', 2));
        if (!seek_->isSliderDown()) seek_->setValue(position->durationSeconds > 0 ? int(position->timeSeconds / position->durationSeconds * 10000) : 0);
    } else time_->clear();
    QString message = tr("Stopped");
    if (status == engine::AudioVoiceStatus::Pending) message = tr("Loading…");
    if (status == engine::AudioVoiceStatus::Playing) message = tr("Playing — raw 2D audition, independent of game mixer");
    if (status == engine::AudioVoiceStatus::Paused) message = tr("Paused");
    if (status == engine::AudioVoiceStatus::Finished) message = tr("Finished");
    if (status == engine::AudioVoiceStatus::Failed) message = QString::fromStdString(a->GetVoiceError(voice_));
    status_->setText(error_.empty() ? message : QString::fromStdString(error_));
    if (!a) { diagnostic_->setText(tr("Audio subsystem unavailable")); return; }
    for (int i = 0; i < 4; ++i) if (!gains_[i]->isSliderDown()) {
        const QSignalBlocker blocker(gains_[i]);
        gains_[i]->setValue(int((i ? a->GetBusVolume(static_cast<engine::AudioBus>(i - 1)) : a->GetMasterVolume()) * 100 + .5f));
    }
    const auto listener = a->GetEffectiveListener();
    const auto listenerText = a->GetListenerDiagnostic().empty() && context_.scene().IsValid(listener) ?
        QString::fromStdString(context_.scene().GetName(listener)) : QString::fromStdString(a->GetListenerDiagnostic());
    diagnostic_->setText(tr("Device: %1\nLive voices: %2\nListener: %3").arg(a->IsAvailable() ? tr("Available") : QString::fromStdString(a->GetDiagnostic())).arg(qulonglong(a->GetVoiceCount())).arg(listenerText));
}
}
