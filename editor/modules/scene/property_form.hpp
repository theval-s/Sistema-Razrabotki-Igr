#pragma once

#include <QWidget>

#include <functional>
#include <vector>

#include <SimpleMath.h>

class QFormLayout;

/// A form of labelled fields bound to values in the scene. Fields read their value through the
/// binding on every refresh(), since component pools move their data around as components come
/// and go, and write edits back through it, so that the scene can report them.
/// A field being typed into isn't overwritten by refresh(). Typed numbers apply on Enter or
/// when the field loses focus; the arrows and the wheel apply each step. Number fields and
/// combo boxes only take the wheel once clicked into, so that it scrolls the form past them.
class PropertyForm : public QWidget {
public:
    /// `get` returns nullptr once the entity or the component is gone
    template <class T>
    struct Binding {
        std::function<const T*()> get;
        std::function<void(const T&)> set;
    };

    struct NumberFormat {
        double step = 0.1;
        int decimals = 3;
        double minimum = -1.0e6;
        double maximum = 1.0e6;
    };

    /// Runs each edit a field makes, e.g. as an undoable step. Nested forms use their outer form's.
    using Recorder = std::function<void(const QString& label, const std::function<void()>& change)>;

    explicit PropertyForm(QWidget* parent = nullptr);

    void setRecorder(Recorder recorder) { recorder_ = std::move(recorder); }

    void addFloat(const QString& label, Binding<float> value, NumberFormat format = {});
    /// Stored in radians, edited in degrees: `format` is in degrees.
    void addAngle(const QString& label, Binding<float> value, NumberFormat format);
    void addVec3(const QString& label, Binding<DirectX::SimpleMath::Vector3> value, NumberFormat format = {});
    /// A Vec3 with a chain toggle: while it's on, editing one component scales the other two by
    /// the same factor (unless that component was 0). The toggle is remembered across forms.
    void addScale(const QString& label, Binding<DirectX::SimpleMath::Vector3> value, NumberFormat format = {});
    /// Edited as Euler angles in degrees, kept in the boxes: converting the quaternion back on every
    /// edit would make the angles jump around. They're recomputed when it changes elsewhere.
    void addRotation(const QString& label, Binding<DirectX::SimpleMath::Quaternion> value);
    /// Linear RGB: one box per channel, and a swatch that opens a color picker (which clamps to [0, 1]).
    void addColor(const QString& label, Binding<DirectX::SimpleMath::Vector3> value);
    void addBool(const QString& label, Binding<bool> value);
    /// Read-only, selectable text.
    void addText(const QString& label, std::function<QString()> text);
    /// Any other widget, which edits on its own; `refresh` is called by refresh(). Its number
    /// fields and combo boxes get the wheel handling described above.
    void addRow(const QString& label, QWidget* widget, std::function<void()> refresh);
    /// Same, without a label: the widget spans both columns.
    void addRow(QWidget* widget, std::function<void()> refresh);

    /// Scene -> fields.
    void refresh();

    /// Lets `widget` take wheel events only while it has focus; otherwise they go on to its
    /// parent, e.g. the scroll area around the form. For widgets placed outside a PropertyForm.
    static void ignoreWheelUnlessFocused(QWidget* widget);

private:
    /// ignoreWheelUnlessFocused() for `widget` and its children that are number fields or combo boxes.
    static void guardWheels(QWidget* widget);
    void addScaledFloat(const QString& label, Binding<float> value, NumberFormat format, double scale);
    /// `set` going through the recorder.
    template <class T>
    std::function<void(const T&)> recorded(const QString& label, std::function<void(const T&)> set) {
        return [this, label, set = std::move(set)](const T& value) { record(label, [&] { set(value); }); };
    }
    void record(const QString& label, const std::function<void()>& change) const;

    QFormLayout* layout_;
    std::vector<std::function<void()>> refreshers_;
    Recorder recorder_;
};
