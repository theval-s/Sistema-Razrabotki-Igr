#pragma once

#include <QDialog>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/MaterialDefinition.hpp>
#include <engine/assets/ModelImport.hpp>
#include <engine/assets/ParticleEffectDefinition.hpp>
#include <engine/assets/PostProcessDefinition.hpp>
#include <engine/assets/RenderPipelineDefinition.hpp>
#include <engine/assets/SkyboxDefinition.hpp>

#include "asset_operations.hpp"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace editor {
class AssetPicker;
}

namespace content {

class ColorButton;
class EmissionFields;

/// File dialog filter for images that import as textures.
[[nodiscard]] QString imageFilter();
/// File dialog filters for Import: images and models together, then each alone, then anything.
[[nodiscard]] QString importFilter();

/// Base class for asset dialog that creates one asset in a folder
class NewAssetDialog : public QDialog {
    Q_OBJECT

public:
    /// The asset goes to `folder`/<name>`suffix`. Folder because I said so :P
    NewAssetDialog(const QString& title, const QString& folder, const QString& suffix, const QString& name,
                   QWidget* parent, bool editing = false);

    [[nodiscard]] QString name() const;
    /// Where the new asset goes.
    [[nodiscard]] QString path() const;

protected:
    [[nodiscard]] QFormLayout* form() const { return form_; }
    /// Why the asset can't be created as it stands; empty if it can.
    [[nodiscard]] virtual QString problem() const;
    /// Checks problem() again. Subclasses call it once their fields exist and whenever one changes.
    void validate();
    [[nodiscard]] bool editing() const { return editing_; }
    void setVisible(bool visible) override;

private:
    QString folder_;
    QString suffix_;
    bool editing_ = false;
    QLineEdit* name_ = nullptr;
    QFormLayout* form_ = nullptr;
    QLabel* problem_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QPushButton* ok_ = nullptr;
};

class SkyboxDialog : public NewAssetDialog {
public:
    SkyboxDialog(const engine::AssetCatalog& catalog, const QString& folder, const QString& name,
                 QWidget* parent, bool editing = false);
    void load(const engine::SkyboxDefinition& definition);
    [[nodiscard]] engine::SkyboxDefinition definition() const;
protected:
    [[nodiscard]] QString problem() const override;
private:
    engine::SkyboxDefinition value_;
    editor::AssetPicker* cubemap_ = nullptr;
    QLabel* missing_ = nullptr;
};

class PostProcessDialog : public NewAssetDialog {
public:
    PostProcessDialog(const engine::AssetCatalog& catalog, const QString& folder, const QString& name,
                      QWidget* parent, bool editing = false);
    void load(const engine::PostProcessDefinition& definition);
    [[nodiscard]] engine::PostProcessDefinition definition() const;
protected:
    [[nodiscard]] QString problem() const override;
private:
    engine::PostProcessDefinition value_;
    editor::AssetPicker* shader_ = nullptr;
    QLineEdit* entry_ = nullptr;
    QTableWidget* defines_ = nullptr;
    QLabel* missing_ = nullptr;
};

class RenderPipelineDialog : public NewAssetDialog {
public:
    RenderPipelineDialog(const engine::AssetCatalog& catalog, const QString& folder, const QString& name,
                         QWidget* parent, bool editing = false);
    void load(const engine::RenderPipelineDefinition& definition);
    [[nodiscard]] engine::RenderPipelineDefinition definition() const;
protected:
    [[nodiscard]] QString problem() const override;
private:
    engine::RenderPipelineDefinition value_;
    editor::AssetPicker* shader_ = nullptr;
    QLineEdit* vertex_ = nullptr;
    QLineEdit* pixel_ = nullptr;
    QComboBox* blend_ = nullptr;
    QComboBox* depth_ = nullptr;
    QTableWidget* defines_ = nullptr;
    QLabel* missing_ = nullptr;
};

class ParticleEffectDialog : public NewAssetDialog {
public:
    ParticleEffectDialog(const engine::AssetCatalog& catalog, const QString& folder, const QString& name,
                         QWidget* parent, bool editing = false);
    void load(const engine::ParticleEffectDefinition& definition);
    [[nodiscard]] engine::ParticleEffectDefinition definition() const;
protected:
    [[nodiscard]] QString problem() const override;
private:
    engine::ParticleEffectDefinition value_;
    editor::AssetPicker* texture_ = nullptr;
    std::vector<std::function<void()>> refreshers_;
};

/// A model's material slots and what they get: its textures, and a material instance each.
/// Import Model and Extract Materials both show it.
class ModelMaterialsForm : public QWidget {
    Q_OBJECT

public:
    ModelMaterialsForm(const engine::AssetCatalog& catalog, const QString& model, QWidget* parent = nullptr);

