#include "rig_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <editor/asset_picker.hpp>
#include <editor/paths.hpp>
#include <chrono>
#include <future>
#include <thread>
#include <utility>

namespace content {

RigDialog::RigDialog(const engine::AssetCatalog& catalog, const QString& source, const QString& folder,
                     QWidget* parent, const engine::RigAssetGroup* existing)
    : NewAssetDialog(existing ? tr("Reimport Animated Rig") : tr("Import Animated Rig"), folder, {},
                     QFileInfo(source).completeBaseName(), parent, existing != nullptr), source_(source) {
    if (existing) existing_ = *existing;
    resize(740, 560);
    auto* source_label = new QLabel(source);
    source_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form()->addRow(tr("Self-contained GLB"), source_label);
    report_ = new QLabel(tr("Inspecting skin and animation clips…"));
    report_->setWordWrap(true);
    report_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form()->addRow(report_);
    clips_ = new QTableWidget(0, 3);
    clips_->setObjectName(QStringLiteral("rigClips"));
    clips_->setHorizontalHeaderLabels({tr("Clip / existing asset"), tr("Duration / diagnostic"), tr("Import / mapping")});
    clips_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    form()->addRow(clips_);
    texture_ = new QCheckBox(existing ? tr("Refresh embedded baseColor texture (preserve material edits)")
                                    : tr("Extract embedded baseColor and create material instance"));
    texture_->setChecked(!existing);
    form()->addRow(texture_);
    if (!existing) {
        material_ = new editor::AssetPicker(catalog, engine::CatalogAssetKind::Material);
        material_->selectPath(QStringLiteral("engine/primitives/default.material.json"));
        connect(material_, &QComboBox::currentIndexChanged, this, [this] { validate(); });
        form()->addRow(tr("Base material"), material_);
        scale_ = new QDoubleSpinBox;
        scale_->setObjectName(QStringLiteral("rigPlacementScale"));
        scale_->setDecimals(6);
        scale_->setRange(0.000001, 1000000);
        scale_->setValue(1);
        scale_->setToolTip(tr("Initial entity scale only; source skeletal transforms are preserved. Fox example: 0.022."));
        form()->addRow(tr("Placement scale"), scale_);
        place_ = new QCheckBox(tr("Add to Scene after import"));
        form()->addRow(place_);
    }
    connect(clips_, &QTableWidget::itemChanged, this, [this] { validate(); });
    connect(texture_, &QCheckBox::toggled, this, [this] { validate(); });
    validate();
    inspect();
}

RigDialog::~RigDialog() {
    if (inspection_worker_.joinable()) inspection_worker_.join();
}

void RigDialog::inspect() {
    struct Result { engine::RigInspection inspection; std::string error; bool ok = false; };
    auto task = std::make_shared<std::packaged_task<Result()>>([path = editor::toPath(source_)] {
        Result result;
        try { result.ok = engine::InspectAnimatedGlb(path, result.inspection, result.error); }
        catch (const std::exception& e) { result.error = e.what(); }
        catch (...) { result.error = "GLB inspection failed"; }
        return result;
    });
    auto future = std::make_shared<std::future<Result>>(task->get_future());
    inspection_worker_ = std::jthread([task] { (*task)(); });
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this, timer, future] {
        if (future->wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        timer->stop();
        if (inspection_worker_.joinable()) inspection_worker_.join();
        auto result = future->get();
        ready_ = true;
        inspection_ = std::move(result.inspection);
        error_ = result.ok ? QString() : QString::fromStdString(result.error);
        showInspection();
        validate();
    });
    timer->start(40);
}

