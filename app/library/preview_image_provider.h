// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QQuickImageProvider>

namespace wallpaperx {

// Serves image://wallpaperx-preview/<id>, where the id comes from Wallpaper::previewSource():
// the preview image read from its wallpaper folder, scaled while decoding to the requested size. Runs on QML's image loader threads.
class PreviewImageProvider : public QQuickImageProvider {
public:
    PreviewImageProvider();

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;
};

} // namespace wallpaperx
