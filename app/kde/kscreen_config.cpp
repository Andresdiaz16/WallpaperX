// SPDX-License-Identifier: GPL-2.0-or-later

#include "kscreen_config.h"

#include <QDBusArgument>
#include <QDBusVariant>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace wallpaperx {

namespace {

std::optional<double> number(const QVariant& value)
{
    // KScreen serialises through JSON, so a whole number arrives as an integer and anything else
    // as a double, per value rather than per field.
    switch (value.metaType().id()) {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
        return value.toDouble();
    default:
        return std::nullopt;
    }
}

std::optional<QPointF> point(const QVariant& value)
{
    const QVariantMap map = value.toMap();
    const auto x = number(map.value(QStringLiteral("x")));
    const auto y = number(map.value(QStringLiteral("y")));
    if (!x || !y) {
        return std::nullopt;
    }
    return QPointF(*x, *y);
}

std::optional<QSize> size(const QVariant& value)
{
    const QVariantMap map = value.toMap();
    const auto w = number(map.value(QStringLiteral("width")));
    const auto h = number(map.value(QStringLiteral("height")));
    if (!w || !h) {
        return std::nullopt;
    }
    return QSize(static_cast<int>(*w), static_cast<int>(*h));
}

// KScreen::Output::Rotation, one bit per state.
void applyRotation(Monitor& monitor, int flags)
{
    switch (flags) {
    case 1 << 1: monitor.rotation = 90; break;
    case 1 << 2: monitor.rotation = 180; break;
    case 1 << 3: monitor.rotation = 270; break;
    case 1 << 4: monitor.flipped = true; break;
    case 1 << 5: monitor.rotation = 90; monitor.flipped = true; break;
    case 1 << 6: monitor.rotation = 180; monitor.flipped = true; break;
    case 1 << 7: monitor.rotation = 270; monitor.flipped = true; break;
    default: break;
    }
}

bool flag(const QVariantMap& map, const QString& key)
{
    const QVariant value = map.value(key);
    return value.metaType().id() == QMetaType::Bool && value.toBool();
}

// Mode ids are left out: the backend assigns them, and they can change while the panel does not.
QString describePanel(const QVariantMap& output)
{
    QStringList modes;
    for (const QVariant& entry : output.value(QStringLiteral("modes")).toList()) {
        const QVariantMap mode = entry.toMap();
        const auto pixels = size(mode.value(QStringLiteral("size")));
        const auto rate = number(mode.value(QStringLiteral("refreshRate")));
        if (pixels && rate) {
            modes.append(QStringLiteral("%1x%2@%3").arg(pixels->width()).arg(pixels->height()).arg(*rate));
        }
    }
    const auto millimetres = size(output.value(QStringLiteral("sizeMM")));
    if (modes.isEmpty() && !millimetres) {
        return {};
    }
    modes.sort();
    const QString physical = millimetres ? QStringLiteral("%1x%2mm").arg(millimetres->width()).arg(millimetres->height())
                                         : QString();
    return physical + QLatin1Char(' ') + modes.join(QLatin1Char(' '));
}

} // namespace

QVariant plainVariant(const QVariant& value)
{
    if (value.metaType() == QMetaType::fromType<QDBusVariant>()) {
        return plainVariant(value.value<QDBusVariant>().variant());
    }
    if (value.metaType() != QMetaType::fromType<QDBusArgument>()) {
        return value;
    }

    const auto arg = value.value<QDBusArgument>();
    switch (arg.currentType()) {
    case QDBusArgument::MapType: {
        QVariantMap map;
        arg.beginMap();
        while (!arg.atEnd()) {
            arg.beginMapEntry();
            const QVariant key = plainVariant(arg.asVariant());
            const QVariant entry = plainVariant(arg.asVariant());
            arg.endMapEntry();
            map.insert(key.toString(), entry);
        }
        arg.endMap();
        return map;
    }
    case QDBusArgument::ArrayType: {
        QVariantList list;
        arg.beginArray();
        while (!arg.atEnd()) {
            list.append(plainVariant(arg.asVariant()));
        }
        arg.endArray();
        return list;
    }
    case QDBusArgument::StructureType: {
        QVariantList fields;
        arg.beginStructure();
        while (!arg.atEnd()) {
            fields.append(plainVariant(arg.asVariant()));
        }
        arg.endStructure();
        return fields;
    }
    default:
        return plainVariant(arg.asVariant());
    }
}

