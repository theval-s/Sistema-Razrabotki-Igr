#include <QAction>
#include <QByteArray>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include <string>
#include <string_view>
#include <utility>

#include <engine/project/Project.hpp>

#include <editor/editor_context.hpp>
#include <editor/editor_log.hpp>
#include <editor/editor_module.hpp>
#include <editor/paths.hpp>
#include <editor/play_session.hpp>
#include <editor/scene_document.hpp>
#include <editor/shortcut_registry.hpp>

namespace {

    struct BuildSettings {
        QString outputParent;  // the game is built into <outputParent>/<project name>
        QString title;         // empty: the project name
        int width = 1280;
        int height = 720;
        bool fullscreen = false;
        bool captureMouse = false;
    };

    QString nameOf(const engine::Project& project) {
        return QString::fromStdString(project.name);
    }

    // Per user, per project name. Moves into the project file together with the rest of the runtime
    // settings once the project format has a place for them.
    BuildSettings loadSettings(const engine::Project& project) {
        QSettings settings;
        settings.beginGroup(QStringLiteral("build/") + nameOf(project));
        BuildSettings result;
        result.outputParent =
            settings.value(QStringLiteral("outputParent"),
                QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/Builds"))
            .toString();
        result.title = settings.value(QStringLiteral("title")).toString();
        result.width = settings.value(QStringLiteral("width"), result.width).toInt();
        result.height = settings.value(QStringLiteral("height"), result.height).toInt();
        result.fullscreen = settings.value(QStringLiteral("fullscreen"), result.fullscreen).toBool();
        result.captureMouse = settings.value(QStringLiteral("captureMouse"), result.captureMouse).toBool();
        return result;
    }

    void saveSettings(const engine::Project& project, const BuildSettings& value) {
        QSettings settings;
        settings.beginGroup(QStringLiteral("build/") + nameOf(project));
        settings.setValue(QStringLiteral("outputParent"), value.outputParent);
        settings.setValue(QStringLiteral("title"), value.title);
        settings.setValue(QStringLiteral("width"), value.width);
        settings.setValue(QStringLiteral("height"), value.height);
        settings.setValue(QStringLiteral("fullscreen"), value.fullscreen);
        settings.setValue(QStringLiteral("captureMouse"), value.captureMouse);
    }

    class BuildDialog final : public QDialog {
    public:
        BuildDialog(const engine::Project& project, BuildSettings settings, QWidget* parent)
            : QDialog(parent), name_(nameOf(project)), settings_(std::move(settings)) {
            setWindowTitle(tr("Build Game"));

            output_ = new QLineEdit(settings_.outputParent);
            auto* browse = new QPushButton(tr("Browse..."));
            auto* outputRow = new QHBoxLayout;
            outputRow->addWidget(output_, 1);
            outputRow->addWidget(browse);
            target_ = new QLabel;
            title_ = new QLineEdit(settings_.title);
            title_->setPlaceholderText(name_);
            width_ = new QSpinBox;
            width_->setRange(320, 16384);  // keep in step with ParseRuntimeSettings
            width_->setValue(settings_.width);
            height_ = new QSpinBox;
            height_->setRange(320, 16384);
            height_->setValue(settings_.height);
            auto* sizeRow = new QHBoxLayout;
            sizeRow->addWidget(width_);
            sizeRow->addWidget(new QLabel(QStringLiteral("x")));
            sizeRow->addWidget(height_);
            sizeRow->addStretch(1);
            fullscreen_ = new QCheckBox(tr("Fullscreen (borderless)"));
            fullscreen_->setChecked(settings_.fullscreen);
            captureMouse_ = new QCheckBox(tr("Hide and capture the mouse"));
            captureMouse_->setChecked(settings_.captureMouse);

            auto* form = new QFormLayout;
            form->addRow(tr("Output folder"), outputRow);
            form->addRow(QString(), target_);
            form->addRow(tr("Window title"), title_);
            form->addRow(tr("Window size"), sizeRow);
            form->addRow(QString(), fullscreen_);
            form->addRow(QString(), captureMouse_);

            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
            buttons->button(QDialogButtonBox::Ok)->setText(tr("Build"));
            auto* layout = new QVBoxLayout(this);
            layout->addLayout(form);
            layout->addWidget(buttons);

            connect(browse, &QPushButton::clicked, this, [this] {
                const QString dir = QFileDialog::getExistingDirectory(this, tr("Output folder"), output_->text());
                if (!dir.isEmpty()) output_->setText(dir);
                });
            connect(output_, &QLineEdit::textChanged, this, [this] { refreshTarget(); });
            connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
            refreshTarget();
        }

        [[nodiscard]] const BuildSettings& settings() const { return settings_; }

        void accept() override {
            const QString parent = output_->text().trimmed();
            if (parent.isEmpty()) {
                QMessageBox::warning(this, tr("Build Game"), tr("Choose an output folder."));
                return;
            }
            settings_ = { parent,           title_->text().trimmed(), width_->value(),
                         height_->value(), fullscreen_->isChecked(), captureMouse_->isChecked() };
            QDialog::accept();
        }

    private:
        void refreshTarget() {
            target_->setText(tr("The game will be built into %1").arg(QDir::toNativeSeparators(QDir(output_->text()).absoluteFilePath(name_))));
        }

        QString name_;
        BuildSettings settings_;
        QLineEdit* output_ = nullptr;
        QLabel* target_ = nullptr;
        QLineEdit* title_ = nullptr;
        QSpinBox* width_ = nullptr;
        QSpinBox* height_ = nullptr;
        QCheckBox* fullscreen_ = nullptr;
        QCheckBox* captureMouse_ = nullptr;
    };

    class BuildModule final : public editor::IEditorModule {
    public:
        [[nodiscard]] std::string_view name() const override {
            return "Build";
        }

        void startup(editor::EditorContext& context) override {
            context_ = &context;
            action_ = context.menu(QObject::tr("&Project"))->addAction(QObject::tr("Build Game..."));
            // No default key: the user binds one in the shortcut settings.
            context.shortcuts().bind(action_, QStringLiteral("project.build"), QObject::tr("Project"));
            QObject::connect(action_, &QAction::triggered, action_, [this] { build(); });
            // Play owns the scene document while it runs.
            QObject::connect(&context.playSession(), &editor::PlaySession::playingChanged, action_,
                &QAction::setDisabled);
        }

        void shutdown() override {
            if (process_) {
                process_->disconnect();  // no finish() on the way out
                process_->kill();
                process_->waitForFinished(3000);
                delete process_;
                process_ = nullptr;
            }
            delete action_;
        }

    private:
        void build() {
            if (process_) return;  // one build at a time
            const engine::Project& project = context_->project();
            QWidget* window = context_->mainWindow();

            const QString tool = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("sri-build.exe"));
            if (!QFileInfo::exists(tool)) {
                QMessageBox::warning(window, QObject::tr("Build Game"),
                    QObject::tr("sri-build.exe was not found next to the editor.\n"
                        "Build the sri-build and sri_runtime_template targets."));
                return;
            }
            if (project.startupScene.empty()) {
                QMessageBox::warning(window, QObject::tr("Build Game"),
                    QObject::tr("The project has no startup scene. Set one first."));
                return;
            }
            if (!context_->sceneDocument().maybeSave(window)) return;  // sri-build reads files from disk

            BuildDialog dialog(project, loadSettings(project), window);
            if (dialog.exec() != QDialog::Accepted) return;
            saveSettings(project, dialog.settings());
            start(tool, project, dialog.settings());
        }

        void start(const QString& tool, const engine::Project& project, const BuildSettings& settings) {
            name_ = nameOf(project);
            output_ = QDir(settings.outputParent).absoluteFilePath(name_);

            QStringList args{ QStringLiteral("--project"), editor::toQString(project.file),
                             QStringLiteral("--out"),     QDir::toNativeSeparators(output_),
                             QStringLiteral("--width"),   QString::number(settings.width),
                             QStringLiteral("--height"),  QString::number(settings.height) };
            if (!settings.title.isEmpty()) args << QStringLiteral("--title") << settings.title;
            if (settings.fullscreen) args << QStringLiteral("--fullscreen");
            if (settings.captureMouse) args << QStringLiteral("--capture-mouse");

            errors_.clear();
            warnings_.clear();
            buffer_.clear();

            process_ = new QProcess(action_);  // dies with the action in shutdown()
            process_->setProgram(tool);
            process_->setArguments(args);
            process_->setWorkingDirectory(QCoreApplication::applicationDirPath());
            process_->setProcessChannelMode(QProcess::MergedChannels);
            QObject::connect(process_, &QProcess::readyReadStandardOutput, process_, [this] { consumeOutput(false); });
            QObject::connect(process_, &QProcess::errorOccurred, process_, [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) finish(-1, QObject::tr("sri-build.exe could not be started."));
                });
            QObject::connect(process_, &QProcess::finished, process_, [this](int code, QProcess::ExitStatus status) {
                consumeOutput(true);
                finish(status == QProcess::NormalExit ? code : -1, {});
                });
            context_->showStatus(QObject::tr("Building %1...").arg(name_), 0);
            process_->start();
        }

