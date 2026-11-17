#pragma once

#include "asset_dialogs.hpp"
#include <engine/assets/RigAuthoring.hpp>
#include <vector>
#include <thread>

class QTableWidget;

namespace content {

/// Inspection owns only CPU data and never touches the scene or catalog on its worker.
class RigDialog : public NewAssetDialog {
public:
    RigDialog(const engine::AssetCatalog& catalog, const QString& source, const QString& folder,
              QWidget* parent, const engine::RigAssetGroup* existing = nullptr);
    ~RigDialog() override;
    [[nodiscard]] engine::RigImportRequest importRequest() const;
    [[nodiscard]] engine::RigReimportRequest reimportRequest() const;
    [[nodiscard]] bool addToScene() const;

protected:
    QString problem() const override;

private:
    void inspect();
    void showInspection();
    QString source_;
    std::optional<engine::RigAssetGroup> existing_;
    engine::RigInspection inspection_;
    bool ready_ = false;
    QString error_;
    QLabel* report_ = nullptr;
    QTableWidget* clips_ = nullptr;
    QCheckBox* texture_ = nullptr;
    QCheckBox* place_ = nullptr;
    QDoubleSpinBox* scale_ = nullptr;
    editor::AssetPicker* material_ = nullptr;
    // Joined before this dialog (and its module code) can be destroyed.
    std::jthread inspection_worker_;
};

} // namespace content
