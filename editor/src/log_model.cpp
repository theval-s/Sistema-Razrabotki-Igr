#include <editor/log_model.hpp>

#include <QColor>
#include <QDateTime>
#include <QGuiApplication>
#include <QPalette>

#include <algorithm>
#include <iterator>
#include <utility>

namespace editor {

namespace {

QString toQString(const spdlog::string_view_t text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString levelName(const spdlog::level::level_enum level) {
    return toQString(spdlog::level::to_string_view(level));
}

QString timeText(const std::chrono::system_clock::time_point time) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss.zzz"));
}

/// The first line, marked when more follow (Lua tracebacks).
QString firstLine(const QString& text) {
    const qsizetype end = text.indexOf(u'\n');
    if (end < 0) return text;
    QString line = text.first(end);
    if (line.endsWith(u'\r')) line.chop(1);
    return line + QStringLiteral(" \u2026");
}

/// Picked against the palette's base color, so they read on light and dark themes.
QVariant levelColor(const spdlog::level::level_enum level) {
    const QPalette palette = QGuiApplication::palette();
    const bool dark = palette.color(QPalette::Base).lightness() < 128;
    switch (level) {
        case spdlog::level::trace:
        case spdlog::level::debug:
            return palette.color(QPalette::PlaceholderText);
        case spdlog::level::warn:
            return dark ? QColor(230, 180, 60) : QColor(160, 100, 0);
        case spdlog::level::err:
        case spdlog::level::critical:
            return dark ? QColor(240, 100, 100) : QColor(190, 30, 30);
        default:
            return {};
    }
}

}  // namespace

std::vector<LogEntry> LogSink::takePending() {
    std::lock_guard lock(mutex_);
    return std::exchange(pending_, {});
}

void LogSink::sink_it_(const spdlog::details::log_msg& msg) {
    pending_.push_back({msg.time, msg.level, toQString(msg.logger_name), toQString(msg.payload)});
}

LogModel::LogModel(const std::size_t capacity, QObject* parent)
    : QAbstractTableModel(parent), capacity_(std::max<std::size_t>(capacity, 1)) {}

int LogModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

int LogModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant LogModel::data(const QModelIndex& index, const int role) const {
    if (!checkIndex(index, CheckIndexOption::IndexIsValid)) return {};
    const LogEntry& item = entry(index.row());
    switch (role) {
        case Qt::DisplayRole:
            switch (index.column()) {
                case TimeColumn: return timeText(item.time);
                case LevelColumn: return levelName(item.level);
                case SourceColumn: return item.source;
                case MessageColumn: return firstLine(item.text);
                default: return {};
            }
        case Qt::ToolTipRole:
        case TextRole:
            return item.text;
        case Qt::ForegroundRole:
            return levelColor(item.level);
        case LevelRole:
            return static_cast<int>(item.level);
        case SourceRole:
            return item.source;
        default:
            return {};
    }
}

QVariant LogModel::headerData(const int section, const Qt::Orientation orientation, const int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
        case TimeColumn: return tr("Time");
        case LevelColumn: return tr("Level");
        case SourceColumn: return tr("Source");
        case MessageColumn: return tr("Message");
        default: return {};
    }
}

const LogEntry& LogModel::entry(const int row) const {
    return entries_[static_cast<std::size_t>(row)];
}

int LogModel::count(const spdlog::level::level_enum level) const {
    return counts_[static_cast<std::size_t>(level)];
}

void LogModel::append(std::vector<LogEntry> entries) {
    if (entries.empty()) return;
    // A batch bigger than the whole log only keeps its tail.
    auto first = entries.begin();
    if (entries.size() > capacity_) first += static_cast<std::ptrdiff_t>(entries.size() - capacity_);
    const std::size_t incoming = static_cast<std::size_t>(std::distance(first, entries.end()));

    const std::size_t overflow = entries_.size() + incoming > capacity_ ? entries_.size() + incoming - capacity_ : 0;
    if (overflow > 0) {
        beginRemoveRows({}, 0, static_cast<int>(overflow) - 1);
        for (std::size_t i = 0; i < overflow; ++i) --counts_[static_cast<std::size_t>(entries_[i].level)];
        entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(overflow));
        endRemoveRows();
    }
    const int row = static_cast<int>(entries_.size());
    beginInsertRows({}, row, row + static_cast<int>(incoming) - 1);
    for (auto it = first; it != entries.end(); ++it) ++counts_[static_cast<std::size_t>(it->level)];
    entries_.insert(entries_.end(), std::make_move_iterator(first), std::make_move_iterator(entries.end()));
    endInsertRows();
    emit countsChanged();
}

void LogModel::clear() {
    if (entries_.empty()) return;
    beginResetModel();
    entries_.clear();
    counts_ = {};
    endResetModel();
    emit countsChanged();
}

QString LogModel::format(const LogEntry& entry) {
    return QStringLiteral("%1 [%2] %3").arg(timeText(entry.time), levelName(entry.level), entry.text);
}

LogFilterModel::LogFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    shown_.fill(true);
}

bool LogFilterModel::isLevelShown(const spdlog::level::level_enum level) const {
    return shown_[static_cast<std::size_t>(level)];
}

void LogFilterModel::setLevelShown(const spdlog::level::level_enum level, const bool shown) {
    if (isLevelShown(level) == shown) return;
    shown_[static_cast<std::size_t>(level)] = shown;
    invalidateFilter();
}

void LogFilterModel::setSearchText(const QString& text) {
    if (search_ == text) return;
    search_ = text;
    invalidateFilter();
}

bool LogFilterModel::filterAcceptsRow(const int source_row, const QModelIndex& source_parent) const {
    const QModelIndex index = sourceModel()->index(source_row, 0, source_parent);
    const auto level = static_cast<spdlog::level::level_enum>(index.data(LogModel::LevelRole).toInt());
    if (!isLevelShown(level)) return false;
    if (search_.isEmpty()) return true;
    return index.data(LogModel::TextRole).toString().contains(search_, Qt::CaseInsensitive) ||
           index.data(LogModel::SourceRole).toString().contains(search_, Qt::CaseInsensitive);
}

}  // namespace editor
