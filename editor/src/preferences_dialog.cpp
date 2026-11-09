#include <editor/preferences_dialog.hpp>

#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyle>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace editor {

namespace {

enum Column { Title, Shortcut, Default };

constexpr int IdRole = Qt::UserRole;

QString text(const QList<QKeySequence>& shortcuts) {
    return QKeySequence::listToString(shortcuts, QKeySequence::NativeText);
}

}  // namespace

PreferencesDialog::PreferencesDialog(ShortcutRegistry& shortcuts, QWidget* parent)
    : QDialog(parent), shortcuts_(shortcuts) {
    setWindowTitle(tr("Preferences"));
    resize(640, 560);

    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildShortcutsPage(), tr("Shortcuts"));
    // Typing filters right away.
    tabs->currentWidget()->setFocus();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    // Enter belongs to the filter and the tree, not to Close.
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

QWidget* PreferencesDialog::buildShortcutsPage() {
    auto* page = new QWidget;

    auto* search = new QLineEdit(page);
    search->setObjectName(QStringLiteral("ShortcutFilter"));
    search->setPlaceholderText(tr("Filter by command or shortcut"));
    search->setClearButtonEnabled(true);
    page->setFocusProxy(search);
    reset_all_ = new QPushButton(tr("Reset All"), page);
    reset_all_->setObjectName(QStringLiteral("ResetAllShortcuts"));
    auto* top = new QHBoxLayout;
    top->addWidget(search);
    top->addWidget(reset_all_);

    tree_ = new QTreeWidget(page);
    tree_->setObjectName(QStringLiteral("ShortcutTree"));
    tree_->setHeaderLabels({tr("Command"), tr("Shortcut"), tr("Default")});
    tree_->setUniformRowHeights(true);
    tree_->header()->setSectionResizeMode(Title, QHeaderView::Stretch);
    tree_->header()->setSectionResizeMode(Shortcut, QHeaderView::ResizeToContents);
    tree_->header()->setSectionResizeMode(Default, QHeaderView::ResizeToContents);

    // Categories by name, commands in the order they were registered (menu order, mostly).
    QMap<QString, QTreeWidgetItem*> categories;
    for (const ShortcutRegistry::Command& command : shortcuts_.commands()) {
        QTreeWidgetItem*& category = categories[command.category];
        if (category == nullptr) {
            category = new QTreeWidgetItem(QStringList{command.category});
            category->setFlags(Qt::ItemIsEnabled);
        }
        auto* row = new QTreeWidgetItem(category, {command.title, QString(), text(command.defaults)});
        row->setData(Title, IdRole, command.id);
        commands_.insert(command.id, command);
    }
    tree_->addTopLevelItems(categories.values());
    for (QTreeWidgetItem* category : categories) {
        category->setFirstColumnSpanned(true);
    }
    tree_->expandAll();

    //thank god for QKeySequenceEdit
    primary_ = new QKeySequenceEdit(page);
    alternate_ = new QKeySequenceEdit(page);
    primary_->setObjectName(QStringLiteral("PrimaryShortcut"));
    alternate_->setObjectName(QStringLiteral("AlternateShortcut"));
    for (QKeySequenceEdit* edit : {primary_, alternate_}) {
        // One chord, so recording ends as soon as the keys are let go.
        edit->setMaximumSequenceLength(1);
        edit->setClearButtonEnabled(true);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this, &PreferencesDialog::applyEdits);
    }
    reset_ = new QPushButton(tr("Reset"), page);
    reset_->setObjectName(QStringLiteral("ResetShortcut"));
    auto* edits = new QHBoxLayout;
    edits->addWidget(new QLabel(tr("Shortcut:"), page));
    edits->addWidget(primary_, 1);
    edits->addWidget(new QLabel(tr("Alternate:"), page));
    edits->addWidget(alternate_, 1);
    edits->addWidget(reset_);

    conflict_ = new QWidget(page);
    auto* icon = new QLabel(conflict_);
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(style()->pixelMetric(QStyle::PM_SmallIconSize)));
    conflict_text_ = new QLabel(conflict_);
    conflict_text_->setObjectName(QStringLiteral("ShortcutConflict"));
    conflict_text_->setWordWrap(true);
    conflict_text_->setMinimumHeight(2 * conflict_text_->fontMetrics().lineSpacing());
    auto* conflict_layout = new QHBoxLayout(conflict_);
    conflict_layout->setContentsMargins({});
    conflict_layout->addWidget(icon, 0, Qt::AlignTop);
    conflict_layout->addWidget(conflict_text_, 1);
    // Two lines kept free, so the edits don't jump when a warning comes and goes.
    QSizePolicy policy = conflict_->sizePolicy();
    policy.setRetainSizeWhenHidden(true);
    conflict_->setSizePolicy(policy);

    auto* layout = new QVBoxLayout(page);
    layout->addLayout(top);
    layout->addWidget(tree_, 1);
    layout->addLayout(edits);
    layout->addWidget(conflict_);

    for (QPushButton* button : {reset_, reset_all_}) {
        button->setAutoDefault(false);
    }
    connect(search, &QLineEdit::textChanged, this, &PreferencesDialog::applyFilter);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &PreferencesDialog::loadEdits);
    // Double-click or Enter on a command starts recording.
    connect(tree_, &QTreeWidget::itemActivated, this, [this] {
        if (primary_->isEnabled()) primary_->setFocus();
    });
    connect(reset_, &QPushButton::clicked, this, [this] {
        shortcuts_.reset(currentId());
        loadEdits();
    });
    connect(reset_all_, &QPushButton::clicked, this, &PreferencesDialog::resetAll);
    connect(&shortcuts_, &ShortcutRegistry::changed, this, [this] {
        refreshRows();
        refreshCurrent();
    });
    refreshRows();
    loadEdits();
    return page;
}

