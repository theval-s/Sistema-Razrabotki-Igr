#include "inspector_panel.hpp"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <string>
#include <utility>

#include <DirectXMath.h>

#include <engine/scene/Scene.hpp>

#include <editor/scene_document.hpp>
#include <editor/selection.hpp>

namespace {

using DirectX::SimpleMath::Quaternion;
using DirectX::SimpleMath::Vector3;

Vector3 read(const std::array<QDoubleSpinBox*, 3>& boxes) {
    return {static_cast<float>(boxes[0]->value()), static_cast<float>(boxes[1]->value()),
            static_cast<float>(boxes[2]->value())};
}

/// Leaves a box the user is typing into alone.
void showVec3(const std::array<QDoubleSpinBox*, 3>& boxes, const Vector3& value) {
    const float components[3] = {value.x, value.y, value.z};
    for (int i = 0; i < 3; ++i) {
        if (!boxes[i]->hasFocus()) {
            const QSignalBlocker blocker(boxes[i]);
            boxes[i]->setValue(components[i]);
        }
    }
}

}  // namespace

InspectorPanel::InspectorPanel(engine::Scene& scene, editor::Selection& selection, editor::SceneDocument& document,
                               QWidget* parent)
    : QWidget(parent), scene_(scene), document_(document) {
    form_ = new QWidget(this);
    auto* form = new QFormLayout(form_);
    name_ = new QLineEdit(form_);
    form->addRow(tr("Name"), name_);
    position_ = makeVec3Row(0.1, 3);
    rotation_ = makeVec3Row(1.0, 2);
    scale_ = makeVec3Row(0.1, 3);
    for (auto [label, boxes] : {std::pair{tr("Position"), position_}, std::pair{tr("Rotation"), rotation_},
                                std::pair{tr("Scale"), scale_}}) {
        auto* row = new QHBoxLayout;
        for (QDoubleSpinBox* box : boxes) {
            row->addWidget(box);
        }
        form->addRow(label, row);
    }

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(form_);
    layout->addStretch();

    connect(name_, &QLineEdit::editingFinished, this, &InspectorPanel::applyName);

    // Same reason as the hierarchy: the scene has no change notifications.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &InspectorPanel::refresh);
    timer->start(100);

    // Only on a new primary: re-showing the same entity would reset the rotation angles being edited.
    connect(&selection, &editor::Selection::changed, this, [this, &selection] {
        if (selection.primary() != entity_) {
            setEntity(selection.primary());
        }
    });
    setEntity(selection.primary());
}

void InspectorPanel::setEntity(const entt::entity entity) {
    entity_ = entity;
    // Force a full refresh, rotation included.
    shown_.rotation = Quaternion{0.0f, 0.0f, 0.0f, 0.0f};
    refresh();
}

InspectorPanel::Vec3Boxes InspectorPanel::makeVec3Row(const double step, const int decimals) {
    Vec3Boxes boxes{};
    for (QDoubleSpinBox*& box : boxes) {
        box = new QDoubleSpinBox(form_);
        box->setRange(-1.0e6, 1.0e6);
        box->setDecimals(decimals);
        box->setSingleStep(step);
        box->setAccelerated(true);
        box->setMinimumWidth(60);
        connect(box, &QDoubleSpinBox::valueChanged, this, &InspectorPanel::applyTransform);
    }
    return boxes;
}

void InspectorPanel::refresh() {
    if (entity_ != entt::null && !scene_.IsValid(entity_)) {
        entity_ = entt::null;  // destroyed meanwhile
    }
    form_->setEnabled(entity_ != entt::null);
    if (entity_ == entt::null) {
        const QSignalBlocker blocker(name_);
        name_->clear();
        return;
    }

    if (!name_->hasFocus()) {
        name_->setText(QString::fromStdString(scene_.GetName(entity_)));
    }

    const engine::Transform& transform = scene_.GetTransform(entity_);
    showVec3(position_, transform.position);
    showVec3(scale_, transform.scale);
    if (transform.rotation != shown_.rotation) {
        // ToEuler() is (pitch, yaw, roll), i.e. about X, Y, Z: the order CreateFromYawPitchRoll takes back.
        const Vector3 euler = transform.rotation.ToEuler();
        showVec3(rotation_, {DirectX::XMConvertToDegrees(euler.x), DirectX::XMConvertToDegrees(euler.y),
                             DirectX::XMConvertToDegrees(euler.z)});
    }
    shown_ = transform;
}

void InspectorPanel::applyName() {
    if (entity_ != entt::null && scene_.IsValid(entity_)) {
        const std::string name = name_->text().toStdString();
        if (name != scene_.GetName(entity_)) {
            scene_.SetName(entity_, name);
        }
    }
}

void InspectorPanel::applyTransform() {
    if (entity_ == entt::null || !scene_.IsValid(entity_)) {
        return;
    }
    const Vector3 degrees = read(rotation_);
    engine::Transform& transform = scene_.GetTransform(entity_);
    transform.position = read(position_);
    transform.scale = read(scale_);
    transform.rotation = Quaternion::CreateFromYawPitchRoll(Vector3{DirectX::XMConvertToRadians(degrees.x),
                                                                    DirectX::XMConvertToRadians(degrees.y),
                                                                    DirectX::XMConvertToRadians(degrees.z)});
    shown_ = transform;
    document_.markDirty();
}
