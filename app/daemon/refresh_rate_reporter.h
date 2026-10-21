// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

namespace wallpaperx {

class DaemonClient;
class KScreenClient;

// Keeps the refresh rate the daemon holds for each screen it has a name for equal to the one KScreen
// reports, so the daemon can play no faster than a screen shows. The daemon does not save them.
//
// What is sent is derived each time from the two answers, compared screen by screen, whenever either
// changes; that covers the app's start, a daemon that starts again knowing none, and a mode or
// screen KScreen reports changed, with no list of reports owed to go stale. While KScreen is not
// Ready nothing is sent, so the daemon keeps what it was told, and outside KDE nothing is ever sent.
// A rate that failed is not sent again until KScreen reports another or another daemon starts.
// Both clients must outlive this.
class RefreshRateReporter : public QObject {
    Q_OBJECT

public:
    RefreshRateReporter(KScreenClient& kscreen, DaemonClient& daemon, QObject* parent = nullptr);

private:
    // The last call for a connector: on its way, or failed. One at a time per connector.
    struct Report {
        enum class Outcome { Sending, Failed };
        Outcome outcome;
        double rate;
        // The daemon it went to, counted by m_daemonStarts.
        quint64 daemon;
    };

    void sync();
    // What KScreen says `connector` refreshes at: 0 when it does not list it showing anything.
    double wanted(const QString& connector) const;

    KScreenClient& m_kscreen;
    DaemonClient& m_daemon;
    QHash<QString, Report> m_reports;
    quint64 m_daemonStarts = 0;
};

} // namespace wallpaperx
