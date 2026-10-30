#include "property_form.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QToolButton>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include <DirectXMath.h>

namespace {

using DirectX::SimpleMath::Quaternion;
using DirectX::SimpleMath::Vector3;
using Vec3Boxes = std::array<QDoubleSpinBox*, 3>;

// we might need a separate file for all the types of field we might add
//as this is A LOT of boilerplate
QDoubleSpinBox* makeSpinBox(QWidget* parent, const PropertyForm::NumberFormat& format) {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(format.minimum, format.maximum);
    box->setDecimals(format.decimals);
    box->setSingleStep(format.step);
    box->setAccelerated(true);
    box->setMinimumWidth(60);
    return box;
}

/// Leaves a box the user is typing into alone.
void showValue(QDoubleSpinBox* box, const double value) {
    if (!box->hasFocus()) {
        const QSignalBlocker blocker(box);
        box->setValue(value);
    }
}

Vector3 read(const Vec3Boxes& boxes) {
    return {static_cast<float>(boxes[0]->value()), static_cast<float>(boxes[1]->value()),
            static_cast<float>(boxes[2]->value())};
}

void showVec3(const Vec3Boxes& boxes, const Vector3& value) {
    showValue(boxes[0], value.x);
    showValue(boxes[1], value.y);
    showValue(boxes[2], value.z);
}

/// Three boxes in a row; returns the row's layout through `row`.
Vec3Boxes makeVec3Boxes(QWidget* parent, const PropertyForm::NumberFormat& format, QHBoxLayout*& row) {
    row = new QHBoxLayout;
    Vec3Boxes boxes{};
    for (QDoubleSpinBox*& box : boxes) {
        box = makeSpinBox(parent, format);
        row->addWidget(box);
    }
    return boxes;
}

QColor toColor(const Vector3& rgb) {
    return QColor::fromRgbF(std::clamp(rgb.x, 0.0f, 1.0f), std::clamp(rgb.y, 0.0f, 1.0f),
                            std::clamp(rgb.z, 0.0f, 1.0f));
}

}  // namespace

PropertyForm::PropertyForm(std::function<void()> edited, QWidget* parent)
    : QWidget(parent), layout_(new QFormLayout(this)), edited_(std::move(edited)) {}

void PropertyForm::addFloat(const QString& label, Getter<float> value, const NumberFormat format) {
    addScaledFloat(label, std::move(value), format, 1.0);
}

void PropertyForm::addAngle(const QString& label, Getter<float> value, const NumberFormat format) {
    addScaledFloat(label, std::move(value), format, DirectX::XMConvertToDegrees(1.0f));
}

void PropertyForm::addScaledFloat(const QString& label, Getter<float> value, const NumberFormat format,
                                  const double scale) {
    QDoubleSpinBox* box = makeSpinBox(this, format);
    layout_->addRow(label, box);
    connect(box, &QDoubleSpinBox::valueChanged, this, [this, value, scale](const double shown) {
        if (float* target = value()) {
            *target = static_cast<float>(shown / scale);
            edited_();
        }
    });
    refreshers_.push_back([box, value, scale] {
        if (const float* current = value()) {
            showValue(box, *current * scale);
        }
    });
}

void PropertyForm::addVec3(const QString& label, Getter<Vector3> value, const NumberFormat format) {
    QHBoxLayout* row = nullptr;
    const Vec3Boxes boxes = makeVec3Boxes(this, format, row);
    layout_->addRow(label, row);
    for (QDoubleSpinBox* box : boxes) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, value, boxes] {
            if (Vector3* target = value()) {
                *target = read(boxes);
                edited_();
            }
        });
    }
    refreshers_.push_back([boxes, value] {
        if (const Vector3* current = value()) {
            showVec3(boxes, *current);
        }
    });
}

