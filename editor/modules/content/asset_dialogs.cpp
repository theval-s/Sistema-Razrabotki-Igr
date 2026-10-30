#include "asset_dialogs.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string>

#include <engine/assets/AssetPathResolver.hpp>
#include <engine/core/FileSystem.hpp>

#include <editor/paths.hpp>

#include "asset_picker.hpp"

namespace content {

namespace {

const QString DefaultMaterial = QStringLiteral("engine/primitives/default.material.json");
const QString WhiteTexture = QStringLiteral("engine/primitives/white/texture.png");

bool isImage(const QString& path) {
    return engine::AssetPathResolver::IsImageFile(editor::toPath(path));
}

/// What a model is called by default: Hammer/model.obj is a "Hammer", not a "model".
QString modelName(const QFileInfo& source) {
    if (source.completeBaseName().compare(QLatin1String("model"), Qt::CaseInsensitive) == 0) {
        return source.dir().dirName();
    }
    return source.completeBaseName();
}

/// The image beside a model that's most likely its texture: one with the model's name, a
/// texture.*, or the only image there. Empty if there's no telling.
QString findTexture(const QFileInfo& source) {
    QFileInfoList images;
    for (const QFileInfo& file : source.dir().entryInfoList(QDir::Files)) {
        if (isImage(file.filePath())) {
            images << file;
        }
    }
    for (const QString& stem : {source.completeBaseName(), QStringLiteral("texture")}) {
        const auto found = std::ranges::find_if(images, [&stem](const QFileInfo& image) {
            return image.completeBaseName().compare(stem, Qt::CaseInsensitive) == 0;
        });
        if (found != images.end()) {
            return found->filePath();
        }
    }
    return images.size() == 1 ? images.front().filePath() : QString();
}

QDoubleSpinBox* shininessBox() {
    auto* box = new QDoubleSpinBox;
    box->setRange(1.0, 1024.0);
    box->setDecimals(1);
    box->setSingleStep(4.0);
    box->setToolTip(QDoubleSpinBox::tr("Specular exponent: higher is a smaller, sharper highlight"));
    return box;
}

}  // namespace

// The extensions CatalogKindForFile() takes as textures and as meshes.
const QString ImagePatterns = QStringLiteral("*.png *.jpg *.jpeg *.tif *.tiff *.dds *.bmp");
const QString ModelPatterns = QStringLiteral("*.fbx *.gltf *.glb *.obj *.dae *.blend *.ply");

QString imageFilter() {
    return NewAssetDialog::tr("Images (%1)").arg(ImagePatterns);
}

QString importFilter() {
    return QStringList{NewAssetDialog::tr("Images and models (%1 %2)").arg(ImagePatterns, ModelPatterns),
                       imageFilter(), NewAssetDialog::tr("Models (%1)").arg(ModelPatterns),
                       NewAssetDialog::tr("All files (*)")}
        .join(QStringLiteral(";;"));
}

/// Shows a colour; a click picks another.
class ColorButton : public QToolButton {
public:
    explicit ColorButton(const QString& title, QWidget* parent = nullptr) : QToolButton(parent) {
        setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        setIconSize({32, 14});
        connect(this, &QToolButton::clicked, this, [this, title] {
            const QColor chosen = QColorDialog::getColor(color_, window(), title);
            if (chosen.isValid()) {
                setColor(chosen);
            }
        });
    }

    [[nodiscard]] engine::Vector3 value() const {
        // A colour channel has 16 bits: 0.25 would come back as 0.2500038.
        const auto channel = [](const double value) { return static_cast<float>(std::round(value * 1000.0) / 1000.0); };
        return {channel(color_.redF()), channel(color_.greenF()), channel(color_.blueF())};
    }

    void setValue(const engine::Vector3& value) {
        const auto channel = [](const float value) { return std::clamp(value, 0.0f, 1.0f); };
        setColor(QColor::fromRgbF(channel(value.x), channel(value.y), channel(value.z)));
    }

private:
    void setColor(const QColor& color) {
        color_ = color;
        QPixmap swatch(iconSize());
        swatch.fill(color);
        setIcon(swatch);
        setText(color.name());
    }

