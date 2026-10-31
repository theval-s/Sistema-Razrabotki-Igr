#include "inspector_panel.hpp"

#include <QAction>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <utility>

#include <editor/editor_context.hpp>
#include <editor/scene_document.hpp>
#include <editor/selection.hpp>

#include "component_inspectors.hpp"
#include "property_form.hpp"

namespace {

/// Title of a component the inspector doesn't know: its C++ type name, minus the keywords.
QString typeTitle(const engine::Scene::ComponentType& type) {
    QString title = QString::fromUtf8(type.name.data(), static_cast<qsizetype>(type.name.size()));
    for (const QLatin1String keyword : {QLatin1String("struct "), QLatin1String("class "), QLatin1String("enum ")}) {
        title.remove(keyword);
    }
    return title.isEmpty() ? QObject::tr("Component %1").arg(type.id, 8, 16, QLatin1Char('0')) : title;
}

/// Where `type` is in componentInspectors(), or past its end for unknown types.
std::size_t inspectorRank(const entt::id_type type) {
    const auto inspectors = componentInspectors();
    return static_cast<std::size_t>(std::ranges::find(inspectors, type, &ComponentInspector::type) -
                                    inspectors.begin());
}

}  // namespace

InspectorPanel::InspectorPanel(editor::EditorContext& context, QWidget* parent) : QWidget(parent), context_(context) {
    header_ = new QWidget(this);
    auto* header = new QFormLayout(header_);
    name_ = new QLineEdit(header_);
    header->addRow(tr("Name"), name_);
    id_ = new QLabel(header_);
    id_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    id_->setEnabled(false);  // greyed out: it's for copying, not editing
    header->addRow(tr("ID"), id_);

    sections_ = new QWidget;
    sections_layout_ = new QVBoxLayout(sections_);
    sections_layout_->setContentsMargins(0, 0, 0, 0);
    sections_layout_->addStretch();
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(sections_);

    // Below the scroll area, so it's there however many components there are.
    add_ = new QPushButton(tr("Add Component"), this);
    add_menu_ = new QMenu(add_);
    add_->setMenu(add_menu_);
    connect(add_menu_, &QMenu::aboutToShow, this, &InspectorPanel::fillAddMenu);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(header_);
    layout->addWidget(scroll, 1);
    layout->addWidget(add_);

    connect(name_, &QLineEdit::editingFinished, this, &InspectorPanel::applyName);

    // Same reason as the hierarchy: the scene has no change notifications.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &InspectorPanel::refresh);
    timer->start(100);

    // Only on a new primary: rebuilding for the same entity would reset the rotation angles being edited.
    editor::Selection& selection = context_.selection();
    connect(&selection, &editor::Selection::changed, this, [this, &selection] {
        if (selection.primary() != entity_) {
            setEntity(selection.primary());
        }
    });
    setEntity(selection.primary());
}

void InspectorPanel::setEntity(const entt::entity entity) {
    entity_ = entity;
    // Fresh forms even if the components are the same: theirs show the previous entity.
    rebuildSections(listedComponents());
    refresh();
}

void InspectorPanel::refresh() {
    const engine::Scene& scene = context_.scene();
    if (entity_ != entt::null && !scene.IsValid(entity_)) {
        entity_ = entt::null;  // destroyed meanwhile
    }
    header_->setEnabled(entity_ != entt::null);
    add_->setEnabled(entity_ != entt::null);
    if (entity_ == entt::null) {
        const QSignalBlocker blocker(name_);
        name_->clear();
        id_->clear();
        if (!shown_types_.empty()) {
            rebuildSections({});
        }
        return;
    }

    if (!name_->hasFocus()) {
        name_->setText(QString::fromStdString(scene.GetName(entity_)));
    }
    const std::string id = scene.GetId(entity_).ToString();
    id_->setText(QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())));

    // Components come and go without the entity changing: rebuild when they do.
    const ComponentList types = listedComponents();
    if (!std::ranges::equal(types, shown_types_, {}, &engine::Scene::ComponentType::id)) {
        rebuildSections(types);
    }
    for (PropertyForm* form : forms_) {
        form->refresh();
    }
}

void InspectorPanel::applyName() {
    engine::Scene& scene = context_.scene();
    if (entity_ != entt::null && scene.IsValid(entity_)) {
        const std::string name = name_->text().toStdString();
        if (name != scene.GetName(entity_)) {
            scene.SetName(entity_, name);
        }
    }
}