    [[nodiscard]] engine::ModelMaterialSettings settings() const;
    /// Why it can't go ahead as it stands; empty if it can.
    [[nodiscard]] QString problem() const;

signals:
    void changed();

private:
    QString error_;  // from reading the model
    QCheckBox* extract_ = nullptr;
    QGroupBox* instances_ = nullptr;
    editor::AssetPicker* material_ = nullptr;
    QLineEdit* fallback_ = nullptr;
};

/// A model file to import into a folder of its own, with its materials (ModelImport).
class ImportModelDialog : public NewAssetDialog {
    Q_OBJECT

public:
    ImportModelDialog(const engine::AssetCatalog& catalog, const QString& source, const QString& folder,
                      QWidget* parent);

    [[nodiscard]] ModelImport result() const;

protected:
    [[nodiscard]] QString problem() const override;

private:
    QString source_;
    QCheckBox* center_ = nullptr;
    ModelMaterialsForm* materials_ = nullptr;
};

/// Textures and material instances for a model already in the project (AssetOperations::extractMaterials).
class ExtractMaterialsDialog : public NewAssetDialog {
    Q_OBJECT

public:
    ExtractMaterialsDialog(const engine::AssetCatalog& catalog, const QString& model, QWidget* parent);

    [[nodiscard]] engine::ModelMaterialSettings result() const;

protected:
    [[nodiscard]] QString problem() const override;

private:
    ModelMaterialsForm* materials_ = nullptr;
};

/// A material instance: a base material and the parameters it overrides.
class MaterialInstanceDialog : public NewAssetDialog {
    Q_OBJECT

public:
    MaterialInstanceDialog(const engine::AssetCatalog& catalog, const QString& folder, const QString& name,
                           QWidget* parent, bool editing = false);

    void load(const engine::MaterialInstanceDefinition& definition);

    /// Preselects the albedo texture by catalog path.
    void setAlbedo(const QString& path);
    [[nodiscard]] engine::MaterialInstanceDefinition definition() const;

protected:
    [[nodiscard]] QString problem() const override;

private:
    /// Shows the base material's values in the parameters that aren't overridden.
    void showInherited();

    const engine::AssetCatalog& catalog_;
    editor::AssetPicker* material_ = nullptr;
    editor::AssetPicker* albedo_ = nullptr;
    QComboBox* albedo_mode_ = nullptr;
    QTimer* pending_albedo_timer_ = nullptr;
    QString pending_albedo_;
    int pending_albedo_ticks_ = 0;
    QCheckBox* override_specular_ = nullptr;
    ColorButton* specular_ = nullptr;
    QCheckBox* override_shininess_ = nullptr;
    QDoubleSpinBox* shininess_ = nullptr;
    EmissionFields* emission_fields_ = nullptr;
    int version_ = 2;
    std::optional<float> loaded_shininess_;
    double displayed_shininess_ = 0;
};

/// A base material: an optional shader with the hooks it implements, and default parameters.
class MaterialDialog : public NewAssetDialog {
    Q_OBJECT

public:
    MaterialDialog(const engine::AssetCatalog& catalog, const QString& folder, const QString& name,
                    QWidget* parent, bool editing = false);

    void load(const engine::MaterialDefinition& definition);

    [[nodiscard]] engine::MaterialDefinition definition() const;

protected:
    [[nodiscard]] QString problem() const override;

private:
    editor::AssetPicker* shader_ = nullptr;
    QCheckBox* gbuffer_vs_ = nullptr;
    QCheckBox* gbuffer_ps_ = nullptr;
    QCheckBox* shadow_vs_ = nullptr;
    editor::AssetPicker* albedo_ = nullptr;
    QComboBox* albedo_mode_ = nullptr;
    QCheckBox* override_specular_ = nullptr;
    ColorButton* specular_ = nullptr;
    QCheckBox* override_shininess_ = nullptr;
    QDoubleSpinBox* shininess_ = nullptr;
    EmissionFields* emission_fields_ = nullptr;
    int version_ = 2;
    std::optional<float> loaded_shininess_;
    double displayed_shininess_ = 0;
};

}  // namespace content
