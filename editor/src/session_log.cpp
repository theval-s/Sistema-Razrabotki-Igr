#include <editor/session_log.hpp>
#include "session_log_detail.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

#include <editor/editor_log.hpp>
#include <editor/paths.hpp>

#ifndef SRI_ENGINE_VERSION
#define SRI_ENGINE_VERSION "unknown"
#endif

namespace editor {
namespace {
QString activePath;
QString error;
bool initialized = false;
}

QString activeLogFile() { return activePath; }
QString fileLogError() { return error; }

void initializeSessionLog() {
    const QString preferred = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("logs/editor.log"));
    const QString fallbackDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const QString fallback = fallbackDir.isEmpty() ? QString() : QDir(fallbackDir).filePath(QStringLiteral("logs/editor.log"));
    detail::initializeSessionLog(preferred, fallback);
}

void detail::initializeSessionLog(const QString& preferred, const QString& fallback) {
    if (initialized) return;
    initialized = true;
    if (engine::log::AddFileSink(toPath(preferred))) {
        activePath = preferred;
    } else if (!fallback.isEmpty() && fallback != preferred && engine::log::AddFileSink(toPath(fallback))) {
        activePath = fallback;
        error = QObject::tr("Could not write the log in %1; using %2 instead.").arg(preferred, fallback);
    } else {
        error = QObject::tr("Could not open a log file at %1 or the application log location %2.")
                    .arg(preferred, fallback.isEmpty() ? QObject::tr("(unavailable)") : fallback);
    }
    if (!error.isEmpty()) editorLog().warn("{}", error.toUtf8().constData());
    editorLog().info("========== Editor session started ==========");
    editorLog().info("Engine version: {}", SRI_ENGINE_VERSION);
    editorLog().info("Executable: {}", QCoreApplication::applicationFilePath().toUtf8().constData());
}

void logSession(const QString& projectPath) {
    initializeSessionLog();
    editorLog().info("Selected project: {}", projectPath.toUtf8().constData());
    editorLog().flush();
}

}  // namespace editor
