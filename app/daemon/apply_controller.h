// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace wallpaperx {

class DaemonClient;
class KScreenClient;
class LibraryManager;
class PlasmaController;

// Puts the wallpaper open in the library on the monitor selected in the picker, keeps that
// monitor's copy of its settings in step while it stays open, and sets the selected monitor's
// pause mode.
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
    Progress pauseState() const { return m_pause; }
    QString pauseMessage() const;
    QString pendingPauseMode() const;

    // Does nothing while blockedReason() is not empty or an Apply is running.
    Q_INVOKABLE void apply();
    // Sets the selected monitor's pause mode, one of the daemon's four names. Does nothing while a
    // change is running, with no monitor selected, or for any other name. Fails at once for a
    // monitor the daemon cannot be given.
    Q_INVOKABLE void setPauseMode(const QString& mode);

signals:
    void applyChanged();
    void blockedReasonChanged();
    void liveChanged();
    void pauseChanged();

private:
    // What an Apply put where; the open wallpaper's values follow it there while it stays open.
    struct Live {
        quint64 id = 0;
        QString wallpaperPath;
        QString connector;
        int monitorId = -1;
        // What the daemon last accepted for it.
        QVariantMap sent;
    };
    enum class Sending { No, Yes, YesThenAgain };

    void settleApply(Progress state, QString message);
    void settlePause(Progress state, QString message);
    void startLive(const QString& wallpaperPath, const QString& connector, int monitorId, QVariantMap sent);
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
    Progress m_pause = Progress::Idle;
    QString m_pauseMessage;
    // The connector the last pause change was for, and the mode it set.
    QString m_pauseOutput;
    QString m_pendingPauseMode;
};

} // namespace wallpaperx