InspectorPanel::ComponentList InspectorPanel::listedComponents() const {
    ComponentList types = context_.scene().GetComponentTypes(entity_);
    std::erase_if(types, [](const engine::Scene::ComponentType& type) { return isHiddenComponent(type.id); });
    // Known ones in the inspector's order, then the rest by name.
    std::ranges::sort(types, [](const engine::Scene::ComponentType& a, const engine::Scene::ComponentType& b) {
        return std::pair{inspectorRank(a.id), a.name} < std::pair{inspectorRank(b.id), b.name};
    });
    return types;
}

void InspectorPanel::rebuildSections(const ComponentList& types) {
    forms_.clear();
    shown_types_.clear();

    while (sections_layout_->count() > 1) {
        QLayoutItem* item = sections_layout_->takeAt(0);
        if (QWidget* section = item->widget()) {
            section->hide();
            section->deleteLater();
        }
        delete item;
    }
    for (const engine::Scene::ComponentType& type : types) {
        addSection(type);
    }
}

void InspectorPanel::addSection(const engine::Scene::ComponentType& type) {
    //a lot of boilerplate, yes I love doing GUIs
    const auto inspectors = componentInspectors();
    const auto known = std::ranges::find(inspectors, type.id, &ComponentInspector::type);

    auto* section = new QWidget(sections_);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* line = new QFrame(section);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    auto* title = new QToolButton(section);
    title->setText(known != inspectors.end() ? known->title : typeTitle(type));
    title->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    title->setAutoRaise(true);
    title->setCheckable(true);
    title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFont font = title->font();
    font.setBold(true);
    title->setFont(font);
    auto* title_row = new QHBoxLayout;
    title_row->addWidget(title);
    if (known != inspectors.end() && known->remove != nullptr) {
        auto* remove = new QToolButton(section);
        remove->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        remove->setAutoRaise(true);
        remove->setToolTip(tr("Remove %1").arg(known->title));
        // The rebuild that follows deletes this button later, not while it's in its signal.
        connect(remove, &QToolButton::clicked, this, [this, remover = known->remove] {
            if (entity_ != entt::null && context_.scene().IsValid(entity_)) {
                remover(context_, entity_);
                refresh();
            }
        });
        title_row->addWidget(remove);
    }
    layout->addLayout(title_row);

    QWidget* body = nullptr;
    if (known != inspectors.end()) {
        // Field edits aren't structural, so the scene's revision doesn't count them.
        auto* form = new PropertyForm([this] { context_.sceneDocument().markDirty(); }, section);
        known->build(*form, context_, entity_);
        forms_.push_back(form);
        body = form;
    } else {
        auto* label = new QLabel(tr("No inspector for this component."), section);
        label->setEnabled(false);
        label->setContentsMargins(8, 4, 8, 4);
        body = label;
    }
    layout->addWidget(body);

    const bool expanded = !collapsed_.contains(type.id);
    title->setChecked(expanded);
    title->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    body->setVisible(expanded);
    connect(title, &QToolButton::toggled, this, [this, title, body, id = type.id](const bool on) {
        title->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        body->setVisible(on);
        if (on) {
            collapsed_.erase(id);
        } else {
            collapsed_.insert(id);
        }
    });

    sections_layout_->insertWidget(sections_layout_->count() - 1, section);
    shown_types_.push_back(type.id);
}

void InspectorPanel::fillAddMenu() {
    add_menu_->clear();
    const ComponentList types = context_.scene().GetComponentTypes(entity_);
    for (const ComponentInspector& inspector : componentInspectors()) {
        if (inspector.add == nullptr ||
            std::ranges::find(types, inspector.type, &engine::Scene::ComponentType::id) != types.end()) {
            continue;
        }
        QAction* action = add_menu_->addAction(inspector.title);
        connect(action, &QAction::triggered, this, [this, adder = inspector.add, id = inspector.type] {
            if (entity_ == entt::null || !context_.scene().IsValid(entity_)) {
                return;
            }
            collapsed_.erase(id);  // show what was just added
            adder(context_, entity_);
            refresh();
        });
    }
    if (add_menu_->isEmpty()) {
        add_menu_->addAction(tr("No more components"))->setEnabled(false);
    }
}
