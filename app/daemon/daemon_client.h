// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDBusConnection>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <deque>
#include <expected>
#include <functional>
#include <optional>

class QDBusMessage;

namespace wallpaperx {

// The daemon's monitor ids are 0 to kMonitorIds - 1 (requireValidMonitorId).
inline constexpr int kMonitorIds = 32;

// Copies of the daemon's own constants, under its names, so not `inline`: an inline variable of
// the daemon's with the same name would merge with it silently in a program linking both (a
// variable's linker name has no type), where internal linkage keeps each to its own file.

// The pause modes SetMonitorPause takes, by the daemon's names.
constexpr const char* kPauseModes[] = {"play", "always_paused", "paused_while_focused", "paused_while_maximized"};
// The daemon refuses a longer connector name (config_store.h); the user cannot rename one.
constexpr qsizetype kMaxOutputNameBytes = 256;
bool isPauseMode(const QString& mode);
// The frame rates SetFrameRate and SetMonitorFrameRate take, and the one a daemon plays at unless told.
constexpr int kFrameRates[] = {30, 60, 120};
constexpr int kDefaultFrameRate = 60;

// GetStatus's `frame_rate`.
struct FrameRates {
    // nullopt for a value a newer daemon wrote, which this one plays at kDefaultFrameRate.
    std::optional<int> rate = kDefaultFrameRate;
    // A monitor's own rate; nullopt for a newer daemon's value, under which the daemon applies `rate`.
    // A monitor with no entry follows `rate`.
    QHash<int, std::optional<int>> screenRates;
    bool keepHigher = false;
    // The refresh rate the daemon was told for the screen each monitor answers to; no entry for none.
    QHash<int, double> refreshRates;

    friend bool operator==(const FrameRates&, const FrameRates&) = default;
};

// What GetStatus reports that the app acts on.
struct DaemonStatus {
    // Connector name to monitor id, from `outputs_literal`.
    QHash<QString, int> outputs;
    // Ids holding a wallpaper, a pause setting or a sound setting, named or not.
    QSet<int> occupied;
    // Ids holding a wallpaper, with what GetStatus says it is: its type, the file it opens (empty when
    // that is not valid UTF-8), and the properties it was last given.
    struct Shown {
        QString type;
        QString path;
        QVariantMap properties;
        bool operator==(const Shown&) const = default;
    };
    QHash<int, Shown> wallpapers;
    // How much of a video the daemon decodes on the processor, and the file it holds; for the ids whose video it
    // decodes some of on the processor, by a state this app knows.
    struct OnProcessor {
        enum class Share { Parts, All };
        QString path;
        Share share = Share::Parts;
        bool operator==(const OnProcessor&) const = default;
    };
    QHash<int, OnProcessor> onProcessor;
    // A monitor's pause mode; nullopt for one a newer daemon wrote that this one does not apply.
    // A monitor with no entry plays.
    QHash<int, std::optional<QString>> pauseModes;
    // Sound as the daemon applies it, globally and per monitor; a monitor with no entry is on.
    bool sound = true;
    QHash<int, bool> screenSound;
    // nullopt from a daemon older than frame-rate settings, which knows no refresh rates either.
    std::optional<FrameRates> frameRates;
    QString persistenceStatus;
    QString persistenceDetail;

    friend bool operator==(const DaemonStatus&, const DaemonStatus&) = default;
};

// The error is a sentence for the user.
std::expected<DaemonStatus, QString> parseDaemonStatus(const QByteArray& json);
// The lowest id no screen is named on and that holds nothing, so binding it takes nothing from
// anyone. Nothing when all are in use.
std::optional<int> freeMonitorId(const DaemonStatus& status);

struct DaemonTimeouts {
    // Every call, GetStatus included. SetMonitorOutput, SetWallpaper*, SetMonitorPause and the sound
    // settings wait for the config file to be written, and a GetStatus can queue behind one of them:
    // given less, a healthy daemon mid-write would be shown as unavailable.
    int callMs = 25000;
};

// The WallpaperX daemon (io.wallpaperx.Daemon) as the app uses it, and its last GetStatus.
//
// Every call answers through a callback that runs only while its `context` is alive, so a caller
// destroyed with a reply pending is never called back. Failures are sentences for the user.
//
// The bus, service name and timeouts are injected so a test can stand up its own daemon on a
// private bus; the application passes the session bus and the defaults.
class DaemonClient : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Created by the application with the bus it should use.")
    Q_PROPERTY(State state READ state NOTIFY statusChanged)
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY statusChanged)
    // Connector name to pause mode, for every named screen; "" for a mode this app does not know.
    // A screen with no entry plays.
    Q_PROPERTY(QVariantMap pauseModes READ pauseModes NOTIFY statusChanged)
    // Whether sound is on for every screen.
    Q_PROPERTY(bool sound READ sound NOTIFY statusChanged)
    // Connector name to whether sound is on there, for every named screen.
    Q_PROPERTY(QVariantMap screenSound READ screenSound NOTIFY statusChanged)
    // The frame rate on every screen; 0 for a value this app does not know.
    Q_PROPERTY(int frameRate READ frameRate NOTIFY statusChanged)
    // Connector name to that screen's own frame rate, for every named screen: 0 when it follows
    // frameRate, -1 for a value this app does not know.
    Q_PROPERTY(QVariantMap screenFrameRates READ screenFrameRates NOTIFY statusChanged)
    // Whether a screen plays at its frame rate even above what it refreshes at.
    Q_PROPERTY(bool keepHigherFrameRate READ keepHigherFrameRate NOTIFY statusChanged)
    // Why the daemon's settings may not be on disk, or empty.
    Q_PROPERTY(QString persistenceProblem READ persistenceProblem NOTIFY statusChanged)

