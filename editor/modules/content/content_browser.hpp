#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstddef>
#include <vector>

#include "asset_operations.hpp"

class QAction;
class QComboBox;
class QFileSystemModel;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPoint;
class QSlider;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeView;
class QWidget;

namespace editor {
class EditorContext;
}

namespace content {

class AssetFilterModel;

/// Browses the project's content (game/...) and the engine's built-in assets (engine/..., read-only):
/// a folder tree on the left, the current folder's items on the right.
class ContentBrowser : public QWidget {
    Q_OBJECT

public:
    explicit ContentBrowser(editor::EditorContext& context, QWidget* parent = nullptr);

private:
    /// A directory tree shown under a logical name, with its own model and folder tree.
    struct Root {
        QString title; // shown on its button
        QString name;  // "game" or "engine", as in asset names
        QString path;  // absolute, '/'-separated as QFileSystemModel reports paths
        bool read_only = false;
        QFileSystemModel* model = nullptr;
        AssetFilterModel* folders = nullptr;
        AssetFilterModel* contents = nullptr;
        QTreeView* tree = nullptr;
        QToolButton* button = nullptr;
    };

    QWidget* buildTopBar();
    QWidget* buildBanner();
    void buildActions();
    void addRoot(const QString& title, const QString& name, const QString& path, bool read_only);

    /// Shows `folder` (absolute) of root `root` in the item view, tree and breadcrumb.
    void navigate(std::size_t root, const QString& folder);
    /// Goes up from the shown folder until one exists, after it was deleted or moved.
    void leaveMissingFolder();
    /// Double-click / Enter: enters folders, opens scenes, hands anything else to the system.
    void activate(const QModelIndex& index);
    void rebuildBreadcrumb();
    void applyViewMode();
    void applyFilters();
    void rescan();
    /// Shows the asset catalog's last error, if any.
    void refreshBanner();

    // Commands. Paths are absolute and '/'-separated.
    [[nodiscard]] QStringList selectedPaths() const;
    /// Selects those of `paths` that are in the shown folder. False if none are.
    bool select(const QStringList& paths);
    void updateActions();
    void showItemMenu(const QPoint& pos);
    void showFolderMenu(std::size_t root, const QPoint& pos);
    void renameItem(const QString& path, const QString& name);
    void dropItems(std::size_t root, const QStringList& paths, const QString& folder, Qt::DropAction action);
    /// Creates a folder in `parent`, shows it and starts renaming it.
    void newFolder(std::size_t root, const QString& parent);
    void deleteItems(const QStringList& paths);
    void copyToClipboard(const QStringList& paths, bool cut);
    void pasteInto(const QString& folder);
    void copyAssetPaths(const QStringList& paths);
    void copyUuids(const QStringList& paths);
    void showInExplorer(const QString& path);

    editor::EditorContext& context_;
    AssetOperations operations_;
    std::vector<Root> roots_;
    std::size_t root_ = 0;
    QString folder_;
    /// Set while navigate() runs: it changes the trees' current items, which would navigate again.
    bool navigating_ = false;
    /// What the last Cut put on the clipboard; pasting exactly these moves them.
    QStringList cut_paths_;
    /// Debounces file changes in the content before adopting new files.
    QTimer* adopt_timer_ = nullptr;

    QHBoxLayout* breadcrumb_ = nullptr;
    QLabel* read_only_label_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QComboBox* kind_ = nullptr;
    QToolButton* list_mode_ = nullptr;
    QSlider* icon_size_ = nullptr;
    QWidget* banner_ = nullptr;
    QLabel* banner_text_ = nullptr;
    QStackedWidget* trees_ = nullptr;
    QListView* items_ = nullptr;

    //Better to keep them saved so we can remap keybinds easier in the future
    // but maybe TODO: extract to another class?
    QAction* open_ = nullptr;
    QAction* new_folder_ = nullptr;
    QAction* rename_ = nullptr;
    QAction* duplicate_ = nullptr;
    QAction* cut_ = nullptr;
    QAction* copy_ = nullptr;
    QAction* paste_ = nullptr;
    QAction* delete_ = nullptr;
    QAction* copy_path_ = nullptr;
    QAction* copy_uuid_ = nullptr;
    QAction* show_in_explorer_ = nullptr;
};

}  // namespace content
