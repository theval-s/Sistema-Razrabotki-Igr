#include "property_form.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QEvent>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
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

const QString LinkScaleKey = QStringLiteral("inspector/linkScale");
/// Set on widgets that have a WheelGuard, so none gets two.
constexpr const char* WheelGuardedProperty = "wheelGuarded";

/// See PropertyForm::ignoreWheelUnlessFocused().
class WheelGuard final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Wheel || static_cast<QWidget*>(watched)->hasFocus()) {
            return false;
        }
        // Filtered but not accepted: Qt then offers the wheel to the parent.
        event->ignore();
        return true;
    }
};

// we might need a separate file for all the types of field we might add
//as this is A LOT of boilerplate
QDoubleSpinBox* makeSpinBox(QWidget* parent, const PropertyForm::NumberFormat& format) {
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(format.minimum, format.maximum);
    box->setDecimals(format.decimals);
    box->setSingleStep(format.step);
    box->setAccelerated(true);
    box->setMinimumWidth(60);
    // Typing "0.5" would otherwise apply 0 first, which e.g. a physics body's scale refuses.
    box->setKeyboardTracking(false);
    PropertyForm::ignoreWheelUnlessFocused(box);
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

float& component(Vector3& vector, const int axis) {
    return (&vector.x)[axis];
}

/// Two links of a chain, joined (on) or pulled apart (off), drawn in `color`.
QIcon chainIcon(const QColor& color) {
    const auto draw = [&color](const bool linked) {
        QPixmap pixmap(32, 32);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 3.0));
        const qreal gap = linked ? -6.0 : 4.0;
        const qreal height = (28.0 - gap) / 2.0;
        painter.drawRoundedRect(QRectF(10.0, 2.0, 12.0, height), 6.0, 6.0);
        painter.drawRoundedRect(QRectF(10.0, 2.0 + height + gap, 12.0, height), 6.0, 6.0);
        return pixmap;
    };
    QIcon icon;
    icon.addPixmap(draw(false), QIcon::Normal, QIcon::Off);
    icon.addPixmap(draw(true), QIcon::Normal, QIcon::On);
    return icon;
}

QColor toColor(const Vector3& rgb) {
    return QColor::fromRgbF(std::clamp(rgb.x, 0.0f, 1.0f), std::clamp(rgb.y, 0.0f, 1.0f),
                            std::clamp(rgb.z, 0.0f, 1.0f));
}

}  // namespace

PropertyForm::PropertyForm(QWidget* parent) : QWidget(parent), layout_(new QFormLayout(this)) {}

void PropertyForm::addFloat(const QString& label, Binding<float> value, const NumberFormat format) {
    addScaledFloat(label, std::move(value), format, 1.0);
}

void PropertyForm::addAngle(const QString& label, Binding<float> value, const NumberFormat format) {
    addScaledFloat(label, std::move(value), format, DirectX::XMConvertToDegrees(1.0f));
}

void PropertyForm::record(const QString& label, const std::function<void()>& change) const {
    for (const QObject* object = this; object != nullptr; object = object->parent()) {
        if (const auto* form = dynamic_cast<const PropertyForm*>(object); form != nullptr && form->recorder_) {
            form->recorder_(label, change);
            return;
        }
    }
    change();
}

void PropertyForm::addScaledFloat(const QString& label, Binding<float> value, const NumberFormat format,
                                  const double scale) {
    value.set = recorded(label, std::move(value.set));
    QDoubleSpinBox* box = makeSpinBox(this, format);
    layout_->addRow(label, box);
    connect(box, &QDoubleSpinBox::valueChanged, this, [set = value.set, scale](const double shown) {
        set(static_cast<float>(shown / scale));
    });
    refreshers_.push_back([box, get = value.get, scale] {
        if (const float* current = get()) {
            showValue(box, *current * scale);
        }
    });
}

void PropertyForm::addVec3(const QString& label, Binding<Vector3> value, const NumberFormat format) {
    value.set = recorded(label, std::move(value.set));
    QHBoxLayout* row = nullptr;
    const Vec3Boxes boxes = makeVec3Boxes(this, format, row);
    layout_->addRow(label, row);
    for (QDoubleSpinBox* box : boxes) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [set = value.set, boxes] { set(read(boxes)); });
    }
    refreshers_.push_back([boxes, get = value.get] {
        if (const Vector3* current = get()) {
            showVec3(boxes, *current);
        }
    });
}

