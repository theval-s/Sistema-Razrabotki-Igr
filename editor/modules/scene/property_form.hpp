#pragma once

#include <QWidget>

#include <functional>
#include <vector>

#include <SimpleMath.h>

class QFormLayout;

/// A form of labelled fields bound to values in the scene. Fields fetch their value through a
/// getter on every refresh() and edit, since component pools move their data around as components
/// come and go; a getter returning nullptr (entity or component gone) leaves its field alone.
/// A field being typed into isn't overwritten by refresh().
class PropertyForm : public QWidget {
public:
    template <class T>
    using Getter = std::function<T*()>;

    struct NumberFormat {
        double step = 0.1;
        int decimals = 3;
        double minimum = -1.0e6;
        double maximum = 1.0e6;
    };

    /// `edited` is called after every edit a field makes.
    explicit PropertyForm(std::function<void()> edited, QWidget* parent = nullptr);

    void addFloat(const QString& label, Getter<float> value, NumberFormat format = {});
    /// Stored in radians, edited in degrees: `format` is in degrees.
    void addAngle(const QString& label, Getter<float> value, NumberFormat format);
    void addVec3(const QString& label, Getter<DirectX::SimpleMath::Vector3> value, NumberFormat format = {});
    /// Edited as Euler angles in degrees, kept in the boxes: converting the quaternion back on every
    /// edit would make the angles jump around. They're recomputed when it changes elsewhere.
    void addRotation(const QString& label, Getter<DirectX::SimpleMath::Quaternion> value);
    /// Linear RGB: one box per channel, and a swatch that opens a color picker (which clamps to [0, 1]).
    void addColor(const QString& label, Getter<DirectX::SimpleMath::Vector3> value);
    void addBool(const QString& label, Getter<bool> value);
    /// Read-only, selectable text.
    void addText(const QString& label, std::function<QString()> text);
    /// Any other widget, which edits on its own; `refresh` is called by refresh().
    void addRow(const QString& label, QWidget* widget, std::function<void()> refresh);
    /// Same, without a label: the widget spans both columns.
    void addRow(QWidget* widget, std::function<void()> refresh);

    /// Scene -> fields.
    void refresh();

private:
    void addScaledFloat(const QString& label, Getter<float> value, NumberFormat format, double scale);

    QFormLayout* layout_;
    std::function<void()> edited_;
    std::vector<std::function<void()>> refreshers_;
};