void RigDialog::showInspection() {
    QStringList report;
    report << tr("%1 joints, %2 mesh sections").arg(inspection_.jointCount).arg(inspection_.sectionCount);
    for (const auto& diagnostic : inspection_.diagnostics) report << QString::fromStdString(diagnostic);
    if (!error_.isEmpty()) report << error_;
    report_->setText(report.join(QLatin1Char('\n')));
    const bool can_extract = inspection_.canExtractBaseColor && (!existing_ || existing_->texture.has_value());
    texture_->setEnabled(can_extract);
    if (!can_extract) texture_->setChecked(false);
    if (existing_ && !existing_->texture)
        texture_->setToolTip(tr("This rig has no owned texture to refresh; neighbouring textures remain external."));
    const auto addRow = [this](const QString& name, const QString& detail) {
        const int row = clips_->rowCount();
        clips_->insertRow(row);
        for (int column = 0; column < 2; ++column) {
            auto* item = new QTableWidgetItem(column == 0 ? name : detail);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            clips_->setItem(row, column, item);
        }
        return row;
    };
    if (existing_) {
        for (const auto& binding : existing_->clips) {
            const int row = addRow(QString::fromStdString(binding.asset.path), binding.removed
                ? tr("Removed (reserved UUID); previously %1").arg(QString::fromStdString(binding.sourceName))
                : QString::fromStdString(binding.sourceName));
            auto* mapping = new QComboBox;
            mapping->addItem(binding.removed ? tr("Keep removed (reserve UUID)") : tr("Remove (reserve UUID)"), QString());
            bool matched = false;
            for (const auto& clip : inspection_.clips) {
                if (!clip.importable) continue;
                mapping->addItem((binding.removed ? tr("Restore as %1 (%2 s)") : tr("Use %1 (%2 s)"))
                                     .arg(QString::fromStdString(clip.name)).arg(clip.durationSeconds, 0, 'f', 3),
                                 QString::fromStdString(clip.name));
                if (!binding.removed && clip.name == binding.sourceName) { mapping->setCurrentIndex(mapping->count() - 1); matched = true; }
            }
            // Missing names require an explicit choice; never silently delete an existing clip.
            if (!binding.removed && !matched) { mapping->addItem(tr("Choose remap or removal…"), QVariant()); mapping->setCurrentIndex(mapping->count() - 1); }
            clips_->setCellWidget(row, 2, mapping);
            connect(mapping, &QComboBox::currentIndexChanged, this, [this] { validate(); });
        }
    }
    for (const auto& clip : inspection_.clips) {
        const int row = addRow(QString::fromStdString(clip.name),
                              clip.importable ? tr("%1 s").arg(clip.durationSeconds, 0, 'f', 3)
                                              : QString::fromStdString(clip.diagnostic));
        auto* item = new QTableWidgetItem(existing_ ? tr("Add new clip") : tr("Import"));
        item->setFlags(clip.importable ? Qt::ItemIsEnabled | Qt::ItemIsUserCheckable : Qt::NoItemFlags);
        item->setCheckState(!existing_ && clip.importable ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, QString::fromStdString(clip.name));
        clips_->setItem(row, 2, item);
    }
}

QString RigDialog::problem() const {
    if (const auto base = NewAssetDialog::problem(); !base.isEmpty()) return base;
    if (!ready_) return tr("Waiting for inspection…");
    if (!error_.isEmpty()) return error_;
    if (!inspection_.importable) return tr("This GLB cannot be imported. See validation diagnostics above.");
    if (texture_ && texture_->isChecked() && material_ && !material_->reference()) return tr("Choose a base material.");
    QStringList names;
    for (int row = 0; row < clips_->rowCount(); ++row) {
        QString name;
        if (auto* mapping = qobject_cast<QComboBox*>(clips_->cellWidget(row, 2))) {
            if (!mapping->currentData().isValid()) return tr("Choose remap or removal for every existing clip.");
            name = mapping->currentData().toString();
        } else if (const auto* item = clips_->item(row, 2); item && item->checkState() == Qt::Checked) {
            name = item->data(Qt::UserRole).toString();
        }
        if (name.isEmpty()) continue;
        if (names.contains(name)) return tr("Each source clip can be selected only once.");
        names << name;
    }
    return {};
}

engine::RigImportRequest RigDialog::importRequest() const {
    engine::RigImportRequest request;
    request.sourceGlb = editor::toPath(source_);
    request.destinationDirectory = editor::toPath(path());
    request.expectedSourceRevision = inspection_.sourceRevision;
    request.extractBaseColor = texture_->isChecked();
    request.baseMaterial = material_->reference();
    request.placementScale = static_cast<float>(scale_->value());
    for (int row = 0; row < clips_->rowCount(); ++row)
        if (const auto* item = clips_->item(row, 2); item && item->checkState() == Qt::Checked)
            request.clips.push_back(item->data(Qt::UserRole).toString().toStdString());
    return request;
}

engine::RigReimportRequest RigDialog::reimportRequest() const {
    engine::RigReimportRequest request;
    request.directory = existing_->directory;
    request.sourceGlb = editor::toPath(source_);
    request.expectedSourceRevision = inspection_.sourceRevision;
    request.expectedGroupRevision = existing_->revision;
    request.refreshTexture = texture_->isChecked();
    int row = 0;
    for (const auto& binding : existing_->clips) {
        const auto* mapping = qobject_cast<QComboBox*>(clips_->cellWidget(row++, 2));
        const auto name = mapping->currentData().toString().toStdString();
        if (binding.removed && name.empty()) continue;
        request.clips.push_back({binding.asset.id, name, name.empty()});
    }
    for (; row < clips_->rowCount(); ++row)
        if (const auto* item = clips_->item(row, 2); item && item->checkState() == Qt::Checked)
            request.clips.push_back({{}, item->data(Qt::UserRole).toString().toStdString(), false});
    return request;
}

bool RigDialog::addToScene() const { return place_ && place_->isChecked(); }

} // namespace content