void PropertyForm::addScale(const QString& label, Binding<Vector3> value, const NumberFormat format) {
    value.set = recorded(label, std::move(value.set));
    QHBoxLayout* row = nullptr;
    const Vec3Boxes boxes = makeVec3Boxes(this, format, row);
    auto* link = new QToolButton(this);
    link->setCheckable(true);
    link->setAutoRaise(true);
    link->setIcon(chainIcon(palette().color(QPalette::ButtonText)));
    link->setToolTip(tr("Keep proportions: editing one axis scales the others by the same factor"));
    link->setChecked(QSettings().value(LinkScaleKey, false).toBool());
    connect(link, &QToolButton::toggled, this, [](const bool checked) { QSettings().setValue(LinkScaleKey, checked); });
    row->addWidget(link);
    layout_->addRow(label, row);

    for (int axis = 0; axis < 3; ++axis) {
        connect(boxes[axis], &QDoubleSpinBox::valueChanged, this, [value, boxes, link, axis](const double changed) {
            Vector3 scale = read(boxes);
            const Vector3* current = value.get();
            if (link->isChecked() && current != nullptr) {
                Vector3 before = *current;
                // From 0 there's no factor to scale the others by: they stay.
                if (const float from = component(before, axis); from != 0.0f) {
                    scale = before * (static_cast<float>(changed) / from);
                    component(scale, axis) = static_cast<float>(changed);
                    showVec3(boxes, scale);
                }
            }
            value.set(scale);
        });
    }
    refreshers_.push_back([boxes, get = value.get] {
        if (const Vector3* current = get()) {
            showVec3(boxes, *current);
        }
    });
}

void PropertyForm::addRotation(const QString& label, Binding<Quaternion> value) {
    value.set = recorded(label, std::move(value.set));
    QHBoxLayout* row = nullptr;
    const Vec3Boxes boxes = makeVec3Boxes(this, {.step = 1.0, .decimals = 2}, row);
    layout_->addRow(label, row);
    // What the boxes show. Never a rotation to begin with, so the first refresh shows it.
    auto shown = std::make_shared<Quaternion>(0.0f, 0.0f, 0.0f, 0.0f);
    for (QDoubleSpinBox* box : boxes) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [set = value.set, boxes, shown] {
            const Vector3 degrees = read(boxes);
            // Shown before it's set: the boxes already hold these angles.
            *shown = Quaternion::CreateFromYawPitchRoll(Vector3{DirectX::XMConvertToRadians(degrees.x),
                                                                DirectX::XMConvertToRadians(degrees.y),
                                                                DirectX::XMConvertToRadians(degrees.z)});
            set(*shown);
        });
    }
    refreshers_.push_back([boxes, get = value.get, shown] {
        const Quaternion* current = get();
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

void PropertyForm::addColor(const QString& label, Binding<Vector3> value) {
    value.set = recorded(label, std::move(value.set));
    QHBoxLayout* row = nullptr;
    const Vec3Boxes boxes = makeVec3Boxes(this, {.step = 0.05, .decimals = 3, .minimum = 0.0, .maximum = 1.0e3}, row);
    auto* swatch = new QToolButton(this);
    swatch->setToolTip(tr("Pick a color"));
    swatch->setFixedWidth(28);
    row->addWidget(swatch);
    layout_->addRow(label, row);

    for (QDoubleSpinBox* box : boxes) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [set = value.set, boxes] { set(read(boxes)); });
    }
    connect(swatch, &QToolButton::clicked, this, [this, value, label] {
        const Vector3* current = value.get();
        if (current == nullptr) {
            return;
        }
        // thank god we're using Qt
        const QColor picked = QColorDialog::getColor(toColor(*current), this, label);
        // The entity may be gone after the dialog's event loop: then setting does nothing.
        if (picked.isValid()) {
            value.set(Vector3{static_cast<float>(picked.redF()), static_cast<float>(picked.greenF()),
                              static_cast<float>(picked.blueF())});
        }
    });
    auto shown = std::make_shared<QColor>();
    refreshers_.push_back([boxes, get = value.get, swatch, shown] {
        const Vector3* current = get();
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

void PropertyForm::addBool(const QString& label, Binding<bool> value) {
    value.set = recorded(label, std::move(value.set));
    auto* box = new QCheckBox(this);
    layout_->addRow(label, box);
    connect(box, &QCheckBox::toggled, this, [set = value.set](const bool checked) { set(checked); });
    refreshers_.push_back([box, get = value.get] {
        if (const bool* current = get()) {
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
    guardWheels(widget);
    layout_->addRow(label, widget);
    refreshers_.push_back(std::move(refresh));
}

void PropertyForm::addRow(QWidget* widget, std::function<void()> refresh) {
    widget->setParent(this);
    guardWheels(widget);
    layout_->addRow(widget);
    refreshers_.push_back(std::move(refresh));
}

void PropertyForm::refresh() {
    for (const auto& refresher : refreshers_) {
        refresher();
    }
}

void PropertyForm::ignoreWheelUnlessFocused(QWidget* widget) {
    if (widget->property(WheelGuardedProperty).toBool()) {
        return;
    }
    widget->setProperty(WheelGuardedProperty, true);
    // Clicking or tabbing in gives focus; the wheel alone doesn't.
    widget->setFocusPolicy(Qt::StrongFocus);
    widget->installEventFilter(new WheelGuard(widget));
}

void PropertyForm::guardWheels(QWidget* widget) {
    const auto guard = [](QWidget* candidate) {
        if (qobject_cast<QAbstractSpinBox*>(candidate) != nullptr || qobject_cast<QComboBox*>(candidate) != nullptr) {
            ignoreWheelUnlessFocused(candidate);
        }
    };
    guard(widget);
    for (QWidget* child : widget->findChildren<QWidget*>()) {
        guard(child);
    }
}
