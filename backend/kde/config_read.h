// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QByteArray>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <optional>

namespace wallpaperx {

// How long the owner lookup first waits to ask again after a lost one. Short, because until one
// answers no item can fetch at all.
inline constexpr int kLookupRetryMs = 2000;

// Where a failed owner lookup's retry stops doubling. Short for the same reason; a lookup is one
// message to the bus itself, not to the daemon.
inline constexpr int kLookupRetryMaxMs = 8000;

// `baseMs` doubled `times` times, but never past `capMs`.
int doubledWait(int baseMs, int times, int capMs);

// From any sender, unlike FrameReady: naming the service makes QtDBus resolve its owner before the
// rule is in place. A forged one costs a /config read, never more than one at a time, and a size
// report where one is owed. Not retried: for a new receiver connect() is false only on a disconnected
// bus, never for a rule the bus refuses.
void watchConfigChanged(QObject* receiver, const char* slot);

// Who owns the daemon's bus name, watched once for the process and written to the ledger, which is
// the one record of it. Not per item: with no item alive a per-item watch sees nothing, so a daemon
// replaced while the desktop had no wallpaper item would stay recorded, and every request and
// release would go to a name that has left. GUI thread.
class DaemonWatch : public QObject {
    Q_OBJECT
public:
    // Made on first use, on qApp, and never destroyed before it. Null without a bus.
    static DaemonWatch* instance();

    // Whether no owner is known because asking the bus failed, rather than because the answer has
    // not come yet. Only meaningful in lifetime 0, which the first answer or change ends.
    static bool lookupFailed();

signals:
    // A new daemon lifetime has begun, owned by `owner` -- empty for nobody. The ledger records it
    // before this is emitted.
    void replaced(const QString& owner);
    // The bus answered who owned the name when the watch began, which begins the first lifetime.
    void named(const QString& owner);
    // Asking the bus who owns the name failed; it is asked again, later each time.
    void lookupLost();

private:
    explicit DaemonWatch(QObject* parent);
    void askWhoOwns();

    bool m_lookupFailed = false;
    // Asks lost so far, which the wait before the next ask grows with. The retry covers a lookup lost
    // while the owner stays put, which no change notice would report.
    int m_failedAsks = 0;
};

// One GET of the daemon's /config about one screen, answered once through finished(). Made with this
// plugin's own network manager, not QML's XMLHttpRequest: the lock screen gives its QML engine a
// manager that refuses every request, and the screen there would read as having no daemon. It asks
// only the daemon's /config and follows no redirect, so the lock screen's engine gains no other way
// out. GUI thread.
class ConfigRead : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

    // How the read ended, empty while unfinished. A string because main.qml cannot name this module's
    // types, so an enum's names would reach it as bare numbers:
    //   "answered"   -- a whole answer, in status and responseText;
    //   "unreadable" -- a status line came, then nothing known whole: cut short, malformed,
    //                   undecodable, or a 4xx/5xx declaring no length;
    //   "none"       -- nothing answered: refused, or closed before a status line;
    //   "timedOut"   -- nothing arrived for kTimeoutMs.
    Q_PROPERTY(QString outcome READ outcome NOTIFY finished)
    // The HTTP status when one came, else 0. A timed-out read reports 0 whatever arrived before.
    Q_PROPERTY(int status READ status NOTIFY finished)
    // An answered read's body. Empty for every other outcome, and for a body past kMaxBodyBytes,
    // which still counts as answered.
    Q_PROPERTY(QString responseText READ responseText NOTIFY finished)

public:
    // The most of a body taken in. Whatever holds the port sends it and the shell parses it, so past
    // this the transfer is stopped rather than buffered.
    static constexpr qint64 kMaxBodyBytes = 65536;

    // How long the read waits with nothing arriving. The daemon answers from memory; a server that
    // accepts and never answers would otherwise hold main.qml's one read slot for good.
    static constexpr int kTimeoutMs = 5000;

    enum class Outcome { Unfinished, Answered, Unreadable, None, TimedOut };

    // What a finished read reports: its outcome, and the status and body it keeps.
    struct Ending {
        Outcome outcome;
        int status;
        QString body;
    };

    // How a read ended, from what Qt reported: its error, the HTTP status (0 for none), the
    // Content-Length it kept (none when absent), and every byte taken in. A body past kMaxBodyBytes
    // is still an answer, kept empty. A 4xx/5xx is a whole answer only when its declared length
    // arrived exactly, since Qt can report one cut short as the status's own error.
    static Ending ending(QNetworkReply::NetworkError error, int status, std::optional<qint64> declaredLength,
                         const QByteArray& received);

    ConfigRead(const QString& output, QObject* parent);
    ~ConfigRead() override;

    QString outcome() const;
    int status() const { return m_status; }
    QString responseText() const { return m_body; }

signals:
    // Always queued, so a caller connecting right after creating the read hears it.
    void finished();

private:
    // Moves what has arrived into m_received; false once that is past kMaxBodyBytes.
    bool takeArrived();
    // Ends the transfer, keeping what it established, and finishes.
    void complete();
    void finish(const Ending& ending);

    QPointer<QNetworkReply> m_reply;
    QByteArray m_received;
    Outcome m_outcome = Outcome::Unfinished;
    int m_status = 0;
    QString m_body;
};

// Says when what the daemon's /config answers may have changed, so main.qml asks then, and otherwise
// only to retry a failed read: the daemon announced a change, the first lookup of its bus name found
// an owner, or the name changed owner -- a daemon arriving, leaving or replaced. main.qml asks once
// this watch has loaded; this reports what comes after its creation, and makes the reads. GUI thread.
class ConfigWatch : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(WallpaperXConfigWatch)

public:
    explicit ConfigWatch(QObject* parent = nullptr);

    // Whether the bus has said nobody owns the daemon's name. False while that is not yet known,
    // and without a bus. A query of the moment, with no change signal: an owner change already
    // emits configChanged.
    Q_INVOKABLE bool daemonAbsent() const;

    // A read of what the daemon says about the screen named `output`. It deletes itself once its
    // finished() handlers have run; unanswered, it goes with this watch. Read its properties inside
    // the handler, never after.
    Q_INVOKABLE wallpaperx::ConfigRead* fetch(const QString& output);

signals:
    void configChanged();
};

}  // namespace wallpaperx
