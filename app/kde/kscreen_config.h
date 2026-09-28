// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <expected>

namespace wallpaperx {

// One output as KScreen reports it. `outputName` is the connector name ("DP-3"), the identity
// the plugin reads from Qt's Screen.name and the daemon keys SetMonitorOutput on. Plasma's
// screen index is not carried: it is not the same order, and nothing here may depend on it.
struct Monitor {
    Q_GADGET
    QML_VALUE_TYPE(monitor)
    Q_PROPERTY(QString outputName MEMBER outputName CONSTANT)
    Q_PROPERTY(QString displayName MEMBER displayName CONSTANT)
    Q_PROPERTY(QRectF geometry MEMBER geometry CONSTANT)
    Q_PROPERTY(QSize pixelSize MEMBER pixelSize CONSTANT)
    Q_PROPERTY(int rotation MEMBER rotation CONSTANT)
    Q_PROPERTY(bool flipped MEMBER flipped CONSTANT)
    Q_PROPERTY(double scale MEMBER scale CONSTANT)
    Q_PROPERTY(bool enabled MEMBER enabled CONSTANT)
    Q_PROPERTY(bool connected MEMBER connected CONSTANT)
    Q_PROPERTY(bool primary MEMBER primary CONSTANT)
    Q_PROPERTY(QString mirrorOf MEMBER mirrorOf CONSTANT)
    Q_PROPERTY(bool selectable READ selectable CONSTANT)

public:
    // KScreen's own id for the output, valid only for the backend instance that reported it.
    // It names the output in getEdid and nothing else, so QML is not given it.
    int kscreenId = -1;
    QString outputName;
    // The monitor's model name from its EDID once known, the connector name until then.
    QString displayName;
    // The attached panel's physical size and mode list, as getConfig reports them; empty when it
    // reports neither. A different monitor on the same connector changes it, so a known model
    // name is carried to the next read only while this is unchanged. Not given to QML.
    QString panel;
    // Logical desktop coordinates: KScreen's position with the pixel size divided by the scale.
    QRectF geometry;
    // Pixels on the panel, already swapped for a quarter-turn rotation.
    QSize pixelSize;
    // Degrees counter-clockwise, as Wayland output transforms count them.
    int rotation = 0;
    bool flipped = false;
    double scale = 1.0;
    bool enabled = false;
    bool connected = false;
    bool primary = false;
    // The connector name of the output this one copies; empty unless it is a replica. A replica
    // shows its source's desktop and has none of its own.
    QString mirrorOf;

    // Only an output showing a desktop of its own can carry a wallpaper.
    bool selectable() const { return enabled && connected && !geometry.isEmpty() && mirrorOf.isEmpty(); }

    friend bool operator==(const Monitor&, const Monitor&) = default;
};

// QtDBus hands nested containers over as QDBusArgument and variants as QDBusVariant. This turns
// every level into plain QVariantMap / QVariantList / scalars, which is the shape KScreen's
// serializer produced before marshalling and what parseKScreenConfig reads.
QVariant plainVariant(const QVariant& value);

// Reads the map org.kde.kscreen.Backend.getConfig returns (after plainVariant). The error is a
// sentence for the user; a reply that cannot be trusted to name each screen once, to give each id
// to one screen, or to name the source of each replica, is refused whole.
std::expected<QList<Monitor>, QString> parseKScreenConfig(const QVariantMap& config);

// The monitor-name descriptor of a base EDID block, or empty when there is none.
QString edidMonitorName(const QByteArray& edid);

} // namespace wallpaperx
