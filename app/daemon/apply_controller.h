// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <expected>
#include <functional>
#include <optional>

namespace wallpaperx {

class DaemonClient;
class KScreenClient;
class LibraryManager;
class PlasmaController;

// Puts the wallpaper open in the library on the monitor selected in the picker, keeps the selected
// monitor's copy of its settings in step while that monitor shows it, and sets the selected monitor's
// pause mode, sound and frame rate, and sound, frame rate and keeping a rate above the screens for
// every screen.
//
// Apply asks the daemon first, which is what decides the picture, then Plasma, which only switches
// the desktop to the plugin that draws it. Every failure ends in a state with a sentence saying
// which half failed and why. All four objects given must outlive this one.
class ApplyController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application with the clients it should use.")
    Q_PROPERTY(Progress applyState READ applyState NOTIFY applyChanged)
    // What the running Apply is doing; once it ends, how it went, but only while the wallpaper and
    // screen it was for are the ones selected. Else empty.
    Q_PROPERTY(QString applyMessage READ applyMessage NOTIFY applyChanged)
    // Why Apply cannot run for what is selected now, or empty when it can.
    Q_PROPERTY(QString blockedReason READ blockedReason NOTIFY blockedReasonChanged)
    // The selected screen, while the open wallpaper's settings are kept in step on it; else empty.
    Q_PROPERTY(QString liveOutput READ liveOutput NOTIFY liveChanged)
    // Why the last change sent there was refused, or empty.
    Q_PROPERTY(QString liveError READ liveError NOTIFY liveChanged)
    // The last pause change, on whichever screen: one runs at a time.
    Q_PROPERTY(Progress pauseState READ pauseState NOTIFY pauseChanged)
    // Why the last pause change failed, while the screen it was for is selected; else empty.
    Q_PROPERTY(QString pauseMessage READ pauseMessage NOTIFY pauseChanged)
    // The mode being set while pauseState is Working and its screen is selected, else empty.
    Q_PROPERTY(QString pendingPauseMode READ pendingPauseMode NOTIFY pauseChanged)
    // The same three for the selected monitor's sound; the pending value is undefined while none is.
    Q_PROPERTY(Progress screenSoundState READ screenSoundState NOTIFY screenSoundChanged)
    Q_PROPERTY(QString screenSoundMessage READ screenSoundMessage NOTIFY screenSoundChanged)
    Q_PROPERTY(QVariant pendingScreenSound READ pendingScreenSound NOTIFY screenSoundChanged)
    // And for sound on every screen, whichever is selected.
    Q_PROPERTY(Progress soundState READ soundState NOTIFY soundChanged)
    Q_PROPERTY(QString soundMessage READ soundMessage NOTIFY soundChanged)
    Q_PROPERTY(QVariant pendingSound READ pendingSound NOTIFY soundChanged)
    // And for the frame rate on every screen, the selected monitor's own (0 for following it), and
    // keeping a frame rate above what a screen refreshes at.
    Q_PROPERTY(Progress frameRateState READ frameRateState NOTIFY frameRateChanged)
    Q_PROPERTY(QString frameRateMessage READ frameRateMessage NOTIFY frameRateChanged)
    Q_PROPERTY(QVariant pendingFrameRate READ pendingFrameRate NOTIFY frameRateChanged)
    Q_PROPERTY(Progress screenFrameRateState READ screenFrameRateState NOTIFY screenFrameRateChanged)
    Q_PROPERTY(QString screenFrameRateMessage READ screenFrameRateMessage NOTIFY screenFrameRateChanged)
    Q_PROPERTY(QVariant pendingScreenFrameRate READ pendingScreenFrameRate NOTIFY screenFrameRateChanged)
    Q_PROPERTY(Progress keepHigherState READ keepHigherState NOTIFY keepHigherChanged)
    Q_PROPERTY(QString keepHigherMessage READ keepHigherMessage NOTIFY keepHigherChanged)
    Q_PROPERTY(QVariant pendingKeepHigher READ pendingKeepHigher NOTIFY keepHigherChanged)
    // Whether a screen holding a wallpaper has a frame rate above the refresh rate the daemon holds
    // for it. Always false outside KDE, where the daemon is told no refresh rate.
    Q_PROPERTY(bool frameRateAboveScreens READ frameRateAboveScreens NOTIFY frameRateNoticeChanged)
    // While frameRateAboveScreens and keeping a higher rate is off, why those screens play at their
    // refresh rate; else empty.
    Q_PROPERTY(QString frameRateNotice READ frameRateNotice NOTIFY frameRateNoticeChanged)
    // While the selected screen shows the wallpaper open in the library and the daemon decodes parts or all of it on
    // the processor, a sentence saying so; else empty.
    Q_PROPERTY(QString processorNotice READ processorNotice NOTIFY processorNoticeChanged)

public:
    // Idle: nothing asked yet. Working: a request is running; another is not accepted. Done and
    // Failed: the last one finished, the message says how.
    enum class Progress { Idle, Working, Done, Failed };
    Q_ENUM(Progress)

