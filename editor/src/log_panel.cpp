#include <editor/log_panel.hpp>

#include <QAction>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStringList>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include <editor/log_model.hpp>
#include <editor/menu_button.hpp>

namespace editor {

namespace {

// Often enough to feel live, rarely enough that a burst of messages lands as one batch.
constexpr int DrainIntervalMs = 50;

// Renamed when the default columns change, so an older saved state doesn't override them.
const QString HeaderKey = QStringLiteral("log/columns");
const QString HiddenLevelsKey = QStringLiteral("log/hiddenLevels");
const QString HiddenSourcesKey = QStringLiteral("log/hiddenSources");

/// A level toggle in the toolbar; it shows or hides all of `levels` together.
struct LevelGroup {
    const char* key;  // in QSettings
    const char* label;
    QStyle::StandardPixmap icon;
    std::array<spdlog::level::level_enum, 2> levels;
};

constexpr std::array LevelGroups{
    LevelGroup{"info", QT_TRANSLATE_NOOP("editor::LogPanel", "Info"), QStyle::SP_MessageBoxInformation,
               {spdlog::level::info, spdlog::level::info}},
    LevelGroup{"warning", QT_TRANSLATE_NOOP("editor::LogPanel", "Warnings"), QStyle::SP_MessageBoxWarning,
               {spdlog::level::warn, spdlog::level::warn}},
    LevelGroup{"error", QT_TRANSLATE_NOOP("editor::LogPanel", "Errors"), QStyle::SP_MessageBoxCritical,
               {spdlog::level::err, spdlog::level::critical}},
};

}  // namespace

LogPanel::LogPanel(QWidget* parent)
    : QWidget(parent),
      sink_(std::make_shared<LogSink>()),
      model_(new LogModel(10000, this)),
      filter_(new LogFilterModel(this)) {
    // Debug output is for the console and the log file; the panel has no toggle for it.
    sink_->set_level(spdlog::level::info);
    filter_->setSourceModel(model_);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    view_ = new QTableView;
    view_->setModel(filter_);
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

    clear_ = new QAction(style()->standardIcon(QStyle::SP_DialogResetButton), tr("C&lear"), this);
    connect(clear_, &QAction::triggered, this, [this] {
        model_->clear();
        details_->hide();
    });
    follow_ = new QAction(style()->standardIcon(QStyle::SP_ArrowDown), tr("Auto-scroll"), this);
    follow_->setCheckable(true);
    follow_->setChecked(true);
    follow_->setToolTip(tr("Keep the newest entry in view"));
    connect(follow_, &QAction::toggled, this, &LogPanel::follow);

    auto* splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(view_);
    splitter->addWidget(details_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildToolBar());
    layout->addWidget(splitter);

    auto* copy = new QAction(tr("&Copy"), view_);
    copy->setShortcut(QKeySequence::Copy);
    copy->setShortcutContext(Qt::WidgetShortcut);
    connect(copy, &QAction::triggered, this, &LogPanel::copySelected);
    view_->addAction(copy);
    view_->addAction(clear_);

    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LogPanel::showSelected);
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, &LogPanel::showSelected);

    // Only the user's scrolling switches auto-scroll: new rows and filtering move the
    // scroll bar too. The slider position is already updated when this fires; the value isn't.
    QScrollBar* scroll = view_->verticalScrollBar();
    connect(scroll, &QAbstractSlider::actionTriggered, this,
            [this, scroll] { follow_->setChecked(scroll->sliderPosition() == scroll->maximum()); });

    connect(model_, &LogModel::countsChanged, this, &LogPanel::refreshCounts);
    refreshCounts();

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &LogPanel::drain);
    timer->start(DrainIntervalMs);
}

LogPanel::~LogPanel() {
    QSettings settings;
    settings.setValue(HeaderKey, view_->horizontalHeader()->saveState());
    QStringList hidden;
    for (std::size_t i = 0; i < LevelGroups.size(); ++i) {
        if (!level_actions_[i]->isChecked()) hidden.append(QString::fromLatin1(LevelGroups[i].key));
    }
    settings.setValue(HiddenLevelsKey, hidden);
    settings.setValue(HiddenSourcesKey, filter_->hiddenSources());
}

QWidget* LogPanel::buildToolBar() {
    auto* bar = new QToolBar;
    bar->setIconSize({16, 16});
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    const QStringList hidden = QSettings().value(HiddenLevelsKey).toStringList();
    for (const LevelGroup& group : LevelGroups) {
        auto* action = bar->addAction(style()->standardIcon(group.icon), tr(group.label));
        action->setCheckable(true);
        const bool shown = !hidden.contains(QString::fromLatin1(group.key));
        action->setChecked(shown);
        action->setToolTip(tr("Show %1").arg(tr(group.label).toLower()));
        for (const spdlog::level::level_enum level : group.levels) filter_->setLevelShown(level, shown);
        connect(action, &QAction::toggled, this, [this, &group](const bool checked) {
            for (const spdlog::level::level_enum level : group.levels) filter_->setLevelShown(level, checked);
            follow();
        });
        level_actions_.push_back(action);
    }
    bar->addSeparator();

    auto* search = new QLineEdit;
    search->setPlaceholderText(tr("Search"));
    search->setClearButtonEnabled(true);
    search->setMaximumWidth(260);
    connect(search, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_->setSearchText(text);
        follow();
    });
    bar->addWidget(search);

    for (const QString& source : QSettings().value(HiddenSourcesKey).toStringList()) {
        filter_->setSourceShown(source, false);
    }
    sources_menu_ = new QMenu(this);
    bar->addWidget(makeMenuButton(tr("Sources"), sources_menu_));
    connect(model_, &LogModel::sourcesChanged, this, &LogPanel::refreshSources);
    refreshSources();

    // Right-aligned: what acts on the whole log.
    auto* spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    bar->addAction(follow_);
    bar->addAction(clear_);
    return bar;
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
    model_->append(std::move(entries));
    follow();
}

void LogPanel::follow() {
    if (follow_->isChecked()) view_->scrollToBottom();
}

void LogPanel::refreshCounts() {
    for (std::size_t i = 0; i < LevelGroups.size(); ++i) {
        const LevelGroup& group = LevelGroups[i];
        int count = model_->count(group.levels[0]);
        if (group.levels[1] != group.levels[0]) count += model_->count(group.levels[1]);
        level_actions_[i]->setText(QStringLiteral("%1 %2").arg(tr(group.label)).arg(count));
    }
}

void LogPanel::refreshSources() {
    sources_menu_->clear();
    connect(sources_menu_->addAction(tr("Show All")), &QAction::triggered, this, [this] {
        for (QAction* action : sources_menu_->actions()) action->setChecked(true);
        // Also the remembered ones that haven't logged yet.
        for (const QString& source : filter_->hiddenSources()) filter_->setSourceShown(source, true);
        follow();
    });
    sources_menu_->addSeparator();
    for (const QString& source : model_->sources()) {
        // spdlog's default logger has no name.
        auto* action = sources_menu_->addAction(source.isEmpty() ? tr("(no channel)") : source);
        action->setCheckable(true);
        action->setChecked(filter_->isSourceShown(source));
        connect(action, &QAction::toggled, this, [this, source](const bool checked) {
            filter_->setSourceShown(source, checked);
            follow();
        });
    }
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
    for (const QModelIndex& row : rows) lines.append(LogModel::format(model_->entry(filter_->mapToSource(row).row())));
    QGuiApplication::clipboard()->setText(lines.join(u'\n'));
}

}  // namespace editor
