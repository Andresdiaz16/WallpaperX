// SPDX-License-Identifier: GPL-2.0-or-later

#include "plasma_controller.h"

#include "dbus_reply.h"
#include "plasma_activation.h"

#include <QDBusError>
#include <QDBusMessage>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace wallpaperx {

namespace {

QString scriptFailure(const QString& message)
{
    if (const std::optional<std::string> thrown = plasmaActivationFailure(message.toStdString())) {
        return clamped(QString::fromStdString(*thrown)) + u'.';
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
    const QStringView text(connector);
    return QString::fromStdString(plasmaActivationScript(std::u16string_view(text.utf16(), size_t(text.size()))));
}

std::expected<int, QString> activationResult(const QDBusMessage& reply)
{
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return std::unexpected(describe(QDBusError(reply)));
    }
    const QVariantList args = reply.arguments();
    const QString printed = args.size() == 1 ? args.front().toString() : QString();
    const int switched = plasmaActivationSwitched(printed.toStdString());
    if (switched < 1) {
        return std::unexpected(QStringLiteral("Plasma ran the script that switches the desktop but did not say it had."));
    }
    return switched;
}

PlasmaController::PlasmaController(QDBusConnection bus, QObject* parent)
    : PlasmaController(std::move(bus), QString::fromLatin1(kPlasmaShellService), kPlasmaScriptTimeoutMs, parent)
{
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
    QDBusMessage message = QDBusMessage::createMethodCall(m_service, QString::fromLatin1(kPlasmaShellPath),
                                                          QString::fromLatin1(kPlasmaShellInterface),
                                                          QStringLiteral("evaluateScript"));
    message.setArguments({activationScript(connector)});
    whenAnswered(context, m_bus.asyncCall(message, m_timeoutMs),
                 [done = std::move(done)](const QDBusMessage& reply) { done(activationResult(reply)); });
}

} // namespace wallpaperx
