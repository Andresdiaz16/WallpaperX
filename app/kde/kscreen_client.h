// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "kscreen_config.h"

#include <QDBusConnection>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <optional>

class QDBusMessage;

namespace wallpaperx {

// How long each call may take before KScreen counts as not answering, and how long a launcher
// must stay up after answering before its loss is retried again.
struct KScreenTimeouts {
    // requestBackend may have to start the launcher (systemd allows it 5 s to take its name) and
    // then load a plugin that talks to the compositor, so it gets D-Bus's own default.
    int backendMs = 25000;
    // getConfig and getEdid run once the backend is loaded and answer from its state.
    int callMs = 5000;
    // The launcher stops on its own after idling for minutes; one that leaves sooner than this
    // after a Ready answer counts as failing, not idling.
    int launcherStayUpMs = 60000;
};

// The monitors KDE's display configuration service reports, and which one the user picked.
//
// KScreen's launcher serves /backend only after a client has asked it to load a backend, so a
// query is two calls: requestBackend on "/", then getConfig on "/backend". An empty backend name
// lets the launcher pick by its own platform; a name must be the plugin's suffix ("KWayland"),
// not its file name, or the launcher answers false.
//
// The bus, service name and timeouts are injected so a test can stand up its own service on a
// private bus and wait milliseconds; the application passes the session bus and the defaults.
class KScreenClient : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application with the bus it should use.")
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY stateChanged)
    Q_PROPERTY(QList<wallpaperx::Monitor> monitors READ monitors NOTIFY monitorsChanged)
    Q_PROPERTY(QString selectedOutput READ selectedOutput NOTIFY selectionChanged)
    Q_PROPERTY(wallpaperx::Monitor selectedMonitor READ selectedMonitor NOTIFY selectionChanged)

public:
    // Loading: no answer yet, or re-reading after the launcher left the bus. Ready: `monitors` is
    // KScreen's latest answer. Unavailable: the last attempt failed or the launcher left again
    // before staying up launcherStayUpMs after an answer, `monitors` is empty, `unavailableReason` says why, and
    // nothing is read until refresh() or a new launcher takes the name.
    enum class State { Loading, Ready, Unavailable };
    Q_ENUM(State)

    static constexpr const char* kDefaultService = "org.kde.KScreen";

    explicit KScreenClient(QDBusConnection bus, QString service = QString::fromLatin1(kDefaultService),
                           KScreenTimeouts timeouts = {}, QObject* parent = nullptr);

    State state() const { return m_state; }
    QString unavailableReason() const { return m_unavailableReason; }
    QList<Monitor> monitors() const { return m_monitors; }
    // Empty when nothing is selected. Always names a selectable monitor in `monitors`, so it is
    // empty while not Ready even if a choice is remembered.
    QString selectedOutput() const { return selectedMonitor().outputName; }
    Monitor selectedMonitor() const;

    // Asks KScreen again. A client already Ready stays Ready, showing the previous answer, until
    // the new one arrives; any reply to an earlier refresh is then discarded.
    Q_INVOKABLE void refresh();
    // False, changing nothing, unless `outputName` is a selectable monitor.
    Q_INVOKABLE bool select(const QString& outputName);

signals:
    void stateChanged();
    void monitorsChanged();
    void selectionChanged();

private slots:
    void onBackendConfigChanged();

private:
    void onLauncherOwnerChanged(const QString& service, const QString& oldOwner, const QString& newOwner);
    QDBusMessage call(const QString& path, const QString& interface, const QString& method) const;
    void requestConfig(quint64 generation);
    // The one writer of state, reason and monitors, so an Unavailable client never holds a
    // monitor list and a selection never outlives its monitor in a Ready answer.
    void settle(State state, QString reason, QList<Monitor> monitors);
    void requestMonitorNames(quint64 generation);
    bool isCurrent(quint64 generation) const { return generation == m_generation; }

    QDBusConnection m_bus;
    QString m_service;
    KScreenTimeouts m_timeouts;
    State m_state = State::Loading;
    QString m_unavailableReason;
    QList<Monitor> m_monitors;
    // The user's choice; read it only through selectedMonitor(), which hides it while `monitors`
    // does not list it as selectable. nullopt until anything is selected: only then may the sole
    // selectable screen be chosen for the user. Empty once a Ready answer lost it, which blocks
    // that until the user picks again, since Apply acts on whatever this names. A failed or
    // pending read is not an answer and keeps it.
    std::optional<QString> m_selection;
    quint64 m_generation = 0;
    // Spends the one re-read a launcher loss gets. Cleared only when m_launcherStayUp fires, not
    // by a Ready answer or refresh(), so a launcher that exits after every answer, or each
    // "Try again" on one that crashes, gets one start per loss rather than a loop.
    bool m_launcherRetrySpent = false;
    // Runs only while the retry is spent: from the first Ready answer after that until the
    // launcher has stayed up launcherStayUpMs. Any loss stops it.
    QTimer m_launcherStayUp;
};

} // namespace wallpaperx
