// SPDX-License-Identifier: GPL-2.0-or-later

#include "config_read.h"

#include "buffer_claims.h"
#include "daemon/daemon_bus.h"
#include "daemon/http_server.h"
#include "daemon/log_escape.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QJSEngine>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace wallpaperx {

int doubledWait(int baseMs, int times, int capMs) {
    int ms = baseMs;
    for (int i = 0; i < times && ms < capMs; ++i) ms *= 2;
    return std::min(ms, capMs);
}

void watchConfigChanged(QObject* receiver, const char* slot) {
    if (!QDBusConnection::sessionBus().connect(QString(), QString::fromUtf8(kDaemonObjectPath), QString::fromUtf8(kDaemonInterface),
                                               QStringLiteral("ConfigChanged"), receiver, slot)) {
        std::fprintf(stderr, "wallpaperx: could not subscribe to WallpaperX config changes -- %s\n",
                     sanitizeForLog(QDBusConnection::sessionBus().lastError().message().toStdString()).c_str());
    }
}

DaemonWatch* DaemonWatch::instance() {
    static QPointer<DaemonWatch> watch;
    if (watch) return watch;
    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr || !QDBusConnection::sessionBus().isConnected()) return nullptr;
    watch = new DaemonWatch(app);
    return watch;
}

bool DaemonWatch::lookupFailed() {
    const DaemonWatch* watch = instance();
    return watch != nullptr && watch->m_lookupFailed && BufferClaims::instance().currentDaemon().lifetime == 0;
}

DaemonWatch::DaemonWatch(QObject* parent) : QObject(parent) {
    auto* watcher = new QDBusServiceWatcher(QString::fromUtf8(kDaemonBusName), QDBusConnection::sessionBus(),
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    // Every report is a change of owner, departures included, and each arrives here once.
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString&, const QString&, const QString& owner) {
                BufferClaims::instance().daemonReplaced(owner.toStdString());
                emit replaced(owner);
            });
    // After the watcher: the bus sends its signals and the answer on one connection, in order, so a
    // change is either already in the answer or reported after it.
    askWhoOwns();
}

void DaemonWatch::askWhoOwns() {
    QDBusMessage request = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("/org/freedesktop/DBus"),
                                                          QStringLiteral("org.freedesktop.DBus"),
                                                          QStringLiteral("GetNameOwner"));
    request.setArguments({QString::fromUtf8(kDaemonBusName)});
    // QtDBus's default timeout, every time: a bus slow at login is still answering, and timing an
    // ask out would only throw the answer away and tell every screen the daemon could not be reached.
    auto* call = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(request), this);
    connect(call, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* done) {
        done->deleteLater();
        QString owner;
        if (!done->isError()) {
            owner = done->reply().arguments().value(0).toString();
        } else if (done->error().name() != QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")) {
            // Lost rather than answered. Asked again, since until some answer arrives no request
            // can be addressed; a change reported meanwhile answers it instead.
            m_lookupFailed = true;
            emit lookupLost();
            QTimer::singleShot(doubledWait(kLookupRetryMs, m_failedAsks, kLookupRetryMaxMs), this, [this] {
                if (BufferClaims::instance().currentDaemon().lifetime == 0) askWhoOwns();
            });
            ++m_failedAsks;
            return;
        }
        if (BufferClaims::instance().daemonNamed(owner.toStdString())) emit named(owner);
    });
}

ConfigWatch::ConfigWatch(QObject* parent) : QObject(parent) {
    watchConfigChanged(this, SIGNAL(configChanged()));
    if (DaemonWatch* watch = DaemonWatch::instance()) {
        // A departure too: the screen then says the daemon is not running.
        connect(watch, &DaemonWatch::replaced, this, &ConfigWatch::configChanged);
        // A daemon that took its name before the watch's rule reached the bus is heard of only here,
        // and main.qml's first read may have gone before its port was open. An empty answer is a
        // daemon still absent, which that read already reported.
        connect(watch, &DaemonWatch::named, this, [this](const QString& owner) {
            if (!owner.isEmpty()) emit configChanged();
        });
    }
}

bool ConfigWatch::daemonAbsent() const {
    const BufferClaims::Daemon daemon = BufferClaims::instance().currentDaemon();
    return daemon.lifetime != 0 && daemon.owner.empty();
}

ConfigRead* ConfigWatch::fetch(const QString& output) {
    auto* read = new ConfigRead(output, this);
    // Not left to the collector: main.qml's handler captures the read, and a JavaScript function
    // connected to a signal is a root the collector never sees past, so it would never be freed.
    QJSEngine::setObjectOwnership(read, QJSEngine::CppOwnership);
    return read;
}

