// SPDX-License-Identifier: GPL-2.0-or-later

#include "preview_image_provider.h"

#include "wpx_package.h"

#include <QBuffer>
#include <QImageIOHandler>
#include <QImageReader>

namespace wallpaperx {

PreviewImageProvider::PreviewImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image, QQuickImageProvider::ForceAsynchronousImageLoading)
{
}

QImage PreviewImageProvider::requestImage(const QString& id, QSize* size, const QSize& requestedSize)
{
    const auto location = parsePreviewImageId(id);
    if (!location) {
        return {};
    }
    auto bytes = readPackageEntry(location->archivePath, location->entryName, kMaxPreviewBytes);
    if (!bytes) {
        return {};
    }
    QBuffer buffer(&*bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);

    // size() and setScaledSize() are in the stored orientation; the image comes out turned.
    const bool turned = (reader.transformation() & QImageIOHandler::TransformationRotate90) != 0;
    const QSize stored = reader.size();
    const QSize full = turned ? stored.transposed() : stored;
    if (size != nullptr) {
        *size = full;
    }
    // Covers the requested box, so a cropped tile has no gap; never enlarged.
    if (!full.isEmpty() && (requestedSize.width() > 0 || requestedSize.height() > 0)) {
        QSize box = requestedSize;
        if (box.width() <= 0) {
            box.setWidth(full.width() * box.height() / full.height());
        } else if (box.height() <= 0) {
            box.setHeight(full.height() * box.width() / full.width());
        }
        const QSize target = full.scaled(box, Qt::KeepAspectRatioByExpanding);
        if (target.width() < full.width()) {
            reader.setScaledSize(turned ? target.transposed() : target);
        }
    }
    return reader.read();
}

} // namespace wallpaperx
