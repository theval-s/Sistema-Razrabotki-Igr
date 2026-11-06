#pragma once

#include <QWidget>

#include <memory>

class QPlainTextEdit;
class QTableView;

namespace editor {

class LogModel;
class LogSink;

/// Shows what was logged to sink(), colored by level. The newest entries scroll into view
/// unless the list was scrolled up; selecting one shows its whole text below. The column
/// widths, order and visibility are remembered.
class LogPanel : public QWidget {
    Q_OBJECT

public:
    explicit LogPanel(QWidget* parent = nullptr);
    ~LogPanel() override;

    /// Add to a logger to show its messages here. Keeps working after the panel is gone.
    [[nodiscard]] const std::shared_ptr<LogSink>& sink() const noexcept { return sink_; }

private:
    /// Moves what the sink collected into the list.
    void drain();
    void showSelected();
    void copySelected() const;
    void setupHeader();

    std::shared_ptr<LogSink> sink_;
    LogModel* model_ = nullptr;
    QTableView* view_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
};

}  // namespace editor
