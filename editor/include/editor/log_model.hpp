#pragma once

#include <QAbstractTableModel>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/sinks/base_sink.h>

namespace editor {

struct LogEntry {
    std::chrono::system_clock::time_point time;
    spdlog::level::level_enum level = spdlog::level::info;
    /// The logger's name; empty for the default logger.
    QString source;
    QString text;
};

/// Collects messages from whatever thread logs, for the GUI thread to take in batches.
/// It never touches Qt objects, so it may outlive the Log panel.
class LogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    /// What was logged since the last call, oldest first.
    [[nodiscard]] std::vector<LogEntry> takePending();

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    std::vector<LogEntry> pending_;
};

/// The most recent log entries, oldest first; past capacity() the oldest are dropped.
class LogModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { TimeColumn, LevelColumn, SourceColumn, MessageColumn, ColumnCount };

    /// The same in every column.
    enum Role {
        LevelRole = Qt::UserRole,  // spdlog::level::level_enum as int
        SourceRole,
        /// The whole message; MessageColumn shows only its first line.
        TextRole,
    };

    explicit LogModel(std::size_t capacity = 10000, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    [[nodiscard]] const LogEntry& entry(int row) const;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    /// How many entries of `level` the model holds, shown or not.
    [[nodiscard]] int count(spdlog::level::level_enum level) const;
    /// Every source seen so far, sorted; clear() keeps them.
    [[nodiscard]] const QStringList& sources() const noexcept { return sources_; }

    void append(std::vector<LogEntry> entries);
    void clear();

    /// "12:01:33.120 [warning] text", the whole text, for copying.
    [[nodiscard]] static QString format(const LogEntry& entry);

signals:
    /// After entries were added, dropped or cleared.
    void countsChanged();
    /// After entries from a new source arrived.
    void sourcesChanged();

private:
    std::deque<LogEntry> entries_;
    std::size_t capacity_;
    std::array<int, spdlog::level::n_levels> counts_{};
    QStringList sources_;
};

/// Hides LogModel rows by level, by source and by text.
class LogFilterModel : public QSortFilterProxyModel {
    Q_OBJECT

public:
    explicit LogFilterModel(QObject* parent = nullptr);

    [[nodiscard]] bool isLevelShown(spdlog::level::level_enum level) const;
    void setLevelShown(spdlog::level::level_enum level, bool shown);
    /// Sources are shown unless hidden here, including ones not seen yet.
    [[nodiscard]] bool isSourceShown(const QString& source) const;
    void setSourceShown(const QString& source, bool shown);
    [[nodiscard]] QStringList hiddenSources() const { return hidden_sources_.values(); }
    /// Rows whose message or source contains `text`, ignoring case; empty shows all.
    void setSearchText(const QString& text);

protected:
    [[nodiscard]] bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

private:
    std::array<bool, spdlog::level::n_levels> shown_;
    QSet<QString> hidden_sources_;
    QString search_;
};

}  // namespace editor
