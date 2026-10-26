// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "kscreen_source.h"

#include <QDBusConnection>
#include <QString>
#include <QTimer>

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

// KDE's display configuration service, which libkscreen before 6.8 runs out of process.
//
// KScreen's launcher serves /backend only after a client has asked it to load a backend, so a
// read is two calls: requestBackend on "/", then getConfig on "/backend". An empty backend name
// lets the launcher pick by its own platform; a name must be the plugin's suffix ("KWayland"),
// not its file name, or the launcher answers false.
//
// The bus, service name and timeouts are injected so a test can stand up its own service on a
// private bus and wait milliseconds; the application passes the session bus and the defaults.
class KScreenDBusSource final : public KScreenSource {
    Q_OBJECT

public:
    static constexpr const char* kDefaultService = "org.kde.KScreen";

    KScreenDBusSource(QDBusConnection bus, QString service, KScreenTimeouts timeouts, QObject* parent = nullptr);

    void read() override;

private slots:
    void onBackendConfigChanged() { emit changed(); }

private:
    void onLauncherOwnerChanged(const QString& service, const QString& oldOwner, const QString& newOwner);
    QDBusMessage call(const QString& path, const QString& interface, const QString& method) const;
    void requestConfig(quint64 generation);
    void requestMonitorNames(quint64 generation, const QList<Monitor>& monitors);
    bool isCurrent(quint64 generation) const { return generation == m_generation; }

    QDBusConnection m_bus;
    QString m_service;
    KScreenTimeouts m_timeouts;
    quint64 m_generation = 0;
    // Spends the one re-read a launcher loss gets; a name handed straight to a new owner is no
    // loss, as nothing need be started. Cleared only when m_launcherStayUp fires, not
    // by an answer or a read, so a launcher that exits after every answer, or each
    // "Try again" on one that crashes, gets one start per loss rather than a loop.
    bool m_launcherRetrySpent = false;
    // Runs only while the retry is spent: from the first answer after that until the
    // launcher has stayed up launcherStayUpMs. Any loss stops it.
    QTimer m_launcherStayUp;
};

} // namespace wallpaperx