    QColor color_;
};

NewAssetDialog::NewAssetDialog(const QString& title, const QString& folder, const QString& suffix,
                               const QString& name, QWidget* parent)
    : QDialog(parent), folder_(folder), suffix_(suffix) {
    setWindowTitle(title);
    const QString free = QFileInfo(AssetOperations::uniquePath(folder, name + suffix, suffix.isEmpty())).fileName();
    name_ = new QLineEdit(free.chopped(suffix.size()));
    name_->setMinimumWidth(320);
    name_->selectAll();
    connect(name_, &QLineEdit::textChanged, this, &NewAssetDialog::validate);

    form_ = new QFormLayout;
    form_->addRow(tr("Name"), name_);
    problem_ = new QLabel;
    problem_->setWordWrap(true);
    problem_->setEnabled(false);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_ = buttons_->button(QDialogButtonBox::Ok);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    //TODO: asset importing options

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form_);
    layout->addWidget(problem_);
    layout->addWidget(buttons_);
    validate();
}

QString NewAssetDialog::name() const {
    return name_->text().trimmed();
}

QString NewAssetDialog::path() const {
    return folder_ + QLatin1Char('/') + name() + suffix_;
}

QString NewAssetDialog::problem() const {
    const QString name = this->name();
    if (name.isEmpty()) {
        return tr("Enter a name.");
    }
    static const QRegularExpression invalid(QStringLiteral(R"([\\/:*?"<>|])"));
    if (name.contains(invalid)) {
        return tr(R"(A name can't contain \ / : * ? " < > |)");
    }
    if (name.endsWith(QLatin1Char('.'))) {
        return tr("A name can't end with a dot.");  // Windows drops it
    }
    if (const QString path = this->path();
        QFileInfo::exists(path) || QFileInfo::exists(path + QLatin1String(".meta"))) {
        return tr("%1 already exists here.").arg(name + suffix_);
    }
    return {};
}

void NewAssetDialog::validate() {
    const QString text = problem();
    problem_->setText(text);
    ok_->setEnabled(text.isEmpty());
}

void NewAssetDialog::setVisible(const bool visible) {
    if (visible && !testAttribute(Qt::WA_WState_Created)) {
        //changing default tab order
        QWidget* last = name_->previousInFocusChain();
        while (last != name_ && (buttons_->isAncestorOf(last) || !(last->focusPolicy() & Qt::TabFocus))) {
            last = last->previousInFocusChain();
        }
        setTabOrder({last, ok_, buttons_->button(QDialogButtonBox::Cancel)});
        // Its window would be made at the default size, below the layout's minimum, which Windows refuses.
        adjustSize();
    }
    QDialog::setVisible(visible);
}

ImportModelDialog::ImportModelDialog(const engine::AssetCatalog& catalog, const QString& source,
                                     const QString& folder, QWidget* parent)
    : NewAssetDialog(tr("Import Model"), folder, {}, modelName(QFileInfo(source)), parent),
      source_(cleanPath(source)) {
    const QFileInfo info(source_);
    auto* file = new QLabel(QDir::toNativeSeparators(source_));
    file->setTextInteractionFlags(Qt::TextSelectableByMouse);
    file->setWordWrap(true);
    form()->insertRow(0, tr("File"), file);

    center_ = new QCheckBox(tr("Center on load"));
    center_->setToolTip(tr("Moves the model so its bounds are centred on its origin"));
    center_->setChecked(true);
    form()->addRow(QString(), center_);

    texture_ = new QLineEdit(QDir::toNativeSeparators(findTexture(info)));
    texture_->setPlaceholderText(tr("None"));
    texture_->setClearButtonEnabled(true);
    connect(texture_, &QLineEdit::textChanged, this, &ImportModelDialog::validate);
    auto* browse = new QToolButton;
    browse->setText(QStringLiteral("…"));
    browse->setToolTip(tr("Choose an image"));
    connect(browse, &QToolButton::clicked, this, [this, info] {
        const QString current = texture_->text().trimmed();
        const QString chosen = QFileDialog::getOpenFileName(
            this, tr("Texture"), current.isEmpty() ? info.absolutePath() : current, imageFilter());
        if (!chosen.isEmpty()) {
            texture_->setText(QDir::toNativeSeparators(chosen));
        }
    });
    auto* texture_row = new QHBoxLayout;
    texture_row->addWidget(texture_, 1);
    texture_row->addWidget(browse);
    form()->addRow(tr("Texture"), texture_row);

    instance_ = new QGroupBox(tr("Create a material instance"));
    instance_->setCheckable(true);
    instance_->setChecked(true);
    instance_->setToolTip(tr("For mesh renderers: the base material with the texture as its albedo"));
    connect(instance_, &QGroupBox::toggled, this, &ImportModelDialog::validate);
    material_ = new AssetPicker(catalog, engine::CatalogAssetKind::Material);
    material_->selectPath(DefaultMaterial);
    auto* instance_form = new QFormLayout(instance_);
    instance_form->addRow(tr("Material"), material_);
    form()->addRow(instance_);
    validate();
}

