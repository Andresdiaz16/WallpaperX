// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Replies shaped the way KScreen's launcher builds them: its serializer writes JSON and sends
// QJsonObject::toVariantMap(), so a whole number is a qlonglong and anything else a double, per
// value. Shared by the parser test and the bus test so both read one shape.

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <optional>

namespace wallpaperx::apptest {

// The real reply from the developer's machine, sanitized: DP-4 listed first (id 1, primary, at
// 1440,761) and DP-3 second (id 2, portrait at the origin). Empty when the file cannot be read.
inline std::optional<QVariantMap> realFixture() {
    QFile file(QStringLiteral(WALLPAPERX_APP_TEST_DATA "/kscreen_getconfig.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    return doc.object().toVariantMap();
}

inline QVariantMap withOutputs(QVariantMap config, const QVariantList& outputs) {
    config.insert(QStringLiteral("outputs"), outputs);
    return config;
}

inline QVariantList reversed(QVariantList list) {
    std::reverse(list.begin(), list.end());
    return list;
}

struct OutputSpec {
    QString name;
    qlonglong id = 1;
    QVariant x = qlonglong(0);
    QVariant y = qlonglong(0);
    QVariant width = qlonglong(1920);
    QVariant height = qlonglong(1080);
    bool enabled = true;
    bool connected = true;
    qlonglong priority = 2;
    qlonglong rotation = 1;
    QVariant scale = qlonglong(1);
    bool withPos = true;
    bool withSize = true;
    // KScreen's id of the output this one copies; left out of the map when invalid.
    QVariant replicationSource;
};

inline QVariantMap output(const OutputSpec& s) {
    QVariantMap map{
        {QStringLiteral("name"), s.name},
        {QStringLiteral("id"), s.id},
        {QStringLiteral("enabled"), s.enabled},
        {QStringLiteral("connected"), s.connected},
        {QStringLiteral("priority"), s.priority},
        {QStringLiteral("rotation"), s.rotation},
        {QStringLiteral("scale"), s.scale},
    };
    if (s.withPos) {
        map.insert(QStringLiteral("pos"), QVariantMap{{QStringLiteral("x"), s.x}, {QStringLiteral("y"), s.y}});
    }
    if (s.withSize) {
        map.insert(QStringLiteral("size"),
                   QVariantMap{{QStringLiteral("width"), s.width}, {QStringLiteral("height"), s.height}});
    }
    if (s.replicationSource.isValid()) {
        map.insert(QStringLiteral("replicationSource"), s.replicationSource);
    }
    return map;
}

inline QVariantMap configOf(const QVariantList& outputs) {
    return QVariantMap{{QStringLiteral("outputs"), outputs}};
}

// A 128-byte base EDID block: the fixed header, and `name` in a monitor-name (0xFC) display
// descriptor at `slot` (0-3, bytes 54 + 18 * slot), ended by a newline and padded with spaces.
inline QByteArray edidNamed(const QByteArray& name, int slot = 1) {
    QByteArray edid(128, '\0');
    const char header[] = {0, '\xff', '\xff', '\xff', '\xff', '\xff', '\xff', 0};
    edid.replace(0, 8, QByteArray(header, 8));
    const int at = 54 + 18 * slot;
    edid[at + 3] = '\xfc';
    QByteArray text = name.left(13);
    if (text.size() < 13) {
        text.append('\n');
    }
    text = text.leftJustified(13, ' ');
    edid.replace(at + 5, 13, text);
    return edid;
}

} // namespace wallpaperx::apptest