void PropertyForm::addRotation(const QString& label, Getter<Quaternion> value) {
    QHBoxLayout* row = nullptr;
    const Vec3Boxes boxes = makeVec3Boxes(this, {.step = 1.0, .decimals = 2}, row);
    layout_->addRow(label, row);
    // What the boxes show. Never a rotation to begin with, so the first refresh shows it.
    auto shown = std::make_shared<Quaternion>(0.0f, 0.0f, 0.0f, 0.0f);
    for (QDoubleSpinBox* box : boxes) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, value, boxes, shown] {
            if (Quaternion* target = value()) {
                const Vector3 degrees = read(boxes);
                *target = Quaternion::CreateFromYawPitchRoll(Vector3{DirectX::XMConvertToRadians(degrees.x),
                                                                     DirectX::XMConvertToRadians(degrees.y),
                                                                     DirectX::XMConvertToRadians(degrees.z)});
                *shown = *target;
                edited_();
            }
        });
    }
    refreshers_.push_back([boxes, value, shown] {
        const Quaternion* current = value();
        if (current == nullptr || *current == *shown) {
            return;
        }
        // ToEuler() is (pitch, yaw, roll), i.e. about X, Y, Z: the order CreateFromYawPitchRoll takes back.
        const Vector3 euler = current->ToEuler();
        showVec3(boxes, {DirectX::XMConvertToDegrees(euler.x), DirectX::XMConvertToDegrees(euler.y),
                         DirectX::XMConvertToDegrees(euler.z)});
        *shown = *current;
    });
}

void PropertyForm::addColor(const QString& label, Getter<Vector3> value) {
    QHBoxLayout* row = nullptr;
    const Vec3Boxes boxes = makeVec3Boxes(this, {.step = 0.05, .decimals = 3, .minimum = 0.0, .maximum = 1.0e3}, row);
    auto* swatch = new QToolButton(this);
    swatch->setToolTip(tr("Pick a color"));
    swatch->setFixedWidth(28);
    row->addWidget(swatch);
    layout_->addRow(label, row);

    for (QDoubleSpinBox* box : boxes) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, value, boxes] {
            if (Vector3* target = value()) {
                *target = read(boxes);
                edited_();
            }
        });
    }
    connect(swatch, &QToolButton::clicked, this, [this, value, label] {
        const Vector3* current = value();
        if (current == nullptr) {
            return;
        }
        // thank god we're using Qt
        const QColor picked = QColorDialog::getColor(toColor(*current), this, label);
        // Asked again: the entity may be gone after the dialog's event loop.
        if (Vector3* target = value(); target != nullptr && picked.isValid()) {
            *target = Vector3{static_cast<float>(picked.redF()), static_cast<float>(picked.greenF()),
                              static_cast<float>(picked.blueF())};
            edited_();
        }
    });
    auto shown = std::make_shared<QColor>();
    refreshers_.push_back([boxes, value, swatch, shown] {
        const Vector3* current = value();
        if (current == nullptr) {
            return;
        }
        showVec3(boxes, *current);
        if (const QColor color = toColor(*current); color != *shown) {
            swatch->setStyleSheet(QStringLiteral("background-color: %1;").arg(color.name()));
            *shown = color;
        }
    });
}

void PropertyForm::addBool(const QString& label, Getter<bool> value) {
    auto* box = new QCheckBox(this);
    layout_->addRow(label, box);
    connect(box, &QCheckBox::toggled, this, [this, value](const bool checked) {
        if (bool* target = value()) {
            *target = checked;
            edited_();
        }
    });
    refreshers_.push_back([box, value] {
        if (const bool* current = value()) {
            const QSignalBlocker blocker(box);
            box->setChecked(*current);
        }
    });
}

void PropertyForm::addText(const QString& label, std::function<QString()> text) {
    auto* field = new QLabel(this);
    field->setTextInteractionFlags(Qt::TextSelectableByMouse);
    field->setWordWrap(true);
    layout_->addRow(label, field);
    refreshers_.push_back([field, text = std::move(text)] {
        if (const QString current = text(); current != field->text()) {
            field->setText(current);
        }
    });
}

void PropertyForm::addRow(const QString& label, QWidget* widget, std::function<void()> refresh) {
    widget->setParent(this);
    layout_->addRow(label, widget);
    refreshers_.push_back(std::move(refresh));
}

void PropertyForm::refresh() {
    for (const auto& refresher : refreshers_) {
        refresher();
    }
}
