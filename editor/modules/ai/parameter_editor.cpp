#include "parameter_editor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace behavior {

namespace {

constexpr auto ProblemStyle = "color: rgb(211, 47, 47);";

QString numberText(const engine::StateValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value.value)) {
        return QString::number(*integer);
    }
    if (const auto* natural = std::get_if<std::uint64_t>(&value.value)) {
        return QString::number(*natural);
    }
    return QString::number(value.Number(), 'g', QLocale::FloatingPointShortest);
}

}  // namespace

ParameterEditor::ParameterEditor(const engine::ScriptPropertyMap& parameters, Edited edited, QWidget* parent)
    : QWidget(parent), edited_(std::move(edited)), last_(parameters) {
    grid_ = new QGridLayout;
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setColumnStretch(0, 1);
    grid_->setColumnStretch(2, 1);

    auto* add = new QPushButton(tr("Add Parameter"));
    connect(add, &QPushButton::clicked, this, [this] {
        addRow({}, 0.0);
        rows_.back().name->setFocus();
    });
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(add);
    buttons->addStretch();

    error_ = new QLabel;
    error_->setWordWrap(true);
    error_->setStyleSheet(QString::fromLatin1(ProblemStyle));
    error_->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(grid_);
    layout->addLayout(buttons);
    layout->addWidget(error_);

    for (const auto& [name, value] : parameters) {
        addRow(name, value);
    }
}

void ParameterEditor::addRow(const std::string& name, const engine::StateValue& value) {
    Row row;
    row.name = new QLineEdit(QString::fromStdString(name));
    row.name->setPlaceholderText(tr("Name"));
    connect(row.name, &QLineEdit::editingFinished, this, &ParameterEditor::commit);

    Type type = Type::Number;
    if (std::holds_alternative<std::string>(value.value)) {
        type = Type::Text;
    } else if (std::holds_alternative<bool>(value.value)) {
        type = Type::Boolean;
    }
    row.type = new QComboBox;
    row.type->addItem(tr("Text"), static_cast<int>(Type::Text));
    row.type->addItem(tr("Number"), static_cast<int>(Type::Number));
    row.type->addItem(tr("Boolean"), static_cast<int>(Type::Boolean));
    row.type->setCurrentIndex(row.type->findData(static_cast<int>(type)));
    connect(row.type, &QComboBox::activated, this, [this, name = row.name, types = row.type](const int index) {
        retype(name, static_cast<Type>(types->itemData(index).toInt()));
    });

    row.value = valueField(type, type == Type::Text ? QString::fromStdString(value.String()) : numberText(value),
                           value.Boolean());

    auto* remove = new QToolButton;
    remove->setText(QStringLiteral("×"));
    remove->setToolTip(tr("Remove this parameter"));
    connect(remove, &QToolButton::clicked, this, [this, name = row.name] {
        const Row* found = find(name);
        if (found == nullptr) {
            return;
        }
        for (QWidget* widget : {static_cast<QWidget*>(found->name), static_cast<QWidget*>(found->type), found->value,
                                found->remove}) {
            widget->hide();
            widget->deleteLater();  // one of them is sending this
        }
        std::erase_if(rows_, [name](const Row& each) { return each.name == name; });
        commit();
    });
    row.remove = remove;

    const int line = grid_->rowCount();
    grid_->addWidget(row.name, line, 0);
    grid_->addWidget(row.type, line, 1);
    grid_->addWidget(row.value, line, 2);
    grid_->addWidget(row.remove, line, 3);
    rows_.push_back(row);
}

void ParameterEditor::retype(QLineEdit* name, const Type type) {
    Row* row = find(name);
    if (row == nullptr) {
        return;
    }
    QString text;
    bool checked = false;
    if (const auto* box = qobject_cast<QCheckBox*>(row->value)) {
        checked = box->isChecked();
        text = type == Type::Number ? QString::number(checked ? 1 : 0)
                                    : (checked ? QStringLiteral("true") : QStringLiteral("false"));
    } else {
        text = static_cast<QLineEdit*>(row->value)->text();
        checked = text.trimmed() == QStringLiteral("true") || text.trimmed() == QStringLiteral("1");
        if (type == Type::Number) {
            bool number = false;
            text.trimmed().toDouble(&number);
            if (!number) {
                text = QStringLiteral("0");
            }
        }
    }
    QWidget* field = valueField(type, text, checked);
    delete grid_->replaceWidget(row->value, field);
    row->value->hide();
    row->value->deleteLater();
    row->value = field;
    commit();
}

void ParameterEditor::commit() {
    engine::ScriptPropertyMap values;
    QString error;
    for (const Row& row : rows_) {
        const QString name = row.name->text().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        std::optional<engine::StateValue> value = valueOf(row, error);
        if (!value) {
            break;
        }
        if (!values.emplace(name.toStdString(), std::move(*value)).second) {
            error = tr("Two parameters are called '%1'.").arg(name);
            break;
        }
    }
    error_->setText(error);
    error_->setVisible(!error.isEmpty());
    if (error.isEmpty() && values != last_) {
        last_ = values;
        edited_(std::move(values));
    }
}

std::optional<engine::StateValue> ParameterEditor::valueOf(const Row& row, QString& error) const {
    if (const auto* box = qobject_cast<const QCheckBox*>(row.value)) {
        return engine::StateValue(box->isChecked());
    }
    const QString text = static_cast<const QLineEdit*>(row.value)->text();
    if (static_cast<Type>(row.type->currentData().toInt()) == Type::Text) {
        return engine::StateValue(text.toStdString());
    }
    // Doubles, as the tree's reader gives them.
    bool valid = false;
    if (const double number = text.trimmed().toDouble(&valid); valid && std::isfinite(number)) {
        return engine::StateValue(number);
    }
    error = tr("'%1' needs a number, not '%2'.").arg(row.name->text().trimmed(), text);
    return std::nullopt;
}

QWidget* ParameterEditor::valueField(const Type type, const QString& text, const bool checked) {
    if (type == Type::Boolean) {
        auto* box = new QCheckBox;
        box->setChecked(checked);
        connect(box, &QCheckBox::toggled, this, &ParameterEditor::commit);
        return box;
    }
    auto* edit = new QLineEdit(text);
    connect(edit, &QLineEdit::editingFinished, this, &ParameterEditor::commit);
    return edit;
}

ParameterEditor::Row* ParameterEditor::find(const QLineEdit* name) {
    const auto found = std::ranges::find(rows_, name, &Row::name);
    return found != rows_.end() ? &*found : nullptr;
}

}  // namespace behavior
