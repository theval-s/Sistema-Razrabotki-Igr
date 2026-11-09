#include <editor/shortcut_registry.hpp>

#include <QAction>
#include <QMenu>
#include <QSettings>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace editor {

namespace {

QString settingsKey(const QString& id) {
    return QStringLiteral("shortcuts/") + id;
}

/// Without empty and repeated sequences.
QList<QKeySequence> cleaned(const QList<QKeySequence>& shortcuts) {
    QList<QKeySequence> result;
    for (const QKeySequence& shortcut : shortcuts) {
        if (!shortcut.isEmpty() && !result.contains(shortcut)) result.push_back(shortcut);
    }
    return result;
}

bool windowWide(const QAction& action) {
    return action.shortcutContext() == Qt::WindowShortcut || action.shortcutContext() == Qt::ApplicationShortcut;
}

/// The widgets whose focus makes the shortcut work; a menu only shows it.
QList<QWidget*> scopes(const QAction& action) {
    QList<QWidget*> widgets;
    for (QObject* object : action.associatedObjects()) {
        if (auto* widget = qobject_cast<QWidget*>(object); widget != nullptr && qobject_cast<QMenu*>(widget) == nullptr) {
            widgets.push_back(widget);
        }
    }
    return widgets;
}

/// Whether some focused widget would trigger both.
bool overlap(const QAction& a, const QAction& b) {
    if (windowWide(a) || windowWide(b)) return true;
    const auto covers = [](const QAction& action, const QWidget* scope, const QWidget* focus) {
        return scope == focus || (action.shortcutContext() == Qt::WidgetWithChildrenShortcut && scope->isAncestorOf(focus));
    };
    for (const QWidget* x : scopes(a)) {
        for (const QWidget* y : scopes(b)) {
            if (covers(a, x, y) || covers(b, y, x)) return true;
        }
    }
    return false;
}

}  // namespace

ShortcutRegistry::ShortcutRegistry(QObject* parent) : QObject(parent) {}

void ShortcutRegistry::bind(QAction* action, const QString& id, const QString& category,
                            const QKeySequence& defaultShortcut) {
    Entry* entry = find(id);
    if (entry == nullptr) {
        Command command{id, category, action->text().remove(QLatin1Char('&')), cleaned({defaultShortcut}), {}};
        const QSettings settings;
        command.shortcuts = settings.contains(settingsKey(id))
                                ? cleaned(QKeySequence::listFromString(settings.value(settingsKey(id)).toString(),
                                                                       QKeySequence::PortableText))
                                : command.defaults;
        entry = &entries_.emplace_back(Entry{std::move(command), {}});
    }
    entry->actions.emplace_back(action);
    action->setShortcuts(entry->command.shortcuts);
}

QList<ShortcutRegistry::Command> ShortcutRegistry::commands() const {
    QList<Command> result;
    for (const Entry& entry : entries_) {
        result.push_back(entry.command);
    }
    return result;
}

QList<QKeySequence> ShortcutRegistry::shortcuts(const QString& id) const {
    const Entry* entry = find(id);
    return entry != nullptr ? entry->command.shortcuts : QList<QKeySequence>{};
}

bool ShortcutRegistry::setShortcuts(const QString& id, const QList<QKeySequence>& shortcuts) {
    Entry* entry = find(id);
    if (entry == nullptr) return false;
    entry->command.shortcuts = cleaned(shortcuts);
    // Only what differs from the default is saved, so changed defaults reach everyone else.
    QSettings settings;
    if (entry->command.shortcuts == entry->command.defaults) {
        settings.remove(settingsKey(id));
    } else {
        settings.setValue(settingsKey(id),
                          QKeySequence::listToString(entry->command.shortcuts, QKeySequence::PortableText));
    }
    std::erase_if(entry->actions, [](const QPointer<QAction>& action) { return action.isNull(); });
    for (const QPointer<QAction>& action : entry->actions) {
        action->setShortcuts(entry->command.shortcuts);
    }
    emit changed(id);
    return true;
}

bool ShortcutRegistry::reset(const QString& id) {
    const Entry* entry = find(id);
    return entry != nullptr && setShortcuts(id, entry->command.defaults);
}

QStringList ShortcutRegistry::conflicts(const QString& id, const QKeySequence& sequence) const {
    QStringList result;
    const Entry* entry = find(id);
    if (entry == nullptr || sequence.isEmpty()) return result;
    for (const Entry& other : entries_) {
        if (other.command.id == id || !other.command.shortcuts.contains(sequence)) continue;
        const bool clash = std::ranges::any_of(entry->actions, [&](const QPointer<QAction>& a) {
            return a && std::ranges::any_of(other.actions, [&](const QPointer<QAction>& b) { return b && overlap(*a, *b); });
        });
        if (clash) result.push_back(other.command.id);
    }
    return result;
}

ShortcutRegistry::Entry* ShortcutRegistry::find(const QString& id) {
    const auto it = std::ranges::find(entries_, id, [](const Entry& entry) -> const QString& { return entry.command.id; });
    return it != entries_.end() ? &*it : nullptr;
}

const ShortcutRegistry::Entry* ShortcutRegistry::find(const QString& id) const {
    const auto it = std::ranges::find(entries_, id, [](const Entry& entry) -> const QString& { return entry.command.id; });
    return it != entries_.end() ? &*it : nullptr;
}

}  // namespace editor