    ApplyController(KScreenClient& kscreen, LibraryManager& library, DaemonClient& daemon, PlasmaController& plasma,
                    QObject* parent = nullptr);

    Progress applyState() const { return m_apply; }
    QString applyMessage() const;
    QString blockedReason() const;
    QString liveOutput() const { return m_live ? m_live->connector : QString(); }
    QString liveError() const { return m_liveError; }
    Progress pauseState() const { return m_pause.state; }
    QString pauseMessage() const { return messageOf(m_pause); }
    QString pendingPauseMode() const { return pendingOf(m_pause).toString(); }
    Progress screenSoundState() const { return m_screenSound.state; }
    QString screenSoundMessage() const { return messageOf(m_screenSound); }
    QVariant pendingScreenSound() const { return pendingOf(m_screenSound); }
    Progress soundState() const { return m_sound.state; }
    QString soundMessage() const { return messageOf(m_sound); }
    QVariant pendingSound() const { return pendingOf(m_sound); }
    Progress frameRateState() const { return m_frameRate.state; }
    QString frameRateMessage() const { return messageOf(m_frameRate); }
    QVariant pendingFrameRate() const { return pendingOf(m_frameRate); }
    Progress screenFrameRateState() const { return m_screenFrameRate.state; }
    QString screenFrameRateMessage() const { return messageOf(m_screenFrameRate); }
    QVariant pendingScreenFrameRate() const { return pendingOf(m_screenFrameRate); }
    Progress keepHigherState() const { return m_keepHigher.state; }
    QString keepHigherMessage() const { return messageOf(m_keepHigher); }
    QVariant pendingKeepHigher() const { return pendingOf(m_keepHigher); }
    bool frameRateAboveScreens() const { return screensAboveRefresh().has_value(); }
    QString frameRateNotice() const;
    QString processorNotice() const;

    // Does nothing while blockedReason() is not empty or an Apply is running.
    Q_INVOKABLE void apply();
    // Sets the selected monitor's pause mode, one of the daemon's four names. Does nothing while a
    // change is running, with no monitor selected, or for any other name. Fails at once for a
    // monitor the daemon cannot be given.
    Q_INVOKABLE void setPauseMode(const QString& mode);
    // Switches sound on or off on the selected monitor, under the same rules.
    Q_INVOKABLE void setScreenSound(bool on);
    // Switches sound on or off for every screen. Does nothing while a change is running.
    Q_INVOKABLE void setSound(bool on);
    // Sets the frame rate on every screen, one of kFrameRates. Does nothing while a change is running,
    // or for any other rate.
    Q_INVOKABLE void setFrameRate(int rate);
    // Sets the selected monitor's frame rate, one of kFrameRates or 0 to follow the one on every
    // screen, under setPauseMode's rules.
    Q_INVOKABLE void setScreenFrameRate(int rate);
    // Keeps every screen at its frame rate even above what it refreshes at, or not. Does nothing
    // while a change is running.
    Q_INVOKABLE void setKeepHigherFrameRate(bool keep);

signals:
    void applyChanged();
    void blockedReasonChanged();
    void liveChanged();
    void pauseChanged();
    void screenSoundChanged();
    void soundChanged();
    void frameRateChanged();
    void screenFrameRateChanged();
    void keepHigherChanged();
    void frameRateNoticeChanged();
    void processorNoticeChanged();

private:
    // A setting change, one at a time per setting: how the last one went, the connector it was for
    // unless the setting is every screen's, and the value it sets.
    struct Change {
        bool everyScreen = false;
        Progress state = Progress::Idle;
        QString message;
        QString output;
        QVariant value;
    };
    using ChangeSignal = void (ApplyController::*)();

