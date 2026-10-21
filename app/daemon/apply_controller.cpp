// SPDX-License-Identifier: GPL-2.0-or-later

#include "apply_controller.h"

#include "daemon_client.h"
#include "kscreen_client.h"
#include "library_manager.h"
#include "plasma_controller.h"

#include <algorithm>
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
    connect(&m_daemon, &DaemonClient::serviceStarted, this, [this] {
        if (m_live) {
            // A new id, so a reply from the daemon that went away records nothing.
            m_live->id = ++m_lastLiveId;
            m_live->sent.reset();
            sendLive();
        }
    });
    connect(&m_kscreen, &KScreenClient::selectionChanged, this, [this] {
        emit blockedReasonChanged();
        emit applyChanged();
        emit pauseChanged();
        emit screenSoundChanged();
        emit screenFrameRateChanged();
    });
    connect(&m_daemon, &DaemonClient::statusChanged, this, &ApplyController::frameRateNoticeChanged);
}

QString ApplyController::applyMessage() const
{
    const bool selected = m_library.selectedPath() == m_applyWallpaper && m_kscreen.selectedOutput() == m_applyOutput;
    return m_apply == Progress::Working || selected ? m_applyMessage : QString();
}

bool ApplyController::forSelected(const Change& change) const
{
    return change.everyScreen || m_kscreen.selectedOutput() == change.output;
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
            startLive(Live{0, wallpaper.filePath, wallpaper.type, wallpaper.assetPath, connector, id, values});
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
    if (!isPauseMode(mode)) {
        return;
    }
    changeScreenSetting(m_pause, &ApplyController::pauseChanged, mode, tr("The pause setting"),
                        [this, mode](int monitorId, DaemonClient::Done done) {
                            m_daemon.setPause(this, monitorId, mode, std::move(done));
                        });
}

void ApplyController::setScreenSound(bool on)
{
    changeScreenSetting(m_screenSound, &ApplyController::screenSoundChanged, on, tr("The sound setting"),
                        [this, on](int monitorId, DaemonClient::Done done) {
                            m_daemon.setScreenSound(this, monitorId, on, std::move(done));
                        });
}

void ApplyController::setSound(bool on)
{
    changeGlobalSetting(m_sound, &ApplyController::soundChanged, on, tr("Sound was not changed for every screen."),
                        [this, on](DaemonClient::Done done) { m_daemon.setSound(this, on, std::move(done)); });
}

void ApplyController::setFrameRate(int rate)
{
    if (std::ranges::find(kFrameRates, rate) == std::end(kFrameRates)) {
        return;
    }
    changeGlobalSetting(m_frameRate, &ApplyController::frameRateChanged, rate,
                        tr("The frame rate was not changed for every screen."),
                        [this, rate](DaemonClient::Done done) { m_daemon.setFrameRate(this, rate, std::move(done)); });
}

void ApplyController::setScreenFrameRate(int rate)
{
    if (rate != 0 && std::ranges::find(kFrameRates, rate) == std::end(kFrameRates)) {
        return;
    }
    changeScreenSetting(m_screenFrameRate, &ApplyController::screenFrameRateChanged, rate, tr("The frame rate"),
                        [this, rate](int monitorId, DaemonClient::Done done) {
                            m_daemon.setScreenFrameRate(this, monitorId, rate, std::move(done));
                        });
}

void ApplyController::setKeepHigherFrameRate(bool keep)
{
    changeGlobalSetting(m_keepHigher, &ApplyController::keepHigherChanged, keep,
                        tr("Keeping a higher frame rate was not changed."),
                        [this, keep](DaemonClient::Done done) { m_daemon.setKeepHigherFrameRate(this, keep, std::move(done)); });
}