namespace {

// One for the process: each manager runs an HTTP thread of its own. Parented to the application, so
// its replies end before the event loop does.
QNetworkAccessManager* configNetwork() {
    static QPointer<QNetworkAccessManager> shared;
    if (shared.isNull()) {
        shared = new QNetworkAccessManager(QCoreApplication::instance());
        // An application-wide proxy would otherwise carry a loopback request off the machine.
        shared->setProxy(QNetworkProxy::NoProxy);
        shared->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    }
    return shared;
}

// Whether `error` is the one Qt reports for an HTTP 4xx or 5xx status itself. Any other error --
// refused, cut short, a malformed reply, a 2xx whose body cannot be decoded -- leaves no whole answer.
// It says nothing about the body: Qt reports a 4xx or 5xx cut short mid-body this way too, about one
// time in four on a loaded machine, instead of RemoteHostClosedError. 407 is filed under proxy
// errors; with NoProxy set only the server's own status line can produce it.
bool isStatusError(QNetworkReply::NetworkError error) {
    return (error >= QNetworkReply::ContentAccessDenied && error <= QNetworkReply::UnknownContentError) ||
           error == QNetworkReply::ProtocolInvalidOperationError ||
           error == QNetworkReply::ProxyAuthenticationRequiredError ||
           (error >= QNetworkReply::InternalServerError && error <= QNetworkReply::UnknownServerError);
}

}  // namespace

ConfigRead::ConfigRead(const QString& output, QObject* parent) : QObject(parent) {
    QUrl target;
    target.setScheme(QStringLiteral("http"));
    target.setHost(QString::fromLatin1(HttpServer::kLoopbackHost));
    target.setPort(HttpServer::kDefaultPort);
    target.setPath(QStringLiteral("/config"));
    // Every byte but the unreserved ones encoded, '+' included, which the daemon would read as a
    // space. The timestamp defeats any cache between here and the daemon: a wallpaper the user just
    // chose must not wait on a stale 200.
    target.setQuery(QStringLiteral("output=") + QString::fromLatin1(QUrl::toPercentEncoding(output)) +
                        QStringLiteral("&t=") + QString::number(QDateTime::currentMSecsSinceEpoch()),
                    QUrl::StrictMode);
    QNetworkRequest request(target);
    request.setTransferTimeout(kTimeoutMs);
    m_reply = configNetwork()->get(request);
    // The body is taken in as it arrives, so the reply's own buffer stays small and its HTTP thread
    // stops reading the socket while it is full.
    m_reply->setReadBufferSize(kMaxBodyBytes);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!takeArrived()) complete();
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        takeArrived();
        complete();
    });
}

// A reply left here is a plugin unloaded mid-read: it ends unheard.
ConfigRead::~ConfigRead() {
    if (m_reply.isNull()) return;
    m_reply->disconnect(this);
    m_reply->abort();
    m_reply->deleteLater();
}

bool ConfigRead::takeArrived() {
    // Not open once Qt has ended the transfer itself, as its timeout does; reading then only warns.
    if (m_reply->isOpen() && m_received.size() <= kMaxBodyBytes) m_received += m_reply->readAll();
    return m_received.size() <= kMaxBodyBytes;
}

void ConfigRead::complete() {
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError error = reply->error();
    const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
    // Unhooked first: on an unfinished reply abort() emits finished() from inside itself.
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    const std::optional<qint64> declaredLength =
        length.isValid() ? std::optional<qint64>(length.toLongLong()) : std::nullopt;
    finish(ending(error, status, declaredLength, std::exchange(m_received, QByteArray())));
}

ConfigRead::Ending ConfigRead::ending(QNetworkReply::NetworkError error, int status,
                                      std::optional<qint64> declaredLength, const QByteArray& received) {
    if (received.size() > kMaxBodyBytes) return {Outcome::Answered, status, QString()};
    // Our own aborts never reach here (complete() reads the error first, the destructor's is
    // unhooked), so a cancelled reply was ended by Qt -- its transfer timeout, or the manager's
    // teardown.
    if (error == QNetworkReply::TimeoutError || error == QNetworkReply::OperationCanceledError) {
        return {Outcome::TimedOut, 0, QString()};
    }
    // Qt's status error does not mean the body is whole, so a refusal counts only when all of its
    // declared length came. The daemon always declares one; without it (chunked, or a body Qt
    // decompressed and dropped the header for) a cut cannot be told from an end.
    const bool wholeRefusal = status >= 400 && isStatusError(error) && declaredLength.has_value() &&
                              *declaredLength == received.size();
    if (error == QNetworkReply::NoError || wholeRefusal) {
        return {Outcome::Answered, status, QString::fromUtf8(received)};
    }
    if (status != 0) return {Outcome::Unreadable, status, QString()};
    return {Outcome::None, 0, QString()};
}

QString ConfigRead::outcome() const {
    switch (m_outcome) {
    case Outcome::Unfinished: return QString();
    case Outcome::Answered: return QStringLiteral("answered");
    case Outcome::Unreadable: return QStringLiteral("unreadable");
    case Outcome::None: return QStringLiteral("none");
    case Outcome::TimedOut: return QStringLiteral("timedOut");
    }
    return QString();
}

void ConfigRead::finish(const Ending& ending) {
    m_outcome = ending.outcome;
    m_status = ending.status;
    m_body = ending.body;
    // Posted before emitting: the deletion waits for every handler, and is dropped if a handler
    // deletes the watch and this read with it.
    deleteLater();
    emit finished();
}

}  // namespace wallpaperx
