#include "environment_panel.hpp"

#include <QAbstractItemView>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <limits>

#include <engine/engine.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <editor/asset_picker.hpp>
#include <editor/edit_actions.hpp>
#include <editor/editor_context.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_document.hpp>
#include <editor/scene_events.hpp>
#include <editor/scene_history.hpp>

#include "property_form.hpp"

namespace {
class EnvironmentPanel final : public QWidget {
public:
    explicit EnvironmentPanel(editor::EditorContext& context) : context_(context) {
        auto* layout = new QVBoxLayout(this);
        auto* note = new QLabel(tr("Background only; does not change scene lighting or exposure.\n"
                                  "Colours are nonnegative linear RGB; values above 1 are HDR."));
        note->setWordWrap(true);
        layout->addWidget(note);

        form_ = new PropertyForm;
        form_->setRecorder([this](const QString& label, const std::function<void()>& change) {
            if (!canEdit()) return;
            context_.history().edit(tr("Environment: %1").arg(label), change);
        });
        picker_ = new editor::AssetPicker(context.engine().GetSceneAssets().GetCatalog(),
                                          engine::CatalogAssetKind::Skybox, tr("None (fallback colour)"));
        picker_->setObjectName(QStringLiteral("environmentSkybox"));
        picker_->setMinimumContentsLength(12);
        form_->addRow(tr("Skybox"), picker_, [this] { refreshPicker(); });
        connect(picker_, &QComboBox::currentIndexChanged, this, [this] {
            // -1 means an unresolved reference, never an implicit request to clear it.
            if (!canEdit() || picker_->currentIndex() < 0) return;
            const auto reference = picker_->reference().value_or(engine::AssetReference{});
            context_.history().edit(tr("Environment: Skybox"), [this, reference] {
                auto value = context_.scene().GetEnvironment();
                value.skybox = reference;
                apply(value);
            });
        });

        const double maximum = std::numeric_limits<float>::max();
        form_->addAngle(tr("Yaw (degrees)"), binding(&engine::EnvironmentSettings::yawRadians),
                        {.step = 1, .decimals = 3, .minimum = -maximum, .maximum = maximum});
        form_->addFloat(tr("Brightness"), binding(&engine::EnvironmentSettings::brightness),
                        {.step = 0.1, .decimals = 4, .minimum = 0, .maximum = maximum});
        form_->addVec3(tr("Tint (linear RGB)"), binding(&engine::EnvironmentSettings::tint),
                       {.step = 0.05, .decimals = 4, .minimum = 0, .maximum = maximum});
        form_->addVec3(tr("Fallback (linear RGB)"), binding(&engine::EnvironmentSettings::fallbackColor),
                       {.step = 0.05, .decimals = 4, .minimum = 0, .maximum = maximum});
        form_->setToolTip(tr("Yaw rotates around world +Y. Fallback colour is used when no ready skybox exists, "
                            "without applying tint or brightness."));
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(form_);
        layout->addWidget(scroll);
        status_ = new QLabel;
        status_->setWordWrap(true);
        status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(status_);
        layout->addStretch();
        addActions({context.editActions().undoAction(), context.editActions().redoAction()});

        connect(&context.sceneEvents(), &editor::SceneEvents::changed, this, [this] { refresh(); });
        connect(&context.sceneDocument(), &editor::SceneDocument::opened, this, [this] { refresh(); });
        connect(&context.playSession(), &editor::PlaySession::playingChanged, this, [this](bool playing) {
            form_->setDisabled(playing);
            refresh();
        });
        // Catalog changes and asynchronous resource state are not scene changes.
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { refresh(); });
        timer->start(500);
        form_->setDisabled(!canEdit());
        refresh();
    }

private:
    bool canEdit() const {
        return !context_.playSession().isPlaying() && context_.history().isEnabled();
    }

    template <typename T>
    PropertyForm::Binding<T> binding(T engine::EnvironmentSettings::* member) {
        return {
            [this, member] { return &(context_.scene().GetEnvironment().*member); },
            [this, member](const T& field) {
                if (!canEdit()) return;
                // Start from live settings: display rounding and catalog refreshes must
                // preserve other fields, including unresolved UUIDs.
                auto value = context_.scene().GetEnvironment();
                value.*member = field;
                apply(value);
            }
        };
    }

    void apply(const engine::EnvironmentSettings& value) {
        std::string error;
        if (!context_.scene().SetEnvironment(value, error))
            context_.showStatus(tr("Environment: %1").arg(QString::fromStdString(error)), 5000);
    }

    void refreshPicker() {
        if (picker_->hasFocus() || picker_->lineEdit()->hasFocus() || picker_->view()->isVisible()) return;
        const QSignalBlocker blocker(picker_);
        picker_->reload();
        const auto& reference = context_.scene().GetEnvironment().skybox;
        if (reference.id.IsValid()) {
            if (!picker_->selectId(reference.id)) {
                picker_->setCurrentIndex(-1);
                picker_->setEditText(tr("Unresolved: %1 [%2]")
                    .arg(QString::fromStdString(reference.path), QString::fromStdString(reference.id.ToString())));
            }
        } else picker_->setCurrentIndex(0);
    }

    void refresh() {
        form_->setDisabled(!canEdit());
        form_->refresh();
        const auto& reference = context_.scene().GetEnvironment().skybox;
        auto& assets = context_.engine().GetSceneAssets();
        const auto* entry = assets.GetCatalog().Find(reference.id, engine::CatalogAssetKind::Skybox);
        QString message;
        if (reference.id.IsValid() && !entry) {
            message = tr("Skybox UUID is unresolved: %1. Reference preserved; showing fallback colour.")
                .arg(QString::fromStdString(reference.id.ToString()));
        } else if (entry) {
            switch (assets.GetState<engine::SkyboxAsset>(entry->path)) {
            case engine::AssetState::Failed:
                message = tr("Skybox failed to load. See the Output log for details; showing fallback colour.");
                break;
            case engine::AssetState::Ready: break;
            case engine::AssetState::Unloaded:
                message = tr("Skybox is unavailable. A failed load retires its request; see the Output log. "
                             "Showing fallback colour.");
                break;
            default: message = tr("Skybox is not ready; showing fallback colour."); break;
            }
        }
        status_->setText(message);
        status_->setVisible(!message.isEmpty());
    }

    editor::EditorContext& context_;
    PropertyForm* form_ = nullptr;
    editor::AssetPicker* picker_ = nullptr;
    QLabel* status_ = nullptr;
};
} // namespace

QWidget* createEnvironmentPanel(editor::EditorContext& context) {
    return new EnvironmentPanel(context);
}