        // sri-build prints one "info:" / "warning:" / "error:" line per message, in UTF-8.
        void consumeOutput(const bool flush) {
            buffer_ += process_->readAllStandardOutput();
            for (qsizetype end = buffer_.indexOf('\n'); end >= 0; end = buffer_.indexOf('\n')) {
                handleLine(QString::fromUtf8(buffer_.left(end)).trimmed());
                buffer_.remove(0, end + 1);
            }
            if (flush && !buffer_.trimmed().isEmpty()) {
                handleLine(QString::fromUtf8(buffer_).trimmed());
                buffer_.clear();
            }
        }

        void handleLine(const QString& line) {
            if (line.isEmpty()) return;
            if (line.startsWith(QStringLiteral("error: "))) {
                errors_ << line.mid(7);
                editor::editorLog().error("[build] {}", line.mid(7).toStdString());
            }
            else if (line.startsWith(QStringLiteral("warning: "))) {
                warnings_ << line.mid(9);
                editor::editorLog().warn("[build] {}", line.mid(9).toStdString());
            }
            else {
                editor::editorLog().info("[build] {}", (line.startsWith(QStringLiteral("info: ")) ? line.mid(6) : line).toStdString());
            }
        }

        void finish(const int exitCode, const QString& startError) {
            if (!process_) return;
            process_->deleteLater();
            process_ = nullptr;
            QWidget* window = context_->mainWindow();
            const QString native = QDir::toNativeSeparators(output_);

            if (exitCode != 0) {
                context_->showStatus(QObject::tr("Build failed, see the Log."), 10000);
                const QString reason = !startError.isEmpty() ? startError
                    : errors_.isEmpty() ? QObject::tr("See the Log for details.")
                    : errors_.join(QLatin1Char('\n'));
                QMessageBox::warning(window, QObject::tr("Build Game"), reason);
                return;
            }

            context_->showStatus(QObject::tr("Game built: %1").arg(native), 10000);
            QMessageBox box(QMessageBox::Information, QObject::tr("Build Game"),
                QObject::tr("The game was built:\n%1").arg(native), QMessageBox::Close, window);
            if (!warnings_.isEmpty()) {
                box.setInformativeText(QObject::tr("%1 warning(s).").arg(warnings_.size()));
                box.setDetailedText(warnings_.join(QLatin1Char('\n')));
            }
            QPushButton* openFolder = box.addButton(QObject::tr("Open Folder"), QMessageBox::ActionRole);
            QPushButton* run = box.addButton(QObject::tr("Run Game"), QMessageBox::ActionRole);
            box.exec();
            if (box.clickedButton() == openFolder) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(output_));
            }
            else if (box.clickedButton() == run) {
                QProcess::startDetached(QDir(output_).filePath(name_ + QStringLiteral(".exe")), {}, output_);
            }
        }

        editor::EditorContext* context_ = nullptr;
        QAction* action_ = nullptr;
        QProcess* process_ = nullptr;
        QString name_;
        QString output_;
        QByteArray buffer_;
        QStringList errors_;
        QStringList warnings_;
    };

}  // namespace

SRI_EDITOR_MODULE(BuildModule)