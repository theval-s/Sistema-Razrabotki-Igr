#include <editor/log_panel.hpp>

#include <QAction>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStringList>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

#include <editor/log_model.hpp>

namespace editor {

namespace {

// Often enough to feel live, rarely enough that a burst of messages lands as one batch.
constexpr int DrainIntervalMs = 50;

const QString HeaderKey = QStringLiteral("log/header");

}  // namespace

LogPanel::LogPanel(QWidget* parent)
    : QWidget(parent), sink_(std::make_shared<LogSink>()), model_(new LogModel(10000, this)) {
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    view_ = new QTableView;
    view_->setModel(model_);
    view_->setFont(fixed);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setWordWrap(false);
    // Rows are one line each (the details pane has the rest): a fixed height spares the
    // view measuring every row.
    QHeaderView* rows = view_->verticalHeader();
    rows->hide();
    rows->setSectionResizeMode(QHeaderView::Fixed);
    rows->setDefaultSectionSize(QFontMetrics(fixed).height() + 4);
    setupHeader();
    view_->setContextMenuPolicy(Qt::ActionsContextMenu);

    details_ = new QPlainTextEdit;
    details_->setReadOnly(true);
    details_->setFont(fixed);
    details_->hide();

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(view_);
    splitter->addWidget(details_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(splitter);

    auto* copy = new QAction(tr("&Copy"), view_);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    connect(copy, &QAction::triggered, this, &LogPanel::copySelected);
    view_->addAction(copy);
    auto* clear = new QAction(tr("C&lear"), view_);
    connect(clear, &QAction::triggered, this, [this] {
        model_->clear();
        details_->hide();
    });
    view_->addAction(clear);

    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LogPanel::showSelected);
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, &LogPanel::showSelected);

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &LogPanel::drain);
    timer->start(DrainIntervalMs);
}

LogPanel::~LogPanel() {
    QSettings().setValue(HeaderKey, view_->horizontalHeader()->saveState());
}

void LogPanel::setupHeader() {
    QHeaderView* header = view_->horizontalHeader();
    header->setStretchLastSection(true);
    header->setHighlightSections(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    const QFontMetrics metrics(view_->font());
    const int padding = metrics.averageCharWidth() * 2;
    header->resizeSection(LogModel::TimeColumn, metrics.horizontalAdvance(QStringLiteral("00:00:00.000")) + padding);
    header->resizeSection(LogModel::LevelColumn, metrics.horizontalAdvance(QStringLiteral("critical")) + padding);
    header->resizeSection(LogModel::SourceColumn, metrics.horizontalAdvance(QStringLiteral("Physics")) + padding);
    // Empty until the engine logs through named loggers.
    header->setSectionHidden(LogModel::SourceColumn, true);
    header->restoreState(QSettings().value(HeaderKey).toByteArray());

    // Right-click the header to show or hide columns; the message always stays.
    header->setContextMenuPolicy(Qt::ActionsContextMenu);
    for (int column = 0; column < LogModel::MessageColumn; ++column) {
        auto* action = new QAction(model_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString(), header);
        action->setCheckable(true);
        action->setChecked(!header->isSectionHidden(column));
        connect(action, &QAction::toggled, header,
                [header, column](const bool shown) { header->setSectionHidden(column, !shown); });
        header->addAction(action);
    }
}

void LogPanel::drain() {
    std::vector<LogEntry> entries = sink_->takePending();
    if (entries.empty()) return;
    const QScrollBar* scroll = view_->verticalScrollBar();
    const bool follow = scroll->value() == scroll->maximum();
    model_->append(std::move(entries));
    if (follow) view_->scrollToBottom();
}

void LogPanel::showSelected() {
    const QModelIndex current = view_->selectionModel()->currentIndex();
    if (!view_->selectionModel()->isSelected(current)) {
        details_->hide();
        return;
    }
    details_->setPlainText(current.data(LogModel::TextRole).toString());
    details_->show();
}

void LogPanel::copySelected() const {
    QModelIndexList rows = view_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    // Selection order is click order; copy in log order.
    std::ranges::sort(rows, {}, &QModelIndex::row);
    QStringList lines;
    lines.reserve(rows.size());
    for (const QModelIndex& row : rows) lines.append(LogModel::format(model_->entry(row.row())));
    QGuiApplication::clipboard()->setText(lines.join(u'\n'));
}

}  // namespace editor