ModelImport ImportModelDialog::result() const {
    ModelImport result;
    result.source = source_;
    result.name = name();
    result.center_on_load = center_->isChecked();
    if (const QString texture = texture_->text().trimmed(); !texture.isEmpty()) {
        result.texture = cleanPath(texture);
    }
    if (instance_->isChecked()) {
        result.material = material_->reference();
    }
    return result;
}

QString ImportModelDialog::problem() const {
    if (QString problem = NewAssetDialog::problem(); !problem.isEmpty()) {
        return problem;
    }
    if (const QString texture = texture_->text().trimmed();
        !texture.isEmpty() && (!QFileInfo(texture).isFile() || !isImage(texture))) {
        return tr("The texture isn't an image file.");
    }
    if (instance_->isChecked() && !material_->reference()) {
        return tr("Choose the material for the material instance.");
    }
    return {};
}

MaterialInstanceDialog::MaterialInstanceDialog(const engine::AssetCatalog& catalog, const QString& folder,
                                               const QString& name, QWidget* parent)
    : NewAssetDialog(tr("New Material Instance"), folder, QStringLiteral(".material-instance.json"), name, parent),
      catalog_(catalog) {
    material_ = new AssetPicker(catalog, engine::CatalogAssetKind::Material);
    material_->selectPath(DefaultMaterial);
    form()->addRow(tr("Material"), material_);
    albedo_ = new AssetPicker(catalog, engine::CatalogAssetKind::Texture, tr("(the material's)"));
    form()->addRow(tr("Albedo texture"), albedo_);

    // Unchecked parameters come from the material, and show its values.
    override_specular_ = new QCheckBox(tr("Specular colour"));
    specular_ = new ColorButton(tr("Specular Colour"));
    specular_->setEnabled(false);
    connect(override_specular_, &QCheckBox::toggled, specular_, &QWidget::setEnabled);
    form()->addRow(override_specular_, specular_);
    override_shininess_ = new QCheckBox(tr("Shininess"));
    shininess_ = shininessBox();
    shininess_->setEnabled(false);
    connect(override_shininess_, &QCheckBox::toggled, shininess_, &QWidget::setEnabled);
    form()->addRow(override_shininess_, shininess_);

    connect(material_, &QComboBox::currentIndexChanged, this, [this] {
        showInherited();
        validate();
    });
    showInherited();
    validate();
}

void MaterialInstanceDialog::setAlbedo(const QString& path) {
    albedo_->selectPath(path);
}

engine::MaterialInstanceDefinition MaterialInstanceDialog::definition() const {
    engine::MaterialInstanceDefinition definition;
    definition.version = 2;
    if (const auto material = material_->reference()) {
        definition.material = material->path;
        definition.materialReference = material;
    }
    if (const auto albedo = albedo_->reference()) {
        definition.parameters.albedoTexture = albedo->path;
        definition.parameters.albedoReference = albedo;
    }
    if (override_specular_->isChecked()) {
        definition.parameters.specularColor = specular_->value();
    }
    if (override_shininess_->isChecked()) {
        definition.parameters.shininess = static_cast<float>(shininess_->value());
    }
    return definition;
}

QString MaterialInstanceDialog::problem() const {
    if (QString problem = NewAssetDialog::problem(); !problem.isEmpty()) {
        return problem;
    }
    return material_->reference() ? QString() : tr("Choose a material.");
}