std::optional<ApplyController::AboveRefresh> ApplyController::screensAboveRefresh() const
{
    const DaemonStatus& status = m_daemon.status();
    if (m_daemon.state() != DaemonClient::State::Ready || !status.frameRates) {
        return std::nullopt;
    }
    const FrameRates& rates = *status.frameRates;
    // As the daemon applies them: a value it does not know gives way to the one above it.
    const int global = rates.rate.value_or(kDefaultFrameRate);
    std::optional<AboveRefresh> above;
    // The daemon holds a refresh rate only for a screen it has a name for, and slows no other.
    for (auto it = rates.refreshRates.cbegin(); it != rates.refreshRates.cend(); ++it) {
        const int id = it.key();
        const int refresh = qRound(it.value());
        const int rate = rates.screenRates.value(id, global).value_or(global);
        // Against the rounded rate, the one the notice names: 60 on a 59.94 Hz screen loses nothing.
        if (!status.wallpapers.contains(id) || rate <= refresh) {
            continue;
        }
        AboveRefresh& found = above ? *above : above.emplace();
        found.fastest = std::max(found.fastest, refresh);
    }
    return above;
}

QString ApplyController::frameRateNotice() const
{
    const auto above = screensAboveRefresh();
    if (!above || m_daemon.status().frameRates->keepHigher) {
        return {};
    }
    return tr("Your screens refresh at most %1 times a second. Pictures above that can't be shown — they only cost "
              "power and memory — so WallpaperX plays at your screen's rate. You can keep a higher rate anyway.")
        .arg(above->fastest);
}

void ApplyController::changeGlobalSetting(Change& change, ChangeSignal changed, const QVariant& value,
                                          const QString& what,
                                          std::function<void(std::function<void(std::expected<void, QString>)>)> send)
{
    if (change.state == Progress::Working) {
        return;
    }
    change.value = value;
    settle(change, changed, Progress::Working, {});
    // `change` is a member, as in changeScreenSetting.
    send([this, &change, changed, what](std::expected<void, QString> set) {
        if (!set) {
            settle(change, changed, Progress::Failed, QStringLiteral("%1 %2").arg(what, set.error()));
            return;
        }
        settle(change, changed, Progress::Done, {});
    });
}

void ApplyController::changeScreenSetting(
    Change& change, ChangeSignal changed, const QVariant& value, const QString& what,
    std::function<void(int monitorId, std::function<void(std::expected<void, QString>)>)> send)
{
    const Monitor monitor = m_kscreen.selectedMonitor();
    if (change.state == Progress::Working || monitor.outputName.isEmpty()) {
        return;
    }
    const QString where = monitorName(monitor);
    change.output = monitor.outputName;
    change.value = value;
    if (const QString refusal = outputRefusal(monitor.outputName); !refusal.isEmpty()) {
        settle(change, changed, Progress::Failed, refusal);
        return;
    }
    settle(change, changed, Progress::Working, {});
    // `change` is a member, so it lives as long as the replies, which run only while this does.
    const auto fail = [this, &change, changed, what, where](const QString& why) {
        settle(change, changed, Progress::Failed, tr("%1 for %2 was not changed. %3").arg(what, where, why));
    };
    m_daemon.bindMonitor(this, monitor.outputName,
                         [this, &change, changed, fail, send = std::move(send)](std::expected<int, QString> monitorId) {
        if (!monitorId) {
            fail(monitorId.error());
            return;
        }
        send(*monitorId, [this, &change, changed, fail](std::expected<void, QString> set) {
            if (!set) {
                fail(set.error());
                return;
            }
            settle(change, changed, Progress::Done, {});
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

void ApplyController::settle(Change& change, ChangeSignal changed, Progress state, QString message)
{
    change.state = state;
    change.message = std::move(message);
    emit (this->*changed)();
}

void ApplyController::startLive(Live live)
{
    live.id = ++m_lastLiveId;
    m_live = std::move(live);
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
    auto done = [this, id, values](std::expected<void, QString> set) {
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
            // A save the daemon could not queue is in its persistence health now; one it queued is
            // announced by PersistenceChanged once written.
            m_daemon.refresh();
        }
    };
    if (m_live->sent) {
        m_daemon.setProperties(this, m_live->monitorId, values, std::move(done));
    } else {
        // Addressed by wallpaper: a monitor addressed alone may hold another one now.
        m_daemon.setWallpaper(this, m_live->type, m_live->assetPath, m_live->monitorId, values, std::move(done));
    }
}

} // namespace wallpaperx
