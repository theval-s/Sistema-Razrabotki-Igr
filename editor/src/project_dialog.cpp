#include <editor/project_dialog.hpp>

//holy qt, but half of this can be left to transitive includes
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <string>

#include <engine/project/Project.hpp>

#include <editor/paths.hpp>

namespace editor {

namespace {

const QString RecentKey = QStringLiteral("projects/recent");
const QString LocationKey = QStringLiteral("projects/lastLocation");
constexpr qsizetype MaxRecent = 10;

QString lastLocation() {
    return QSettings().value(LocationKey, QDir::homePath()).toString();
}

}  // namespace

QStringList recentProjects() {
    //god bless qsettings
    QStringList files = QSettings().value(RecentKey).toStringList();
    files.removeIf([](const QString& file) { return !QFileInfo::exists(file); });
    return files;
}

void addRecentProject(const QString& file) {
    const QString path = QFileInfo(file).absoluteFilePath();
    QSettings settings;
    QStringList files = settings.value(RecentKey).toStringList();
    files.removeIf([&path](const QString& known) { return known.compare(path, Qt::CaseInsensitive) == 0; });
    files.prepend(path);
    settings.setValue(RecentKey, files.mid(0, MaxRecent));
    settings.setValue(LocationKey, QFileInfo(QFileInfo(path).absolutePath()).absolutePath());
}

QString createProjectInteractively(QWidget* parent) {
    const QString location =
        QFileDialog::getExistingDirectory(parent, QObject::tr("New Project: Choose Location"), lastLocation());
    if (location.isEmpty()) {
        return {};
    }
    QInputDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("New Project"));
    dialog.setLabelText(QObject::tr("Name:"));
    dialog.resize(360, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) {
        return {};
    }
    const QString name = dialog.textValue().trimmed();
    if (name.isEmpty()) {
        return {};
    }
    std::string error;
    const auto project = engine::Project::Create(toPath(location) / toPath(name), name.toStdString(), error);
    if (!project) {
        QMessageBox::warning(parent, QObject::tr("New Project"), QString::fromStdString(error));
        return {};
    }
    return toQString(project->file);
}

QString pickProjectFile(QWidget* parent) {
    return QFileDialog::getOpenFileName(parent, QObject::tr("Open Project"), lastLocation(),
                                        QObject::tr("SRI projects (*%1)").arg(engine::Project::FileExtension));
}

ProjectDialog::ProjectDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("SRI Editor"));
    resize(520, 360);

    recent_ = new QListWidget(this);
    for (const QString& file : recentProjects()) {
        const QFileInfo info(file);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1  —  %2").arg(info.completeBaseName(), QDir::toNativeSeparators(info.absolutePath())),
            recent_);
        item->setData(Qt::UserRole, file);
        item->setToolTip(QDir::toNativeSeparators(file));
    }

    auto* create = new QPushButton(tr("&New Project..."), this);
    auto* browse = new QPushButton(tr("Open &Other..."), this);
    auto* open = new QPushButton(tr("&Open"), this);
    auto* quit = new QPushButton(tr("&Quit"), this);
    open->setDefault(true);
    open->setEnabled(false);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(create);
    buttons->addWidget(browse);
    buttons->addStretch();
    buttons->addWidget(open);
    buttons->addWidget(quit);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Recent projects"), this));
    layout->addWidget(recent_);
    layout->addLayout(buttons);

    const auto selected = [this] {
        const QListWidgetItem* item = recent_->currentItem();
        return item != nullptr ? item->data(Qt::UserRole).toString() : QString();
    };
    connect(recent_, &QListWidget::currentItemChanged, open, [open, selected] { open->setEnabled(!selected().isEmpty()); });
    connect(recent_, &QListWidget::itemActivated, this, [this, selected] { choose(selected()); });
    connect(open, &QPushButton::clicked, this, [this, selected] { choose(selected()); });
    connect(create, &QPushButton::clicked, this, [this] { choose(createProjectInteractively(this)); });
    connect(browse, &QPushButton::clicked, this, [this] { choose(pickProjectFile(this)); });
    connect(quit, &QPushButton::clicked, this, &QDialog::reject);

    if (recent_->count() > 0) {
        recent_->setCurrentRow(0);
    }
}

QString ProjectDialog::projectFile() const {
    return file_;
}

void ProjectDialog::choose(const QString& file) {
    if (file.isEmpty()) {
        return;
    }
    file_ = file;
    accept();
}

}  // namespace editor
