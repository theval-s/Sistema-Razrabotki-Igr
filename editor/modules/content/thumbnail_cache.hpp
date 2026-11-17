#pragma once

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QSize>
#include <QString>
#include <QThreadPool>

namespace content {

/// Downscaled previews of image files, read on worker threads. A file read again once its
/// modification time changes; one that can't be read (DDS, a broken file) has no preview.
class ThumbnailCache : public QObject {
    Q_OBJECT

public:
    /// Previews are `size` pixel squares with the image centred, so icon views keep uniform item sizes.
    explicit ThumbnailCache(int size = 192, QObject* parent = nullptr);
    ~ThumbnailCache() override;

    /// The preview of `path` as last modified at `modified`. Null while it's read, and for no preview.
    [[nodiscard]] QIcon icon(const QString& path, const QDateTime& modified);
    /// The image's own size, once read; invalid before, and for no preview.
    [[nodiscard]] QSize imageSize(const QString& path) const;
    /// Drops reads that haven't started, e.g. for a folder no longer shown.
    void cancelPending();

signals:
    /// A read of `path` finished, with a preview or without.
    void ready(const QString& path);

private:
    struct Entry {
        QDateTime modified;
        QIcon icon;
        QSize size;
    };

    void finish(const QString& path, const QDateTime& modified, const QImage& image, const QSize& size);

    int size_;
    QCache<QString, Entry> entries_;  // cost in KiB of preview pixels
    QHash<QString, QDateTime> pending_;
    QThreadPool pool_;
};

}  // namespace content
