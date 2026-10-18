// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

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

// Puts the wallpaper open in the library on the monitor selected in the picker, keeps that
// monitor's copy of its settings in step while it stays open, and sets the selected monitor's
// pause mode and sound, and sound for every screen.
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
    // The screen the open wallpaper's settings are kept in step on, or empty.
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

signals:
    void applyChanged();
    void blockedReasonChanged();
    void liveChanged();
    void pauseChanged();
    void screenSoundChanged();
    void soundChanged();

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
    // Binds the selected monitor and hands its id to `send`; `what` names the setting in a failure.
    void changeScreenSetting(Change& change, ChangeSignal changed, const QVariant& value, const QString& what,
                             std::function<void(int monitorId, std::function<void(std::expected<void, QString>)>)> send);

    // What an Apply put where; the open wallpaper's values follow it there while it stays open.
    struct Live {
        quint64 id = 0;
        // The library's path for it, and the type and file the daemon is given.
        QString wallpaperPath;
        QString type;
        QString assetPath;
        QString connector;
        int monitorId = -1;
        // What the daemon last accepted for it. Nothing once a restarted daemon may hold another
        // wallpaper there, and the next send then sets the wallpaper itself with the values.
        std::optional<QVariantMap> sent;
    };
    enum class Sending { No, Yes, YesThenAgain };

    void settleApply(Progress state, QString message);
    // Gives `live` a new id.
    void startLive(Live live);
    void endLiveUnlessOpen();
    void sendLive();

    KScreenClient& m_kscreen;
    LibraryManager& m_library;
    DaemonClient& m_daemon;
    PlasmaController& m_plasma;

    Progress m_apply = Progress::Idle;
    QString m_applyMessage;
    // What the last Apply was for: the wallpaper's path and the connector.
    QString m_applyWallpaper;
    QString m_applyOutput;
    std::optional<Live> m_live;
    quint64 m_lastLiveId = 0;
    Sending m_sending = Sending::No;
    QString m_liveError;
    Change m_pause;
    Change m_screenSound;
    Change m_sound{true};
};

} // namespace wallpaperx
