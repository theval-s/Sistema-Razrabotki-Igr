#pragma once

#include <QString>
#include <QWidget>

#include <cstddef>
#include <vector>

class QComboBox;
class QFileSystemModel;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QSlider;
class QStackedWidget;
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
    void addRoot(const QString& title, const QString& name, const QString& path, bool read_only);

    /// Shows `folder` (absolute) of given root in the item view, tree and breadcrumb.
    void navigate(std::size_t root, const QString& folder);
    /// Double-click / Enter: enters folders, opens scenes, and etc.
    void activate(const QModelIndex& index);
    void rebuildBreadcrumb();
    void applyViewMode();
    void applyFilters();
    void rescan();
    /// Shows the asset catalog's last error, if any.
    void refreshBanner();

    editor::EditorContext& context_;
    std::vector<Root> roots_;
    std::size_t root_ = 0;
    QString folder_;
    /// Set while navigate() moves the tree's current item, which would navigate again.
    bool navigating_ = false;

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
};

}  // namespace content
