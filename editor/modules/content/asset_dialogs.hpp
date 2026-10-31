#pragma once

#include <QDialog>
#include <QString>

#include <optional>

#include <engine/assets/AssetCatalog.hpp>
#include <engine/assets/MaterialDefinition.hpp>

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
class QTimer;

namespace editor {
class AssetPicker;
}

namespace content {

class ColorButton;

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

/// A model file to import, with its texture and an optional material instance (ModelImport).
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
    QLineEdit* texture_ = nullptr;
    QGroupBox* instance_ = nullptr;
    editor::AssetPicker* material_ = nullptr;
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
    int version_ = 2;
    std::optional<float> loaded_shininess_;
    double displayed_shininess_ = 0;
};

}  // namespace content
