// SPDX-License-Identifier: GPL-2.0-or-later

#include "kscreen_dbus_source.h"

#include "dbus_reply.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDBusServiceWatcher>

#include <utility>

namespace wallpaperx {

namespace {

constexpr const char* kLauncherPath = "/";
constexpr const char* kLauncherInterface = "org.kde.KScreen";
constexpr const char* kBackendPath = "/backend";
constexpr const char* kBackendInterface = "org.kde.kscreen.Backend";

QString describe(const QDBusError& error)
{
    if (isUnreachable(error)) {
        return QStringLiteral("KDE's display configuration service (KScreen) is not running.");
    }
    if (isNoAnswer(error)) {
        return QStringLiteral("KScreen did not answer.");
    }
    return QStringLiteral("KScreen refused the request: %1").arg(clamped(error.message()));
}

} // namespace

KScreenDBusSource::KScreenDBusSource(QDBusConnection bus, QString service, KScreenTimeouts timeouts, QObject* parent)
    : KScreenSource(parent)
    , m_bus(std::move(bus))
    , m_service(std::move(service))
    , m_timeouts(timeouts)
{
    // The payload is ignored: re-reading through getConfig keeps one parsing path, and a burst of
    // changes costs a few calls whose superseded replies are dropped.
    m_bus.connect(m_service, QString::fromLatin1(kBackendPath), QString::fromLatin1(kBackendInterface),
                  QStringLiteral("configChanged"), this, SLOT(onBackendConfigChanged()));

    auto* launcher = new QDBusServiceWatcher(m_service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(launcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &KScreenDBusSource::onLauncherOwnerChanged);

    m_launcherStayUp.setSingleShot(true);
    m_launcherStayUp.setInterval(m_timeouts.launcherStayUpMs);
    connect(&m_launcherStayUp, &QTimer::timeout, this, [this] { m_launcherRetrySpent = false; });
}

void KScreenDBusSource::onLauncherOwnerChanged(const QString&, const QString& oldOwner, const QString& newOwner)
{
    // A launcher that exits takes the backend this source loaded, so its list describes nothing.
    // The launcher can stop on its own in a normal session, so it is read once more, which starts
    // it again. Losing that one too before it stayed up ends there, so a launcher that crashes on
    // start, or exits after each answer, is started at most twice.
    // A name handed straight to a new owner starts nothing, so it is read again without spending
    // the retry.
    if (!oldOwner.isEmpty()) {
        m_launcherStayUp.stop();
        if (m_launcherRetrySpent && newOwner.isEmpty()) {
            ++m_generation;
            emit unavailable(QStringLiteral("KDE's display configuration service (KScreen) keeps stopping."));
        } else {
            m_launcherRetrySpent = m_launcherRetrySpent || newOwner.isEmpty();
            emit loading();
            read();
        }
    }
    // A new launcher sends no configChanged until asked for a backend.
    if (!newOwner.isEmpty()) {
        emit returned();
    }
}

QDBusMessage KScreenDBusSource::call(const QString& path, const QString& interface, const QString& method) const
{
    return QDBusMessage::createMethodCall(m_service, path, interface, method);
}

void KScreenDBusSource::read()
{
    const quint64 generation = ++m_generation;
    QDBusMessage request = call(QString::fromLatin1(kLauncherPath), QString::fromLatin1(kLauncherInterface),
                                QStringLiteral("requestBackend"));
    request.setArguments({QString(), QVariantMap()});
    whenAnswered(this, m_bus.asyncCall(request, m_timeouts.backendMs), [this, generation](const QDBusMessage& reply) {
        if (!isCurrent(generation)) {
            return;
        }
        if (reply.type() == QDBusMessage::ErrorMessage) {
            emit unavailable(describe(QDBusError(reply)));
            return;
        }
        const QVariantList args = reply.arguments();
        if (args.size() != 1 || args.front().metaType().id() != QMetaType::Bool || !args.front().toBool()) {
            emit unavailable(QStringLiteral("KScreen could not load a display backend."));
            return;
        }
        requestConfig(generation);
    });
}

void KScreenDBusSource::requestConfig(quint64 generation)
{
    const QDBusMessage request = call(QString::fromLatin1(kBackendPath), QString::fromLatin1(kBackendInterface),
                                      QStringLiteral("getConfig"));
    whenAnswered(this, m_bus.asyncCall(request, m_timeouts.callMs), [this, generation](const QDBusMessage& reply) {
        if (!isCurrent(generation)) {
            return;
        }
        if (reply.type() == QDBusMessage::ErrorMessage) {
            emit unavailable(describe(QDBusError(reply)));
            return;
        }
        const QVariantList args = reply.arguments();
        const QVariant config = args.size() == 1 ? plainVariant(args.front()) : QVariant();
        if (config.metaType().id() != QMetaType::QVariantMap) {
            emit unavailable(QStringLiteral("KScreen answered with something other than a configuration."));
            return;
        }
        const auto monitors = parseKScreenConfig(config.toMap());
        if (!monitors) {
            emit unavailable(monitors.error());
            return;
        }
        // Not restarted by a later answer: the period measures how long this launcher has been up,
        // and a burst of configChanged reads would otherwise keep postponing the re-arm.
        if (m_launcherRetrySpent && !m_launcherStayUp.isActive()) {
            m_launcherStayUp.start();
        }
        emit answered(*monitors);
        requestMonitorNames(generation, *monitors);
    });
}

void KScreenDBusSource::requestMonitorNames(quint64 generation, const QList<Monitor>& monitors)
{
    for (const Monitor& monitor : monitors) {
        // An empty connector has no panel to name.
        if (monitor.kscreenId < 0 || !monitor.connected) {
            continue;
        }
        QDBusMessage request = call(QString::fromLatin1(kBackendPath), QString::fromLatin1(kBackendInterface),
                                    QStringLiteral("getEdid"));
        request.setArguments({monitor.kscreenId});
        whenAnswered(this, m_bus.asyncCall(request, m_timeouts.callMs),
                     [this, generation, id = monitor.kscreenId](const QDBusMessage& reply) {
                         // A failed lookup only costs the friendly name: the one already shown stays.
                         if (!isCurrent(generation) || reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1) {
                             return;
                         }
                         // An EDID without a name is an answer too: a name carried from the last read gives way.
                         emit named(id, edidMonitorName(reply.arguments().front().toByteArray()));
                     });
    }
}

} // namespace wallpaperx
