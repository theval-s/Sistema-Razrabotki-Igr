#pragma once

#include <QWidget>

#include <memory>
#include <vector>

class QAction;
class QMenu;
class QPlainTextEdit;
class QTableView;

namespace editor {

class LogFilterModel;
class LogModel;
class LogSink;

/// Shows what was logged to sink(), colored by level, with a toolbar to filter by level,
/// source (the log channel) and text. While auto-scroll is on the newest entries scroll into
/// view; scrolling up turns it off and scrolling back to the bottom on again. Selecting an
/// entry shows its whole text below. The shown levels and sources and the column widths,
/// order and visibility are remembered.
class LogPanel : public QWidget {
    Q_OBJECT

public:
    explicit LogPanel(QWidget* parent = nullptr);
    ~LogPanel() override;

    /// Add to engine::log::Sinks() to show every channel's messages here. Keeps working after the panel is gone.
    [[nodiscard]] const std::shared_ptr<LogSink>& sink() const noexcept { return sink_; }

private:
    QWidget* buildToolBar();
    void setupHeader();
    /// Moves what the sink collected into the list.
    void drain();
    /// Scrolls to the newest entry if auto-scroll is on.
    void follow();
    void refreshCounts();
    /// Lists the sources seen so far in the Sources menu.
    void refreshSources();
    void showSelected();
    void copySelected() const;

    std::shared_ptr<LogSink> sink_;
    LogModel* model_ = nullptr;
    LogFilterModel* filter_ = nullptr;
    QTableView* view_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QAction* clear_ = nullptr;
    QAction* follow_ = nullptr;
    QMenu* sources_menu_ = nullptr;
    /// One per level group, in the toolbar's order.
    std::vector<QAction*> level_actions_;
};

}  // namespace editor