void MaterialInstanceDialog::showInherited() {
    engine::MaterialPBRParameters inherited;  // what a material without defaults gets
    const auto material = material_->reference();
    const engine::CatalogEntry* entry =
        material ? catalog_.Find(material->id, engine::CatalogAssetKind::Material) : nullptr;
    engine::MaterialDefinition definition;
    std::string text;
    std::string error;
    if (entry != nullptr && engine::ReadTextFile(entry->source, text, error) &&
        engine::ParseMaterialDefinition(text, definition, error)) {
        inherited.specularColor = definition.defaults.specularColor.value_or(inherited.specularColor);
        inherited.shininess = definition.defaults.shininess.value_or(inherited.shininess);
    }
    if (!override_specular_->isChecked()) {
        specular_->setValue(inherited.specularColor);
    }
    if (!override_shininess_->isChecked()) {
        shininess_->setValue(inherited.shininess);
    }
}

MaterialDialog::MaterialDialog(const engine::AssetCatalog& catalog, const QString& folder, const QString& name,
                               QWidget* parent)
    : NewAssetDialog(tr("New Material"), folder, QStringLiteral(".material.json"), name, parent) {
    shader_ = new AssetPicker(catalog, engine::CatalogAssetKind::Shader, tr("(standard shading)"));
    form()->addRow(tr("Shader"), shader_);
    // The functions a shader defines to patch the standard passes (MaterialDesc).
    gbuffer_vs_ = new QCheckBox(QStringLiteral("GBufferVS"));
    gbuffer_vs_->setToolTip(QStringLiteral("PatchGBufferVertex"));
    gbuffer_ps_ = new QCheckBox(QStringLiteral("GBufferPS"));
    gbuffer_ps_->setToolTip(QStringLiteral("PatchGBufferSurface"));
    shadow_vs_ = new QCheckBox(QStringLiteral("ShadowVS"));
    shadow_vs_->setToolTip(QStringLiteral("PatchShadowVertex"));
    auto* hooks = new QHBoxLayout;
    for (QCheckBox* hook : {gbuffer_vs_, gbuffer_ps_, shadow_vs_}) {
        connect(hook, &QCheckBox::toggled, this, &MaterialDialog::validate);
        hooks->addWidget(hook);
    }
    hooks->addStretch();
    form()->addRow(tr("Hooks"), hooks);

    albedo_ = new AssetPicker(catalog, engine::CatalogAssetKind::Texture, tr("(none)"));
    albedo_->selectPath(WhiteTexture);
    form()->addRow(tr("Albedo texture"), albedo_);
    const engine::MaterialPBRParameters defaults;
    specular_ = new ColorButton(tr("Specular Colour"));
    specular_->setValue(defaults.specularColor);
    form()->addRow(tr("Specular colour"), specular_);
    shininess_ = shininessBox();
    shininess_->setValue(defaults.shininess);
    form()->addRow(tr("Shininess"), shininess_);

    const auto shader_changed = [this] {
        const bool shader = shader_->reference().has_value();
        for (QCheckBox* hook : {gbuffer_vs_, gbuffer_ps_, shadow_vs_}) {
            hook->setEnabled(shader);
        }
        validate();
    };
    connect(shader_, &QComboBox::currentIndexChanged, this, shader_changed);
    shader_changed();
}

engine::MaterialDefinition MaterialDialog::definition() const {
    engine::MaterialDefinition definition;
    definition.version = 2;
    if (const auto shader = shader_->reference()) {
        definition.shaderFile = shader->path;
        definition.shaderReference = shader;
        const std::pair<QCheckBox*, engine::MaterialHook> hooks[] = {{gbuffer_vs_, engine::MaterialHook::GBufferVS},
                                                                      {gbuffer_ps_, engine::MaterialHook::GBufferPS},
                                                                      {shadow_vs_, engine::MaterialHook::ShadowVS}};
        for (const auto& [box, hook] : hooks) {
            if (box->isChecked()) {
                definition.hooks = definition.hooks | hook;
            }
        }
    }
    if (const auto albedo = albedo_->reference()) {
        definition.defaults.albedoTexture = albedo->path;
        definition.defaults.albedoReference = albedo;
    }
    definition.defaults.specularColor = specular_->value();
    definition.defaults.shininess = static_cast<float>(shininess_->value());
    return definition;
}

QString MaterialDialog::problem() const {
    if (QString problem = NewAssetDialog::problem(); !problem.isEmpty()) {
        return problem;
    }
    const bool hooked = gbuffer_vs_->isChecked() || gbuffer_ps_->isChecked() || shadow_vs_->isChecked();
    return shader_->reference() && !hooked ? tr("Pick the hooks the shader implements.") : QString();
}

}  // namespace content
