#pragma once

#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <vector>

#include <editor/editor_core_export.hpp>

class QAction;
class QKeyEvent;

namespace editor {

/// The ultimate place for all shortcuts with stable ids
class EDITOR_CORE_API ShortcutRegistry : public QObject {
    Q_OBJECT

public:
    struct Command {
        QString id;
        QString category;
        QString title;
        QList<QKeySequence> defaults;
        QList<QKeySequence> shortcuts;
    };

    explicit ShortcutRegistry(QObject* parent = nullptr);

    /// Gives `action` the user's shortcut for `id`, else `defaultShortcut`; the first bind of an id registers it.
    void bind(QAction* action, const QString& id, const QString& category, const QKeySequence& defaultShortcut = {});

    /// In the order they were registered.
    [[nodiscard]] QList<Command> commands() const;
    [[nodiscard]] QList<QKeySequence> shortcuts(const QString& id) const;
    /// Whether the key press is one of `id`'s single-key shortcuts, for widgets that filter keys themselves.
    [[nodiscard]] bool matches(const QString& id, const QKeyEvent& event) const;
    /// Applies to every action bound to `id` and is saved; false for an unknown id.
    bool setShortcuts(const QString& id, const QList<QKeySequence>& shortcuts);
    /// Back to the default; false for an unknown id.
    bool reset(const QString& id);
    /// Ids of the other commands on `sequence` that also work where `id` does.
    [[nodiscard]] QStringList conflicts(const QString& id, const QKeySequence& sequence) const;

signals:
    void changed(const QString& id);

private:
    struct Entry {
        Command command;
        std::vector<QPointer<QAction>> actions;
    };

    [[nodiscard]] Entry* find(const QString& id);
    [[nodiscard]] const Entry* find(const QString& id) const;

    //note: hashmap or smth?
    std::vector<Entry> entries_;
};

}  // namespace editor