public:
    // Loading: no GetStatus answer yet. Ready: the status is the latest answer. Unavailable: the
    // last read failed, `unavailableReason` says why, and the next is read when the daemon's name
    // changes owner or something here succeeds.
    enum class State { Loading, Ready, Unavailable };
    Q_ENUM(State)

    static constexpr const char* kDefaultService = "io.wallpaperx.Daemon";

    using Done = std::function<void(std::expected<void, QString>)>;

    explicit DaemonClient(QDBusConnection bus, QString service = QString::fromLatin1(kDefaultService),
                          DaemonTimeouts timeouts = {}, QObject* parent = nullptr);

    State state() const { return m_state; }
    QString unavailableReason() const { return m_unavailableReason; }
    QVariantMap pauseModes() const;
    bool sound() const { return m_status.sound; }
    QVariantMap screenSound() const;
    int frameRate() const;
    QVariantMap screenFrameRates() const;
    bool keepHigherFrameRate() const;
    QString persistenceProblem() const;
    // The last GetStatus answer as shown, empty while not Ready.
    const DaemonStatus& status() const { return m_status; }

public slots:
    // Reads GetStatus again; a reply to an earlier read is then discarded. Also run on the daemon's
    // PersistenceChanged and ProcessorDecodingChanged.
    void refresh();

public:
    // The monitor id the daemon knows `connector` by, naming a free one for it first when it has
    // none. One at a time across every caller: two screens bound at once would both be given the
    // same free id, and the second would take it from the first.
    void bindMonitor(QObject* context, const QString& connector, std::function<void(std::expected<int, QString>)> done);
    // `properties` is the wallpaper's values by key: double, bool, or "#rrggbb" string.
    void setWallpaper(QObject* context, const QString& type, const QString& path, int monitorId,
                      const QVariantMap& properties, Done done);
    void setProperties(QObject* context, int monitorId, const QVariantMap& properties, Done done);
    void setPause(QObject* context, int monitorId, const QString& mode, Done done);
    void setSound(QObject* context, bool on, Done done);
    void setScreenSound(QObject* context, int monitorId, bool on, Done done);
    void setFrameRate(QObject* context, int rate, Done done);
    // `rate` 0 has the monitor follow the frame rate on every screen.
    void setScreenFrameRate(QObject* context, int monitorId, int rate, Done done);
    void setKeepHigherFrameRate(QObject* context, bool keep, Done done);
    // `rate` 0 says it is not known. The daemon refuses a connector no monitor answers to.
    void setOutputRefreshRate(QObject* context, const QString& connector, double rate, Done done);

signals:
    void statusChanged();
    // A GetStatus read landed, after statusChanged if it changed anything: also when it changed nothing,
    // which is how a caller learns a change it sent was refused or undone.
    void statusRead();
    // The daemon's name has a new owner: a daemon started, holding only what it had saved.
    void serviceStarted();

private:
    struct Bind {
        QPointer<QObject> context;
        QString connector;
        std::function<void(std::expected<int, QString>)> done;
    };

    QDBusMessage method(const char* name) const;
    void send(QObject* context, const QDBusMessage& message, int timeoutMs,
              std::function<void(std::expected<QDBusMessage, QString>)> onReply);
    void sendAndRefresh(QObject* context, const QDBusMessage& message, Done done);
    // Sends a setting change; on success `record` writes it into the status shown, then it is read again.
    void sendSetting(QObject* context, const QDBusMessage& message, std::function<void()> record, Done done);
    void bindNext();
    void finishBind(std::expected<int, QString> result);
    void settle(State state, QString reason, DaemonStatus status);
    // What a GetStatus read shows: Ready with it, or Unavailable with why it failed.
    void settleRead(std::expected<DaemonStatus, QString> status);

    QDBusConnection m_bus;
    QString m_service;
    DaemonTimeouts m_timeouts;
    State m_state = State::Loading;
    QString m_unavailableReason;
    DaemonStatus m_status;
    quint64 m_generation = 0;
    // The bind running is the front; the rest wait their turn.
    std::deque<Bind> m_binds;
};

} // namespace wallpaperx
