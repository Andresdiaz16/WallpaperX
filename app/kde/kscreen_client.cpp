// SPDX-License-Identifier: GPL-2.0-or-later

#include "kscreen_client.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>

#include <algorithm>
#include <utility>

namespace wallpaperx {

namespace {

constexpr const char* kLauncherPath = "/";
constexpr const char* kLauncherInterface = "org.kde.KScreen";
constexpr const char* kBackendPath = "/backend";
constexpr const char* kBackendInterface = "org.kde.kscreen.Backend";

QString notRunning()
{
    return QStringLiteral("KDE's display configuration service (KScreen) is not running.");
}

QString describe(const QDBusError& error)
{
    switch (error.type()) {
    case QDBusError::ServiceUnknown:
        return notRunning();
    case QDBusError::NoReply:
    case QDBusError::Timeout:
    case QDBusError::TimedOut:
        return QStringLiteral("KScreen did not answer.");
    default:
        return QStringLiteral("KScreen refused the request: %1").arg(error.message());
    }
}

// A model name read from an EDID is kept for the same output only while the same panel is
// attached, so a re-read does not fall back to connector names until getEdid answers again.
void keepKnownNames(QList<Monitor>& fresh, const QList<Monitor>& known)
{
    for (Monitor& monitor : fresh) {
        const auto was = std::ranges::find(known, monitor.outputName, &Monitor::outputName);
        if (was != known.end() && !monitor.panel.isEmpty() && was->panel == monitor.panel) {
            monitor.displayName = was->displayName;
        }
    }
}

template <typename OnReply>
void whenAnswered(QObject* context, QDBusPendingCall pending, OnReply onReply)
{
    auto* watcher = new QDBusPendingCallWatcher(std::move(pending), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                     [onReply = std::move(onReply)](QDBusPendingCallWatcher* done) {
                         done->deleteLater();
                         onReply(done->reply());
                     });
}

} // namespace

KScreenClient::KScreenClient(QDBusConnection bus, QString service, KScreenTimeouts timeouts, QObject* parent)
    : QObject(parent)
    , m_bus(std::move(bus))
    , m_service(std::move(service))
    , m_timeouts(timeouts)
{
    // The payload is ignored: re-reading through getConfig keeps one parsing path, and a burst of
    // changes costs a few calls whose superseded replies are dropped.
    m_bus.connect(m_service, QString::fromLatin1(kBackendPath), QString::fromLatin1(kBackendInterface),
                  QStringLiteral("configChanged"), this, SLOT(onBackendConfigChanged()));

    auto* launcher = new QDBusServiceWatcher(m_service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(launcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &KScreenClient::onLauncherOwnerChanged);

    m_launcherStayUp.setSingleShot(true);
    m_launcherStayUp.setInterval(m_timeouts.launcherStayUpMs);
    connect(&m_launcherStayUp, &QTimer::timeout, this, [this] { m_launcherRetrySpent = false; });
}

void KScreenClient::onLauncherOwnerChanged(const QString&, const QString& oldOwner, const QString& newOwner)
{
    // A launcher that exits takes the backend this client loaded, so its list describes nothing.
    // The launcher can stop on its own in a normal session, so it is read once more, which starts
    // it again. Losing that one too before it stayed up ends there, so a launcher that crashes on
    // start, or exits after each answer, is started at most twice.
    if (!oldOwner.isEmpty()) {
        m_launcherStayUp.stop();
        if (m_launcherRetrySpent) {
            ++m_generation;
            settle(State::Unavailable, QStringLiteral("KDE's display configuration service (KScreen) keeps stopping."), {});
        } else {
            m_launcherRetrySpent = true;
            settle(State::Loading, {}, {});
            refresh();
        }
    }
    // A new launcher sends no configChanged until asked for a backend. Only an Unavailable client
    // has no read running; otherwise this client's own read started the launcher and will read it.
    if (!newOwner.isEmpty() && m_state == State::Unavailable) {
        refresh();
    }
}

Monitor KScreenClient::selectedMonitor() const
{
    if (!m_selection) {
        return {};
    }
    const auto it = std::ranges::find(m_monitors, *m_selection, &Monitor::outputName);
    return (it == m_monitors.end() || !it->selectable()) ? Monitor{} : *it;
}

QDBusMessage KScreenClient::call(const QString& path, const QString& interface, const QString& method) const
{
    return QDBusMessage::createMethodCall(m_service, path, interface, method);
}

void KScreenClient::refresh()
{
    const quint64 generation = ++m_generation;
    if (m_state == State::Unavailable) {
        settle(State::Loading, {}, {});
    }

    QDBusMessage request = call(QString::fromLatin1(kLauncherPath), QString::fromLatin1(kLauncherInterface),
                                QStringLiteral("requestBackend"));
    request.setArguments({QString(), QVariantMap()});
    whenAnswered(this, m_bus.asyncCall(request, m_timeouts.backendMs), [this, generation](const QDBusMessage& reply) {
        if (!isCurrent(generation)) {
            return;
        }
        if (reply.type() == QDBusMessage::ErrorMessage) {
            settle(State::Unavailable, describe(QDBusError(reply)), {});
            return;
        }
        const QVariantList args = reply.arguments();
        if (args.size() != 1 || args.front().metaType().id() != QMetaType::Bool || !args.front().toBool()) {
            settle(State::Unavailable, QStringLiteral("KScreen could not load a display backend."), {});
            return;
        }
        requestConfig(generation);
    });
}

void KScreenClient::requestConfig(quint64 generation)
{
    const QDBusMessage request = call(QString::fromLatin1(kBackendPath), QString::fromLatin1(kBackendInterface),
                                      QStringLiteral("getConfig"));
    whenAnswered(this, m_bus.asyncCall(request, m_timeouts.callMs), [this, generation](const QDBusMessage& reply) {
        if (!isCurrent(generation)) {
            return;
        }
        if (reply.type() == QDBusMessage::ErrorMessage) {
            settle(State::Unavailable, describe(QDBusError(reply)), {});
            return;
        }
        const QVariantList args = reply.arguments();
        const QVariant config = args.size() == 1 ? plainVariant(args.front()) : QVariant();
        if (config.metaType().id() != QMetaType::QVariantMap) {
            settle(State::Unavailable, QStringLiteral("KScreen answered with something other than a configuration."), {});
            return;
        }
        auto monitors = parseKScreenConfig(config.toMap());
        if (!monitors) {
            settle(State::Unavailable, monitors.error(), {});
            return;
        }
        keepKnownNames(*monitors, m_monitors);
        settle(State::Ready, {}, std::move(*monitors));
        requestMonitorNames(generation);
    });
}

void KScreenClient::requestMonitorNames(quint64 generation)
{
    for (const Monitor& monitor : std::as_const(m_monitors)) {
        // An empty connector has no panel to name.
        if (monitor.kscreenId < 0 || !monitor.connected) {
            continue;
        }
        QDBusMessage request = call(QString::fromLatin1(kBackendPath), QString::fromLatin1(kBackendInterface),
                                    QStringLiteral("getEdid"));
        request.setArguments({monitor.kscreenId});
        const int id = monitor.kscreenId;
        whenAnswered(this, m_bus.asyncCall(request, m_timeouts.callMs), [this, generation, id](const QDBusMessage& reply) {
            // A failed lookup only costs the friendly name: the one already shown stays.
            if (!isCurrent(generation) || reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1) {
                return;
            }
            const auto it = std::ranges::find(m_monitors, id, &Monitor::kscreenId);
            if (it == m_monitors.end()) {
                return;
            }
            // An EDID without a name is an answer too: a name carried from the last read gives way.
            const QString name = edidMonitorName(reply.arguments().front().toByteArray());
            const QString shown = name.isEmpty() ? it->outputName : name;
            if (it->displayName == shown) {
                return;
            }
            it->displayName = shown;
            emit monitorsChanged();
            if (it->outputName == selectedOutput()) {
                emit selectionChanged();
            }
        });
    }
}

void KScreenClient::settle(State state, QString reason, QList<Monitor> monitors)
{
    const Monitor previousSelection = selectedMonitor();
    const bool stateChanges = m_state != state || m_unavailableReason != reason;
    const bool monitorsChange = m_monitors != monitors;
    m_state = state;
    m_unavailableReason = std::move(reason);
    m_monitors = std::move(monitors);
    // Not restarted by a later answer: the period measures how long this launcher has been up,
    // and a burst of configChanged reads would otherwise keep postponing the re-arm.
    if (m_state == State::Ready && m_launcherRetrySpent && !m_launcherStayUp.isActive()) {
        m_launcherStayUp.start();
    }

    // Only an answer can say a monitor is gone: Loading and Unavailable carry no list, and the
    // accessors hide a remembered choice until a Ready answer lists it again.
    if (m_state == State::Ready) {
        if (m_selection) {
            if (selectedMonitor().outputName.isEmpty()) {
                m_selection->clear();
            }
        } else if (std::ranges::count_if(m_monitors, &Monitor::selectable) == 1) {
            m_selection = std::ranges::find_if(m_monitors, &Monitor::selectable)->outputName;
        }
    }
    const bool selectionChanges = selectedMonitor() != previousSelection;

    // Emitted after every field is written, so no handler sees a state paired with the
    // previous answer's monitors.
    if (stateChanges) {
        emit stateChanged();
    }
    if (monitorsChange) {
        emit monitorsChanged();
    }
    if (selectionChanges) {
        emit selectionChanged();
    }
}

bool KScreenClient::select(const QString& outputName)
{
    const auto it = std::ranges::find(m_monitors, outputName, &Monitor::outputName);
    if (it == m_monitors.end() || !it->selectable()) {
        return false;
    }
    if (selectedOutput() != outputName) {
        m_selection = outputName;
        emit selectionChanged();
    }
    return true;
}

void KScreenClient::onBackendConfigChanged()
{
    refresh();
}

} // namespace wallpaperx
