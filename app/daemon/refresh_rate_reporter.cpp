// SPDX-License-Identifier: GPL-2.0-or-later

#include "refresh_rate_reporter.h"

#include "daemon_client.h"
#include "kscreen_client.h"

#include <algorithm>
#include <cmath>

namespace wallpaperx {

namespace {

// The daemon's status carries the rate as JSON text, which needs no more than equal to the last bits.
bool sameRate(double a, double b)
{
    return std::abs(a - b) <= 1e-9 * std::max(std::abs(a), std::abs(b));
}

} // namespace

RefreshRateReporter::RefreshRateReporter(KScreenClient& kscreen, DaemonClient& daemon, QObject* parent)
    : QObject(parent)
    , m_kscreen(kscreen)
    , m_daemon(daemon)
{
    connect(&m_kscreen, &KScreenClient::monitorsChanged, this, &RefreshRateReporter::sync);
    connect(&m_daemon, &DaemonClient::statusChanged, this, &RefreshRateReporter::sync);
    // Its own sync too: a new daemon whose status reads the same as the last one's changes nothing else.
    connect(&m_daemon, &DaemonClient::serviceStarted, this, [this] {
        ++m_daemonStarts;
        sync();
    });
}

double RefreshRateReporter::wanted(const QString& connector) const
{
    const QList<Monitor> monitors = m_kscreen.monitors();
    const auto it = std::ranges::find(monitors, connector, &Monitor::outputName);
    return it != monitors.end() && it->selectable() ? it->refreshRate : 0;
}

void RefreshRateReporter::sync()
{
    const DaemonStatus& status = m_daemon.status();
    if (m_kscreen.state() != KScreenClient::State::Ready || m_daemon.state() != DaemonClient::State::Ready ||
        !status.frameRates) {
        return;
    }
    // A failure for a name the daemon no longer lists is forgotten, so the name is sent once named again.
    m_reports.removeIf([&status](QHash<QString, Report>::iterator report) {
        return report->outcome == Report::Outcome::Failed && !status.outputs.contains(report.key());
    });
    for (auto it = status.outputs.cbegin(); it != status.outputs.cend(); ++it) {
        const QString& connector = it.key();
        const double rate = wanted(connector);
        if (const auto last = m_reports.constFind(connector); last != m_reports.cend()) {
            // Retrying what this daemon just failed would only fail again, once per unrelated change.
            if (last->outcome == Report::Outcome::Sending ||
                (last->daemon == m_daemonStarts && sameRate(last->rate, rate))) {
                continue;
            }
            m_reports.erase(last);
        }
        if (sameRate(rate, status.frameRates->refreshRates.value(it.value(), 0))) {
            continue;
        }
        m_reports.insert(connector, {Report::Outcome::Sending, rate, m_daemonStarts});
        m_daemon.setOutputRefreshRate(this, connector, rate, [this, connector, rate](std::expected<void, QString> set) {
            if (set) {
                m_reports.remove(connector);
            } else {
                qWarning("wallpaperx: the refresh rate of %s was not given to the WallpaperX service: %s",
                         qUtf8Printable(connector), qUtf8Printable(set.error()));
                if (const auto sent = m_reports.find(connector); sent != m_reports.end()) {
                    sent->outcome = Report::Outcome::Failed;
                }
            }
            // What changed while the call was out was skipped for this connector, and is seen now.
            sync();
        });
    }
}

} // namespace wallpaperx
