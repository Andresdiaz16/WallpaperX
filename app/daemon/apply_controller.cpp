// SPDX-License-Identifier: GPL-2.0-or-later

#include "apply_controller.h"

#include "daemon_client.h"
#include "kscreen_client.h"
#include "library_manager.h"
#include "plasma_controller.h"

#include <utility>

namespace wallpaperx {

namespace {

QString monitorName(const Monitor& monitor)
{
    return monitor.displayName == monitor.outputName
               ? monitor.outputName
               : QStringLiteral("%1 (%2)").arg(monitor.displayName, monitor.outputName);
}

// Why the daemon would refuse `connector` as a screen's name, or empty. QtDBus sends it as UTF-8.
QString outputRefusal(const QString& connector)
{
    if (connector.toUtf8().size() > kMaxOutputNameBytes) {
        return ApplyController::tr("WallpaperX cannot use this monitor: its connector name is longer than the %1 bytes "
                                   "the WallpaperX service accepts.")
            .arg(kMaxOutputNameBytes);
    }
    return {};
}

} // namespace

ApplyController::ApplyController(KScreenClient& kscreen, LibraryManager& library, DaemonClient& daemon,
                                 PlasmaController& plasma, QObject* parent)
    : QObject(parent)
    , m_kscreen(kscreen)
    , m_library(library)
    , m_daemon(daemon)
    , m_plasma(plasma)
{
    connect(&m_library, &LibraryManager::selectionChanged, this, [this] {
        endLiveUnlessOpen();
        emit blockedReasonChanged();
        emit applyChanged();
    });
    connect(&m_library, &LibraryManager::propertyValuesChanged, this, &ApplyController::sendLive);
    connect(&m_kscreen, &KScreenClient::selectionChanged, this, [this] {
        emit blockedReasonChanged();
        emit applyChanged();
        emit pauseChanged();
    });
}

QString ApplyController::applyMessage() const
{
    const bool selected = m_library.selectedPath() == m_applyWallpaper && m_kscreen.selectedOutput() == m_applyOutput;
    return m_apply == Progress::Working || selected ? m_applyMessage : QString();
}

QString ApplyController::pauseMessage() const
{
    return m_kscreen.selectedOutput() == m_pauseOutput ? m_pauseMessage : QString();
}

QString ApplyController::pendingPauseMode() const
{
    return m_pause == Progress::Working && m_kscreen.selectedOutput() == m_pauseOutput ? m_pendingPauseMode : QString();
}

QString ApplyController::blockedReason() const
{
    const Wallpaper wallpaper = m_library.selectedWallpaper();
    if (!wallpaper.usable()) {
        return tr("Open a wallpaper to apply it.");
    }
    if (wallpaper.type == u"shader") {
        return tr("WallpaperX cannot draw shader wallpapers yet, so this one cannot be applied.");
    }
    // A usable folder's path is valid UTF-8 (readFolderAt), as the bus requires.
    if (wallpaper.assetPath.isEmpty()) {
        return tr("This wallpaper has no file on disk to show.");
    }
    if (m_kscreen.selectedOutput().isEmpty()) {
        return tr("Choose a monitor above to apply it to.");
    }
    return outputRefusal(m_kscreen.selectedOutput());
}

void ApplyController::apply()
{
    if (m_apply == Progress::Working || !blockedReason().isEmpty()) {
        return;
    }
    const Wallpaper wallpaper = m_library.selectedWallpaper();
    const Monitor monitor = m_kscreen.selectedMonitor();
    const QString connector = monitor.outputName;
    const QVariantMap values = m_library.propertyValues();
    const QString title = wallpaper.displayTitle();
    const QString where = monitorName(monitor);

    // What the last Apply put live stays live until this one takes effect (startLive): until then
    // the daemon still shows it there.
    m_applyWallpaper = wallpaper.filePath;
    m_applyOutput = connector;
    settleApply(Progress::Working, tr("Applying %1 to %2…").arg(title, where));
    m_daemon.bindMonitor(this, connector, [=, this](std::expected<int, QString> monitorId) {
        if (!monitorId) {
            settleApply(Progress::Failed, tr("%1 was not applied to %2. %3").arg(title, where, monitorId.error()));
            return;
        }
        m_daemon.setWallpaper(this, wallpaper.type, wallpaper.assetPath, *monitorId, values,
                              [=, this, id = *monitorId](std::expected<void, QString> set) {
            if (!set) {
                settleApply(Progress::Failed, tr("%1 was not applied to %2. %3").arg(title, where, set.error()));
                return;
            }
            // Live from here: the daemon holds the wallpaper whatever Plasma answers.
            startLive(wallpaper.filePath, connector, id, values);
            m_plasma.activate(this, connector, [=, this](std::expected<int, QString> switched) {
                if (!switched) {
                    settleApply(Progress::Failed,
                                tr("WallpaperX has %1 ready for %2, but the desktop there was not switched to WallpaperX. %3")
                                    .arg(title, where, switched.error()));
                    return;
                }
                settleApply(Progress::Done, tr("%1 is on %2.").arg(title, where));
            });
        });
    });
}

void ApplyController::setPauseMode(const QString& mode)
{
    const Monitor monitor = m_kscreen.selectedMonitor();
    if (m_pause == Progress::Working || monitor.outputName.isEmpty() || !isPauseMode(mode)) {
        return;
    }
    const QString where = monitorName(monitor);
    m_pauseOutput = monitor.outputName;
    m_pendingPauseMode = mode;
    if (const QString refusal = outputRefusal(monitor.outputName); !refusal.isEmpty()) {
        settlePause(Progress::Failed, refusal);
        return;
    }
    settlePause(Progress::Working, {});
    m_daemon.bindMonitor(this, monitor.outputName, [=, this](std::expected<int, QString> monitorId) {
        if (!monitorId) {
            settlePause(Progress::Failed, tr("The pause setting for %1 was not changed. %2").arg(where, monitorId.error()));
            return;
        }
        m_daemon.setPause(this, *monitorId, mode, [=, this](std::expected<void, QString> set) {
            if (!set) {
                settlePause(Progress::Failed, tr("The pause setting for %1 was not changed. %2").arg(where, set.error()));
                return;
            }
            settlePause(Progress::Done, {});
        });
    });
}

void ApplyController::settleApply(Progress state, QString message)
{
    if (m_apply == state && m_applyMessage == message) {
        return;
    }
    m_apply = state;
    m_applyMessage = std::move(message);
    emit applyChanged();
}

void ApplyController::settlePause(Progress state, QString message)
{
    m_pause = state;
    m_pauseMessage = std::move(message);
    emit pauseChanged();
}

void ApplyController::startLive(const QString& wallpaperPath, const QString& connector, int monitorId, QVariantMap sent)
{
    m_live = Live{++m_lastLiveId, wallpaperPath, connector, monitorId, std::move(sent)};
    m_liveError.clear();
    emit liveChanged();
    // Closed while the daemon was answering, or edited meanwhile.
    endLiveUnlessOpen();
    sendLive();
}

void ApplyController::endLiveUnlessOpen()
{
    if (m_live && m_library.selectedPath() != m_live->wallpaperPath) {
        m_live.reset();
        m_liveError.clear();
        emit liveChanged();
    }
}

void ApplyController::sendLive()
{
    if (!m_live) {
        return;
    }
    // One at a time, the latest values next: a slider moves faster than the daemon answers.
    if (m_sending != Sending::No) {
        m_sending = Sending::YesThenAgain;
        return;
    }
    const QVariantMap values = m_library.propertyValues();
    if (values == m_live->sent) {
        return;
    }
    m_sending = Sending::Yes;
    const quint64 id = m_live->id;
    m_daemon.setProperties(this, m_live->monitorId, values, [this, id, values](std::expected<void, QString> set) {
        if (m_live && m_live->id == id) {
            const QString error = set ? QString()
                                      : tr("A change was not applied to %1. %2").arg(m_live->connector, set.error());
            if (set) {
                m_live->sent = values;
            }
            if (m_liveError != error) {
                m_liveError = error;
                emit liveChanged();
            }
        }
        if (std::exchange(m_sending, Sending::No) == Sending::YesThenAgain) {
            sendLive();
        } else {
            // The save behind an accepted change is reported only in the daemon's persistence health.
            m_daemon.refresh();
        }
    });
}

} // namespace wallpaperx