std::expected<QList<Monitor>, QString> parseKScreenConfig(const QVariantMap& config)
{
    const QVariant outputs = config.value(QStringLiteral("outputs"));
    if (outputs.metaType().id() != QMetaType::QVariantList) {
        return std::unexpected(QStringLiteral("KScreen's reply has no list of outputs."));
    }

    QList<Monitor> monitors;
    // KScreen's id of the output each monitor copies, 0 for none; resolved once every id is known.
    QList<double> sources;
    QSet<QString> names;
    QHash<int, QString> ids;
    for (const QVariant& entry : outputs.toList()) {
        const QVariantMap output = entry.toMap();
        Monitor monitor;
        monitor.outputName = output.value(QStringLiteral("name")).toString();
        if (monitor.outputName.isEmpty()) {
            return std::unexpected(QStringLiteral("KScreen reported an output without a name."));
        }
        // Two outputs under one name would let a wallpaper land on either screen.
        if (names.contains(monitor.outputName)) {
            return std::unexpected(QStringLiteral("KScreen reported two outputs named %1.").arg(monitor.outputName));
        }
        names.insert(monitor.outputName);

        monitor.displayName = monitor.outputName;
        monitor.panel = describePanel(output);
        // The id routes each getEdid answer to one monitor, so it must be exact and unique.
        if (const QVariant id = output.value(QStringLiteral("id")); id.isValid()) {
            const auto value = number(id);
            if (!value || *value < 0 || *value > std::numeric_limits<int>::max() || *value != std::trunc(*value)) {
                return std::unexpected(QStringLiteral("KScreen gave %1 an id that is not a whole number.").arg(monitor.outputName));
            }
            monitor.kscreenId = static_cast<int>(*value);
            if (const auto seen = ids.constFind(monitor.kscreenId); seen != ids.cend()) {
                return std::unexpected(QStringLiteral("KScreen gave %1 and %2 the same id.").arg(*seen, monitor.outputName));
            }
            ids.insert(monitor.kscreenId, monitor.outputName);
        }
        monitor.enabled = flag(output, QStringLiteral("enabled"));
        monitor.connected = flag(output, QStringLiteral("connected"));
        monitor.primary = number(output.value(QStringLiteral("priority"))).value_or(0) == 1;
        applyRotation(monitor, static_cast<int>(number(output.value(QStringLiteral("rotation"))).value_or(1)));
        if (const auto scale = number(output.value(QStringLiteral("scale"))); scale && *scale > 0) {
            monitor.scale = *scale;
        }

        const auto pos = point(output.value(QStringLiteral("pos")));
        const auto pixels = size(output.value(QStringLiteral("size")));
        if (pos && pixels && !pixels->isEmpty()) {
            monitor.pixelSize = *pixels;
            monitor.geometry = QRectF(*pos, pixels->toSizeF() / monitor.scale);
        } else if (monitor.enabled && monitor.connected) {
            return std::unexpected(QStringLiteral("KScreen did not say where %1 is.").arg(monitor.outputName));
        }

        // Absent from a KScreen that predates mirroring. Present but unreadable, it could mean a
        // replica, and reading that as "none" would offer a screen with no desktop of its own.
        const QVariant source = output.value(QStringLiteral("replicationSource"));
        const auto sourceId = source.isValid() ? number(source) : std::optional<double>(0);
        if (!sourceId) {
            return std::unexpected(QStringLiteral("KScreen did not say whether %1 mirrors another screen.").arg(monitor.outputName));
        }
        sources.append(*sourceId);
        monitors.append(monitor);
    }

    for (qsizetype i = 0; i < monitors.size(); ++i) {
        const double id = sources[i];
        if (id == 0) {
            continue;
        }
        // Ids are the reply's own and unique, so a source it does not list, or a replica of
        // itself, is a reply that contradicts itself.
        const auto it = std::ranges::find_if(monitors, [id](const Monitor& m) { return id > 0 && m.kscreenId == id; });
        if (it == monitors.end() || it == monitors.begin() + i) {
            return std::unexpected(QStringLiteral("KScreen said %1 mirrors a screen, but not which one.").arg(monitors[i].outputName));
        }
        monitors[i].mirrorOf = it->outputName;
    }
    return monitors;
}

QString edidMonitorName(const QByteArray& edid)
{
    static constexpr char kHeader[] = {0, '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', 0};
    if (edid.size() < 128 || !edid.startsWith(QByteArrayView(kHeader, sizeof(kHeader)))) {
        return {};
    }
    // Four 18-byte descriptors; a display descriptor has a zero pixel clock, and tag 0xFC is the
    // name: up to 13 ASCII bytes, ended by a newline and padded with spaces.
    for (const int offset : {54, 72, 90, 108}) {
        const QByteArrayView d(edid.constData() + offset, 18);
        if (d[0] != 0 || d[1] != 0 || static_cast<unsigned char>(d[3]) != 0xfc) {
            continue;
        }
        QString name;
        for (const char c : d.sliced(5)) {
            if (c == '\n') {
                break;
            }
            if (c >= 0x20 && c < 0x7f) {
                name.append(QLatin1Char(c));
            }
        }
        return name.trimmed();
    }
    return {};
}

} // namespace wallpaperx
