#include <editor/autoload_dialog.hpp>

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/assets/AssetSystem.hpp>
#include <engine/engine.hpp>
#include <engine/project/Project.hpp>

#include <editor/paths.hpp>

namespace editor {
namespace {

enum Column { Enabled, Name, Script, ColumnCount };

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

}  // namespace

AutoloadDialog::AutoloadDialog(engine::Engine& engine, const engine::Project& project, QWidget* parent)
    : QDialog(parent), engine_(engine), project_(project), definitions_(project.autoloads) {
    setWindowTitle(tr("Autoload"));
    resize(700, 380);

    table_ = new QTableWidget(0, ColumnCount, this);
    table_->setObjectName(QStringLiteral("AutoloadTable"));
    table_->setHorizontalHeaderLabels({tr("Enabled"), tr("Name"), tr("Script")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->horizontalHeader()->setSectionResizeMode(Enabled, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(Name, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(Script, QHeaderView::Stretch);

    auto* add = new QPushButton(tr("Add..."), this);
    remove_ = new QPushButton(tr("Remove"), this);
    up_ = new QPushButton(tr("Up"), this);
    down_ = new QPushButton(tr("Down"), this);
    add->setObjectName(QStringLiteral("AddAutoload"));
    remove_->setObjectName(QStringLiteral("RemoveAutoload"));
    up_->setObjectName(QStringLiteral("MoveAutoloadUp"));
    down_->setObjectName(QStringLiteral("MoveAutoloadDown"));
    auto* actions = new QHBoxLayout;
    actions->addWidget(add);
    actions->addWidget(remove_);
    actions->addStretch();
    actions->addWidget(up_);
    actions->addWidget(down_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Scripts start in this order on the next Lua session."), this));
    layout->addWidget(table_);
    layout->addLayout(actions);
    layout->addWidget(buttons);

    connect(add, &QPushButton::clicked, this, &AutoloadDialog::addScript);
    connect(remove_, &QPushButton::clicked, this, &AutoloadDialog::removeSelected);
    connect(up_, &QPushButton::clicked, this, [this] { moveSelected(-1); });
    connect(down_, &QPushButton::clicked, this, [this] { moveSelected(1); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, &AutoloadDialog::refreshButtons);
    connect(buttons, &QDialogButtonBox::accepted, this, &AutoloadDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    refreshRows(definitions_.empty() ? -1 : 0);
}

std::vector<engine::AutoloadDefinition> AutoloadDialog::definitions() const {
    auto result = definitions_;
    for (int row = 0; row < table_->rowCount(); ++row) {
        auto& definition = result[static_cast<std::size_t>(row)];
        definition.name = table_->item(row, Name)->text().toUtf8().toStdString();
        definition.enabled = table_->item(row, Enabled)->checkState() == Qt::Checked;
    }
    return result;
}

void AutoloadDialog::accept() {
    std::string error;
    if (!engine::ValidateAutoloadDefinitions(definitions(), error)) {
        QMessageBox::warning(this, tr("Autoload"), text(error));
        return;
    }
    QDialog::accept();
}

void AutoloadDialog::refreshRows(int selected) {
    table_->setRowCount(0);
    for (const auto& definition : definitions_) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        auto* enabled = new QTableWidgetItem;
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabled->setCheckState(definition.enabled ? Qt::Checked : Qt::Unchecked);
        table_->setItem(row, Enabled, enabled);
        table_->setItem(row, Name, new QTableWidgetItem(text(definition.name)));
        auto* script = new QTableWidgetItem(text(definition.script.path));
        script->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        script->setToolTip(tr("UUID: %1\nMissing files are diagnosed when starting the session.")
                               .arg(text(definition.script.id.ToString())));
        table_->setItem(row, Script, script);
    }
    if (selected >= 0 && selected < table_->rowCount()) table_->selectRow(selected);
    refreshButtons();
}

void AutoloadDialog::refreshButtons() {
    const int row = table_->currentRow();
    const bool selected = row >= 0 && row < table_->rowCount();
    remove_->setEnabled(selected);
    up_->setEnabled(selected && row > 0);
    down_->setEnabled(selected && row + 1 < table_->rowCount());
}

void AutoloadDialog::removeSelected() {
    const int row = table_->currentRow();
    if (row < 0 || row >= table_->rowCount()) return;
    definitions_ = definitions();
    definitions_.erase(definitions_.begin() + row);
    refreshRows(std::min(row, static_cast<int>(definitions_.size()) - 1));
}

void AutoloadDialog::moveSelected(int offset) {
    const int row = table_->currentRow();
    const int target = row + offset;
    if (row < 0 || target < 0 || target >= table_->rowCount()) return;
    definitions_ = definitions();
    std::swap(definitions_[static_cast<std::size_t>(row)], definitions_[static_cast<std::size_t>(target)]);
    refreshRows(target);
}

void AutoloadDialog::addScript() {
    const QString chosen = QFileDialog::getOpenFileName(
        this, tr("Add Autoload Script"), toQString(project_.ContentDir()), tr("Lua Scripts (*.lua)"));
    if (chosen.isEmpty()) return;
    std::string error;
    try {
        const auto source = std::filesystem::canonical(toPath(chosen));
        const auto root = std::filesystem::canonical(project_.ContentDir());
        const auto relative = std::filesystem::relative(source, root);
        if (relative.empty() || relative.is_absolute() ||
            std::find(relative.begin(), relative.end(), std::filesystem::path("..")) != relative.end()) {
            QMessageBox::warning(this, tr("Autoload"), tr("Choose a script inside this project's content directory."));
            return;
        }
        if (!std::filesystem::is_regular_file(source) ||
            engine::CatalogKindForFile(source) != engine::CatalogAssetKind::Script) {
            QMessageBox::warning(this, tr("Autoload"), tr("Choose a .lua script; .d.lua files are declarations."));
            return;
        }
        // rescan first
        if (!engine_.RescanAssetCatalogs(error)) {
            QMessageBox::warning(this, tr("Autoload"), tr("Could not refresh the asset catalog.\n%1").arg(text(error)));
            return;
        }
        engine::AssetMetadata metadata;
        if (!engine::ReadAssetMetadata(source, metadata, error)) {
            QMessageBox::warning(this, tr("Autoload"), tr("The script needs valid asset metadata.\n%1").arg(text(error)));
            return;
        }
        const auto* entry = engine_.GetAssets().GetCatalog().FindPath(engine::AssetPathResolver::ToName(source));
        if (!entry || entry->metadata.kind != engine::CatalogAssetKind::Script || entry->metadata.id != metadata.id) {
            QMessageBox::warning(this, tr("Autoload"), tr("The selected file is not a catalogued project Script asset."));
            return;
        }
        auto updated = definitions();
        const bool listed = std::any_of(updated.begin(), updated.end(), [&](const auto& value) {
            return value.script.id == entry->metadata.id;
        });
        if (listed) {
            QMessageBox::warning(this, tr("Autoload"), tr("This script is already in the autoload list."));
            return;
        }
        QString base = toQString(source.stem());
        base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
        if (base.isEmpty()) base = QStringLiteral("Autoload");
        if (base.front().isDigit()) base.prepend(QLatin1Char('_'));
        QString name = base;
        for (int suffix = 1; std::any_of(updated.begin(), updated.end(), [&](const auto& value) {
                 return value.name == name.toUtf8().toStdString();
             }); ++suffix) {
            name = base + QString::number(suffix);
        }
        updated.push_back({name.toUtf8().toStdString(), {entry->metadata.id, entry->path}, true});
        definitions_ = std::move(updated);
        refreshRows(static_cast<int>(definitions_.size()) - 1);
    } catch (const std::exception& exception) {
        QMessageBox::warning(this, tr("Autoload"), text(exception.what()));
    }
}

}  // namespace editor