QString PreferencesDialog::currentId() const {
    const QTreeWidgetItem* row = tree_->currentItem();
    return row != nullptr ? row->data(Title, IdRole).toString() : QString();
}

QString PreferencesDialog::conflictText(const QString& id) const {
    QStringList lines;
    for (const QKeySequence& shortcut : shortcuts_.shortcuts(id)) {
        QStringList others;
        for (const QString& other : shortcuts_.conflicts(id, shortcut)) {
            const ShortcutRegistry::Command command = commands_.value(other);
            others.push_back(tr("%1 (%2)").arg(command.title, command.category));
        }
        if (!others.isEmpty()) {
            lines.push_back(tr("%1 is also used by %2.")
                                .arg(shortcut.toString(QKeySequence::NativeText), others.join(QStringLiteral(", "))));
        }
    }
    if (!lines.isEmpty()) lines.push_back(tr("Where both apply, neither fires."));
    return lines.join(QLatin1Char('\n'));
}

void PreferencesDialog::loadEdits() {
    const QString id = currentId();
    const QList<QKeySequence> current = shortcuts_.shortcuts(id);
    // Showing isn't editing.
    const QSignalBlocker primary_blocker(primary_);
    const QSignalBlocker alternate_blocker(alternate_);
    primary_->setKeySequence(current.value(0));
    alternate_->setKeySequence(current.value(1));
    primary_->setEnabled(!id.isEmpty());
    alternate_->setEnabled(!id.isEmpty());
    refreshCurrent();
}

void PreferencesDialog::applyEdits() {
    const QString id = currentId();
    QList<QKeySequence> edited;
    for (const QKeySequenceEdit* edit : {primary_, alternate_}) {
        if (!edit->keySequence().isEmpty() && !edited.contains(edit->keySequence())) edited.push_back(edit->keySequence());
    }
    // Leaving an edit finishes it too, changed or not.
    if (!id.isEmpty() && edited != shortcuts_.shortcuts(id)) shortcuts_.setShortcuts(id, edited);
}

void PreferencesDialog::refreshRows() {
    bool modified = false;
    for (int c = 0; c < tree_->topLevelItemCount(); ++c) {
        const QTreeWidgetItem* category = tree_->topLevelItem(c);
        for (int r = 0; r < category->childCount(); ++r) {
            QTreeWidgetItem* row = category->child(r);
            const QString id = row->data(Title, IdRole).toString();
            const QList<QKeySequence> current = shortcuts_.shortcuts(id);
            const bool changed = current != commands_.value(id).defaults;
            row->setText(Shortcut, text(current));
            QFont font = row->font(Shortcut);
            font.setBold(changed);
            row->setFont(Shortcut, font);
            const QString conflict = conflictText(id);
            row->setIcon(Shortcut, conflict.isEmpty() ? QIcon() : style()->standardIcon(QStyle::SP_MessageBoxWarning));
            row->setToolTip(Shortcut, conflict);
            modified = modified || changed;
        }
    }
    reset_all_->setEnabled(modified);
}

void PreferencesDialog::refreshCurrent() {
    const QString id = currentId();
    reset_->setEnabled(!id.isEmpty() && shortcuts_.shortcuts(id) != commands_.value(id).defaults);
    const QString conflict = conflictText(id);
    conflict_text_->setText(conflict);
    conflict_->setVisible(!conflict.isEmpty());
}

void PreferencesDialog::applyFilter(const QString& text) {
    const auto matches = [&text](const QString& value) { return value.contains(text, Qt::CaseInsensitive); };
    for (int c = 0; c < tree_->topLevelItemCount(); ++c) {
        QTreeWidgetItem* category = tree_->topLevelItem(c);
        bool any = false;
        for (int r = 0; r < category->childCount(); ++r) {
            QTreeWidgetItem* row = category->child(r);
            const bool match = matches(category->text(Title)) || matches(row->text(Title)) || matches(row->text(Shortcut));
            row->setHidden(!match);
            any = any || match;
        }
        category->setHidden(!any);
    }
}

void PreferencesDialog::resetAll() {
    if (QMessageBox::question(this, tr("Reset All Shortcuts"), tr("Put every shortcut back to its default?")) !=
        QMessageBox::Yes) {
        return;
    }
    for (const QString& id : commands_.keys()) {
        shortcuts_.reset(id);
    }
    loadEdits();
}

}  // namespace editor