    // What a change shows applies only while its screen is selected.
    bool forSelected(const Change& change) const;
    QString messageOf(const Change& change) const { return forSelected(change) ? change.message : QString(); }
    QVariant pendingOf(const Change& change) const
    {
        return change.state == Progress::Working && forSelected(change) ? change.value : QVariant();
    }
    void settle(Change& change, ChangeSignal changed, Progress state, QString message);
    // Sends a setting for every screen; `what` is the failure sentence's start.
    void changeGlobalSetting(Change& change, ChangeSignal changed, const QVariant& value, const QString& what,
                             std::function<void(std::function<void(std::expected<void, QString>)>)> send);
    // The screens holding a wallpaper whose frame rate is above the refresh rate the daemon holds for
    // them, from the daemon's status alone; nothing when there are none.
    struct AboveRefresh {
        // The highest of their refresh rates, rounded as the notice names it.
        int fastest = 0;
    };
    std::optional<AboveRefresh> screensAboveRefresh() const;
    // Binds the selected monitor and hands its id to `send`; `what` names the setting in a failure.
    void changeScreenSetting(Change& change, ChangeSignal changed, const QVariant& value, const QString& what,
                             std::function<void(int monitorId, std::function<void(std::expected<void, QString>)>)> send);

    // The selected screen showing the open wallpaper, as an Apply put it there or GetStatus says it is:
    // the wallpaper's values follow there while both stay selected and each status read still shows it there.
    struct Live {
        quint64 id = 0;
        // The library's path for it, and the type and file the daemon is given.
        QString wallpaperPath;
        QString type;
        QString assetPath;
        QString connector;
        int monitorId = -1;
        // What the daemon last accepted for it, as the controls hold it. Nothing once a restarted daemon
        // may hold another wallpaper there, and the next send then sets the wallpaper itself with the values;
        // until a status read shows it there again, which then stands for it.
        std::optional<QVariantMap> sent;
    };
    // Values for a screen with the link they were asked on.
    struct Queued {
        Live live;
        QVariantMap values;
    };
    // A property send in flight to one screen, and the latest values asked for there meanwhile, which a
    // link ended since does not lose unless its screen is shown to hold something else, or an Apply there
    // is about to put something else or has put its values there.
    struct InFlight {
        Queued now;
        std::optional<Queued> next;
    };

    // Leaving Working follows the selection again, which an Apply to it held back.
    void settleApply(Progress state, QString message);
    // Gives `live` a new id and links it, unless another wallpaper or screen is selected by now.
    void startLive(Live live);
    // Ends the link when the status read shows its screen holding another wallpaper, or, while the daemon is
    // known to hold the link's values, not showing its wallpaper there; then linkSelected().
    void followStatus();
    // Ends the link, if any. The controls keep their values until linkSelected() starts a draft.
    void unlink();
    // Whether the status read says `link`'s screen no longer holds what the link stands for: another
    // wallpaper there, or its connector naming another monitor; or, while the daemon is known to hold the
    // link's values, its wallpaper not there.
    bool contradicted(const Live& link) const;
    // Ends the link unless it is the selected screen's and the open wallpaper's; links a selected screen
    // GetStatus says shows the open wallpaper, and shows that screen's values in the controls. Not while
    // this app's own Apply there may make GetStatus stale: the controls show what the Apply sets. A send
    // of ours to that screen for the open wallpaper links it at once, with the values it sets; one for
    // another link waits for the read after it. Otherwise the controls start a draft for the next Apply
    // there from the defaults, unless they already hold one for that screen and wallpaper.
    void linkSelected();
    void sendLive();
    // Sends `values` to `to`'s screen, by monitor once it is known to hold the wallpaper, else by wallpaper.
    void sendTo(const Live& to, const QVariantMap& values);

    KScreenClient& m_kscreen;
    LibraryManager& m_library;
    DaemonClient& m_daemon;
    PlasmaController& m_plasma;

    Progress m_apply = Progress::Idle;
    QString m_applyMessage;
    // What the last Apply was for: the wallpaper's path, the connector and the values it sent.
    QString m_applyWallpaper;
    QString m_applyOutput;
    QVariantMap m_applyValues;
    std::optional<Live> m_live;
    // While nothing is linked, the screen and wallpaper the controls hold a draft for; none once a link
    // ends, as its screen no longer shows the wallpaper and its values went with it.
    struct DraftFor {
        QString connector;
        QString wallpaperPath;
        bool operator==(const DraftFor&) const = default;
    };
    std::optional<DraftFor> m_draftFor;
    quint64 m_lastLiveId = 0;
    // By monitor id: one send at a time to each screen.
    QHash<int, InFlight> m_sending;
    QString m_liveError;
    Change m_pause;
    Change m_screenSound;
    Change m_sound{true};
    Change m_frameRate{true};
    Change m_screenFrameRate;
    Change m_keepHigher{true};
};

} // namespace wallpaperx
