#pragma once

#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <engine/scripting/ScriptProperties.hpp>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;

namespace behavior {

/// A node's parameters as rows of name, type and value. Each change that leaves them valid goes to
/// `edited`; rows without a name are left out.
class ParameterEditor final : public QWidget {
    Q_OBJECT

public:
    using Edited = std::function<void(engine::ScriptPropertyMap parameters)>;
    ParameterEditor(const engine::ScriptPropertyMap& parameters, Edited edited, QWidget* parent = nullptr);

private:
    enum class Type { Text, Number, Boolean };
    struct Row {
        QLineEdit* name = nullptr;
        QComboBox* type = nullptr;
        QWidget* value = nullptr;  // a QLineEdit, or a QCheckBox for booleans
        QWidget* remove = nullptr;
    };

    void addRow(const std::string& name, const engine::StateValue& value);
    /// Swaps the row's value field for one of `type`, keeping what it can of the value.
    void retype(QLineEdit* name, Type type);
    /// Reports the rows if they're valid and changed, else says what's wrong.
    void commit();
    [[nodiscard]] std::optional<engine::StateValue> valueOf(const Row& row, QString& error) const;
    [[nodiscard]] QWidget* valueField(Type type, const QString& text, bool checked);
    [[nodiscard]] Row* find(const QLineEdit* name);

    Edited edited_;
    engine::ScriptPropertyMap last_;
    std::vector<Row> rows_;
    QGridLayout* grid_ = nullptr;
    QLabel* error_ = nullptr;
};

}  // namespace behavior
