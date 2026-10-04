// SPDX-License-Identifier: GPL-2.0-or-later

#include "plasma_controller.h"

#include "dbus_reply.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QRegularExpression>

#include <utility>

namespace wallpaperx {

namespace {

constexpr const char* kObjectPath = "/PlasmaShell";
constexpr const char* kInterface = "org.kde.PlasmaShell";
// knownWallpaperPlugins() lists plugins by display name only: metadata.json's KPlugin.Name.
constexpr const char* kWallpaperPluginName = "WallpaperX";
constexpr const char* kFailOpen = "[wallpaperx]";
constexpr const char* kFailClose = "[/wallpaperx]";
constexpr const char* kSwitched = "wallpaperx-switched=";

// Everything outside printable ASCII escaped, so no connector name can end the literal or the line.
QString jsString(const QString& text)
{
    QString literal = QStringLiteral("\"");
    for (const QChar c : text) {
        if (c == u'"' || c == u'\\') {
            literal += u'\\';
            literal += c;
        } else if (c.unicode() >= 0x20 && c.unicode() < 0x7f) {
            literal += c;
        } else {
            literal += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
        }
    }
    return literal + u'"';
}

// Plasma wraps a thrown error as "Error: <it> at line N" plus a backtrace, translated; the markers
// find the script's own sentence in any language.
QString scriptFailure(const QString& message)
{
    const qsizetype open = message.indexOf(QLatin1StringView(kFailOpen));
    const qsizetype close = message.indexOf(QLatin1StringView(kFailClose));
    if (open >= 0 && close > open) {
        const qsizetype from = open + qsizetype(qstrlen(kFailOpen));
        return clamped(message.mid(from, close - from)) + u'.';
    }
    return QStringLiteral("Plasma could not run the script that switches the desktop: %1")
        .arg(clamped(message.section(u'\n', 0, 0)));
}

QString describe(const QDBusError& error)
{
    if (isUnreachable(error)) {
        return QStringLiteral("Plasma's desktop shell (plasmashell) is not running.");
    }
    if (isNoAnswer(error)) {
        return QStringLiteral("Plasma's desktop shell did not answer.");
    }
    switch (error.type()) {
    case QDBusError::Disconnected:
        return QStringLiteral("There is no session bus to reach Plasma on.");
    case QDBusError::UnknownMethod:
    case QDBusError::UnknownObject:
    case QDBusError::UnknownInterface:
        return QStringLiteral("The desktop shell does not take Plasma's desktop scripts.");
    default:
        return scriptFailure(error.message());
    }
}

} // namespace

QString activationScript(const QString& connector)
{
    // screenForConnector is the shell's own ScreenPool::idForName, the index desktopForScreen and
    // a desktop's `screen` use: KWin's output order less mirrored and fake outputs, so it cannot be
    // rebuilt from KScreen's list. desktopForScreen is not used: it returns current-activity desktops
    // only, and for a screen without one it wraps a null containment, which crashes plasmashell 6.7.
    // A desktop of another activity has `screen` -1 and is matched by its saved lastScreen, the key
    // Plasma itself matches it by when that activity is shown. The plugin change is applied when the
    // script's wrappers are destroyed, after the script and before the reply, so no reloadConfig().
    return QStringLiteral(R"((function () {
    var connector = %1;
    function fail(why) { throw new Error("%2" + why + "%3"); }
    if (!(%4 in knownWallpaperPlugins()))
        fail("The WallpaperX wallpaper plugin is not installed for Plasma");
    if (typeof screenForConnector !== "function")
        fail("This Plasma cannot find a screen by its connector name");
    var screen = screenForConnector(connector);
    if (typeof screen !== "number" || screen < 0)
        fail("Plasma shows no desktop of its own on " + connector);
    var all = desktops();
    var switched = 0;
    for (var i = 0; i < all.length; ++i) {
        var d = all[i];
        var on = d.screen >= 0 ? d.screen : parseInt(d.readConfig("lastScreen", -1), 10);
        if (on !== screen)
            continue;
        d.wallpaperPlugin = %5;
        ++switched;
    }
    if (switched === 0)
        fail("Plasma has no desktop on " + connector + " yet");
    print("%6" + switched);
})();
)")
        .arg(jsString(connector), QLatin1StringView(kFailOpen), QLatin1StringView(kFailClose),
             jsString(QLatin1StringView(kWallpaperPluginName)), jsString(QLatin1StringView(kWallpaperPluginId)),
             QLatin1StringView(kSwitched));
}

std::expected<int, QString> activationResult(const QDBusMessage& reply)
{
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return std::unexpected(describe(QDBusError(reply)));
    }
    const QVariantList args = reply.arguments();
    const QString printed = args.size() == 1 ? args.front().toString() : QString();
    static const QRegularExpression count(QRegularExpression::escape(QLatin1StringView(kSwitched))
                                          + QStringLiteral("([0-9]{1,6})"));
    const QRegularExpressionMatch found = count.match(printed);
    const int switched = found.hasMatch() ? found.captured(1).toInt() : 0;
    if (switched < 1) {
        return std::unexpected(QStringLiteral("Plasma ran the script that switches the desktop but did not say it had."));
    }
    return switched;
}

PlasmaController::PlasmaController(QDBusConnection bus, QString service, int timeoutMs, QObject* parent)
    : QObject(parent)
    , m_bus(std::move(bus))
    , m_service(std::move(service))
    , m_timeoutMs(timeoutMs)
{
}

void PlasmaController::activate(QObject* context, const QString& connector,
                                std::function<void(std::expected<int, QString>)> done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(m_service, QString::fromLatin1(kObjectPath),
                                                          QString::fromLatin1(kInterface), QStringLiteral("evaluateScript"));
    message.setArguments({activationScript(connector)});
    whenAnswered(context, m_bus.asyncCall(message, m_timeoutMs),
                 [done = std::move(done)](const QDBusMessage& reply) { done(activationResult(reply)); });
}

} // namespace wallpaperx
