// SPDX-License-Identifier: GPL-2.0-or-later

#include "kscreen_client.h"

#include "dbus_reply.h"

#include <QDBusError>
#include <QDBusMessage>
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
    if (isUnreachable(error)) {
        return notRunning();
    }
    if (isNoAnswer(error)) {
        return QStringLiteral("KScreen did not answer.");
    }
    return QStringLiteral("KScreen refused the request: %1").arg(clamped(error.message()));
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
    // A name handed straight to a new owner starts nothing, so it is read again without spending
    // the retry.
    if (!oldOwner.isEmpty()) {
        m_launcherStayUp.stop();
        if (m_launcherRetrySpent && newOwner.isEmpty()) {
            ++m_generation;
            settle(State::Unavailable, QStringLiteral("KDE's display configuration service (KScreen) keeps stopping."));
        } else {
            m_launcherRetrySpent = m_launcherRetrySpent || newOwner.isEmpty();
            settle(State::Loading, {});
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
    if (!m_selection || m_state != State::Ready) {
        return {};
    }
    const auto it = std::ranges::find(m_answer, *m_selection, &Monitor::outputName);
    return (it == m_answer.end() || !it->selectable()) ? Monitor{} : *it;
}

QDBusMessage KScreenClient::call(const QString& path, const QString& interface, const QString& method) const
{
    return QDBusMessage::createMethodCall(m_service, path, interface, method);
}

void KScreenClient::refresh()
{
    const quint64 generation = ++m_generation;
    if (m_state == State::Unavailable) {
        settle(State::Loading, {});
    }

    QDBusMessage request = call(QString::fromLatin1(kLauncherPath), QString::fromLatin1(kLauncherInterface),
                                QStringLiteral("requestBackend"));
    request.setArguments({QString(), QVariantMap()});
    whenAnswered(this, m_bus.asyncCall(request, m_timeouts.backendMs), [this, generation](const QDBusMessage& reply) {
        if (!isCurrent(generation)) {
            return;
        }
        if (reply.type() == QDBusMessage::ErrorMessage) {
            settle(State::Unavailable, describe(QDBusError(reply)));
            return;
        }
        const QVariantList args = reply.arguments();
        if (args.size() != 1 || args.front().metaType().id() != QMetaType::Bool || !args.front().toBool()) {
            settle(State::Unavailable, QStringLiteral("KScreen could not load a display backend."));
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
            settle(State::Unavailable, describe(QDBusError(reply)));
            return;
        }
        const QVariantList args = reply.arguments();
        const QVariant config = args.size() == 1 ? plainVariant(args.front()) : QVariant();
        if (config.metaType().id() != QMetaType::QVariantMap) {
            settle(State::Unavailable, QStringLiteral("KScreen answered with something other than a configuration."));
            return;
        }
        auto monitors = parseKScreenConfig(config.toMap());
        if (!monitors) {
            settle(State::Unavailable, monitors.error());
            return;
        }
        settleReady(std::move(*monitors));
        requestMonitorNames(generation);
    });
}

void KScreenClient::requestMonitorNames(quint64 generation)
{
    for (const Monitor& monitor : std::as_const(m_answer)) {
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
            const auto it = std::ranges::find(m_answer, id, &Monitor::kscreenId);
            if (it == m_answer.end()) {
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

void KScreenClient::settle(State state, QString reason)
{
    Q_ASSERT(state != State::Ready);
    const Shown before = shown();
    m_state = state;
    m_unavailableReason = std::move(reason);
    settled(before);
}

void KScreenClient::settleReady(QList<Monitor> answer)
{
    const Shown before = shown();
    keepKnownNames(answer, m_answer);
    m_state = State::Ready;
    m_unavailableReason.clear();
    m_answer = std::move(answer);
    settled(before);
}

void KScreenClient::settled(const Shown& before)
{
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
        } else if (std::ranges::count_if(m_answer, &Monitor::selectable) == 1) {
            m_selection = std::ranges::find_if(m_answer, &Monitor::selectable)->outputName;
        }
    }

    // Emitted after every field is written, so no handler sees a state paired with the
    // previous answer's monitors.
    if (m_state != before.state || m_unavailableReason != before.reason) {
        emit stateChanged();
    }
    if (monitors() != before.monitors) {
        emit monitorsChanged();
    }
    if (selectedMonitor() != before.selection) {
        emit selectionChanged();
    }
}

bool KScreenClient::select(const QString& outputName)
{
    const QList<Monitor> shownMonitors = monitors();
    const auto it = std::ranges::find(shownMonitors, outputName, &Monitor::outputName);
    if (it == shownMonitors.end() || !it->selectable()) {
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
