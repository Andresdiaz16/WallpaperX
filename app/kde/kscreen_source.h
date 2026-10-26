// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "kscreen_config.h"

#include <QDBusConnection>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

namespace wallpaperx {

// Where KScreenClient reads the monitors from. Only the latest read() reports, and never from
// inside read() itself; one it superseded reports nothing.
class KScreenSource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Starts a read, which ends in answered() or unavailable(). A source may report unavailable()
    // for a read that is running late and its real end afterwards.
    virtual void read() = 0;

signals:
    // What was shown describes nothing any more, and a read is under way.
    void loading();
    void unavailable(const QString& reason);
    // Every monitor carries its connector name as displayName. named() follows for each monitor
    // whose EDID was read, with the model name it gives, empty when it gives none; it applies only
    // to this answer.
    void answered(const QList<Monitor>& monitors);
    void named(int kscreenId, const QString& modelName);
    // The screens may differ from the last answer.
    void changed();
    // The service the source reads came back after leaving; worth a read if the last one failed.
    void returned();
};

// The source for the libkscreen this process loaded: the library itself when it has no other way to
// run its backend (Plasma 6.8 and later, which ship no D-Bus service), otherwise KScreen's D-Bus
// service on `bus`. The library is used only if the build found it.
std::unique_ptr<KScreenSource> systemKScreenSource(QDBusConnection bus);

} // namespace wallpaperx
