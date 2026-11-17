#include "thumbnail_cache.hpp"

#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QPixmap>

#include <algorithm>

namespace content {

namespace {

constexpr int CacheKiB = 64 * 1024;

/// The image at `path` fitted into a transparent `size` square; null if it can't be read.
QImage readPreview(const QString& path, const int size, QSize& full) {
    QImageReader reader(path);
    full = reader.size();
    if (full.isValid() && (full.width() > size || full.height() > size)) {
        reader.setScaledSize(full.scaled(size, size, Qt::KeepAspectRatio).expandedTo({1, 1}));
    }
    QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    // Small images grow without smoothing, so pixel art stays crisp.
    const QSize fit = image.size().scaled(size, size, Qt::KeepAspectRatio).expandedTo({1, 1});
    if (fit != image.size()) {
        image = image.scaled(fit, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }
    QImage square(size, size, QImage::Format_ARGB32_Premultiplied);
    square.fill(Qt::transparent);
    QPainter painter(&square);
    painter.drawImage((size - fit.width()) / 2, (size - fit.height()) / 2, image);
    return square;
}

}  // namespace

ThumbnailCache::ThumbnailCache(const int size, QObject* parent) : QObject(parent), size_(size), entries_(CacheKiB) {
    pool_.setMaxThreadCount(2);  // big PNGs decode whole; leave the other cores to the engine
}

ThumbnailCache::~ThumbnailCache() {
    // Workers post their results to this object, so none may outlive it.
    pool_.clear();
    pool_.waitForDone();
}

QIcon ThumbnailCache::icon(const QString& path, const QDateTime& modified) {
    if (const Entry* entry = entries_.object(path); entry != nullptr && entry->modified == modified) {
        return entry->icon;
    }
    if (const auto it = pending_.constFind(path); it != pending_.cend() && *it == modified) {
        return {};
    }
    pending_.insert(path, modified);
    pool_.start([this, path, modified, size = size_] {
        QSize full;
        const QImage image = readPreview(path, size, full);
        QMetaObject::invokeMethod(this, [this, path, modified, image, full] { finish(path, modified, image, full); },
                                  Qt::QueuedConnection);
    });
    return {};
}

QSize ThumbnailCache::imageSize(const QString& path) const {
    const Entry* entry = entries_.object(path);
    return entry != nullptr ? entry->size : QSize();
}

void ThumbnailCache::cancelPending() {
    pool_.clear();
    pending_.clear();
}

void ThumbnailCache::finish(const QString& path, const QDateTime& modified, const QImage& image, const QSize& size) {
    if (const auto it = pending_.constFind(path); it != pending_.cend()) {
        if (*it != modified) {
            return;  // the file changed while it was read; the newer read reports
        }
        pending_.erase(it);
    }
    auto* entry = new Entry{modified, {}, {}};
    if (!image.isNull()) {
        entry->icon = QIcon(QPixmap::fromImage(image));
        entry->size = size;
    }
    const auto cost = static_cast<int>(std::max<qsizetype>(image.sizeInBytes() / 1024, 1));
    entries_.insert(path, entry, cost);
    emit ready(path);
}

}  // namespace content
