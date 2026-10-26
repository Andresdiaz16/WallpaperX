// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A stand-in for KScreen's launcher on a private bus, and the guard that keeps the tests using it
// off the desktop's own session bus.

#include "kscreen_client.h"
#include "kscreen_dbus_source.h"
#include "tests/test_report.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVirtualObject>
#include <QDeadlineTimer>
#include <QFile>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QVariant>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

#include <unistd.h>

namespace wallpaperx::apptest {

// No default case, so -Wswitch flags a State added later.
inline QString stateName(KScreenClient::State s) {
    switch (s) {
    case KScreenClient::State::Loading: return QStringLiteral("Loading");
    case KScreenClient::State::Ready: return QStringLiteral("Ready");
    case KScreenClient::State::Unavailable: return QStringLiteral("Unavailable");
    }
    return QStringLiteral("?");
}

// Runs the event loop until `pred` holds. False at the deadline: every caller treats that as the
// check failing or the phase being abandoned, never as a result.
template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const QDeadlineTimer deadline(timeoutMs);
    QTimer tick; // guarantees a wake-up, so WaitForMoreEvents cannot sleep past the deadline
    tick.start(5);
    while (!pred()) {
        if (deadline.hasExpired()) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents);
    }
    return true;
}

// A KScreenClient's source for KScreen's service `service` on the session bus.
inline std::unique_ptr<KScreenSource> dbusSource(const QString& service, KScreenTimeouts timeouts = {}) {
    return std::make_unique<KScreenDBusSource>(QDBusConnection::sessionBus(), service, timeouts);
}

inline pid_t parentOf(pid_t pid) {
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) {
        return -1;
    }
    // "pid (comm) state ppid ...": comm may hold spaces, so read after its closing parenthesis.
    const QByteArray line = stat.readAll();
    const QList<QByteArray> fields = line.mid(line.lastIndexOf(')') + 2).split(' ');
    return fields.size() > 1 ? fields[1].toInt() : -1;
}

inline int refuseToRun(const std::string& reason) {
    wallpaperx::testing::notVerified("every goal in this file",
                                     "refusing to run: " + reason + ". Run it through ctest, which gives it a private bus");
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}

// The half of the guard that needs no Qt and opens no bus. Call it first in main(), before any
// Q*Application exists: constructing a QGuiApplication can load a platform theme that connects
// to the session bus. An exit status (77) when this process must not run, nothing when it may.
//
// CONTROL: by hand, test-side, app/tests/monitor_picker_test.cpp run by a shell under a private
//       dbus-run-session (so not its child) with `-platform nonexistent`: "refusing to run: this
//       process was not started by dbus-run-session", exit 77; with this call moved below the
//       QGuiApplication, the same run instead printed "Qt could not load the offscreen platform
//       plugin", showing the application was built first. Restored.
inline std::optional<int> refuseUnlessUnderDbusRunSession() {
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    if (address == nullptr || *address == '\0') {
        return refuseToRun("DBUS_SESSION_BUS_ADDRESS is unset, so Qt would fall back to the user's own session bus");
    }
    const pid_t parent = ::getppid();
    std::error_code ec;
    const std::filesystem::path parentExe =
        std::filesystem::read_symlink("/proc/" + std::to_string(parent) + "/exe", ec);
    if (ec || parentExe.filename() != "dbus-run-session") {
        return refuseToRun("this process was not started by dbus-run-session (its parent is " +
                           (ec ? "unreadable: " + ec.message() : parentExe.string()) + ")");
    }
    return std::nullopt;
}

// Call right after the Q*Application: repeats refuseUnlessUnderDbusRunSession(), then asks the
// bus. An exit status when this process must not run, nothing when it may.
//
// 77 when the bus is, or may be, the user's own: the binary was started outside ctest. 1 when the
// bus is private but could let something other than the fake answer: a bus with service
// directories can auto-start the real KScreen launcher, which reads the live compositor.
// `activatable` is the exact set of names the bus may start; empty for every test without
// activation. This checks names only, never what the bus would run for them: a caller allowing a
// name a real service also uses (org.kde.KScreen) must prove the service file's Exec itself.
//
// GOAL: S15.14 -- a test that stands up a fake KScreen never does so on a bus it did not get from
//       dbus-run-session, nor on one that can start anything but the test's own stand-ins.
// WHY-THIS-SHAPE: the same positive proof as backend/tests/name_watcher.h (parent is
//       dbus-run-session, and the bus daemon is that parent's child), asked of no bus until the
//       parent is known; then ListActivatableNames must be exactly the allowed set, which a stock
//       dbus-run-session fails (it lists org.kde.KScreen among its activatable names).
// CONTROL: by hand, each observed: under plain `dbus-run-session --`, with no configuration of
//       the test's -> exit 1, "can start services on demand" listing
//       org.kde.KScreen; kscreen_launcher_test with an extra org.kde.KScreen.service dropped into
//       its service directory -> exit 1, naming it; under ctest -> passes this guard.
inline std::optional<int> refuseUnlessPrivateBus(const QStringList& activatable = {}) {
    using namespace wallpaperx::testing;
    const auto broken = [](const std::string& fault) {
        harnessFailure(fault);
        static_cast<void>(reportHarnessFaults("in the private bus this run was given, not in the app"));
        return 1;
    };
    if (const auto refused = refuseUnlessUnderDbusRunSession()) {
        return refused;
    }
    const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
    const pid_t parent = ::getppid();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return broken(std::string("the session bus at ") + address + " could not be reached: " +
                      bus.lastError().message().toStdString());
    }
    const QDBusReply<uint> busPid = bus.interface()->call(QStringLiteral("GetConnectionUnixProcessID"),
                                                          QStringLiteral("org.freedesktop.DBus"));
    if (!busPid.isValid()) {
        return refuseToRun(std::string("the bus at ") + address + " would not say which process runs it");
    }
    if (parentOf(static_cast<pid_t>(busPid.value())) != parent) {
        return refuseToRun(std::string("the bus at ") + address + " was not started by the dbus-run-session that started this process");
    }
    const QDBusReply<QStringList> listed = bus.interface()->activatableServiceNames();
    if (!listed.isValid()) {
        return broken("the private bus would not list its activatable names");
    }
    QStringList others = listed.value();
    others.removeAll(QStringLiteral("org.freedesktop.DBus"));
    others.sort();
    QStringList expected = activatable;
    expected.sort();
    if (others != expected) {
        return broken("the private bus can start services on demand (" + others.join(QStringLiteral(", ")).toStdString() +
                      "), not exactly the test's own (" + expected.join(QStringLiteral(", ")).toStdString() +
                      "), so something other than the fake could answer; run it through ctest with its own bus config");
    }
    return std::nullopt;
}

// Serves "/" org.kde.KScreen.requestBackend and "/backend" org.kde.kscreen.Backend getConfig and
// getEdid under `service`, on its own connection, so every reply is marshalled over the bus.
// Each method either answers at once from the fields below or is held for the test to release.
class FakeKScreen : public QDBusVirtualObject {
public:
    enum class Mode { Answer, Hold, Error };

    // A connection of its own per fake, even for the same name: Qt hands back the existing
    // connection for a name already in use, so two fakes queued for one service would share it.
    explicit FakeKScreen(QString service)
        : m_service(std::move(service))
        , m_bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus,
                                              QStringLiteral("fake-%1-%2").arg(m_service).arg(++s_instances))) {}

    ~FakeKScreen() override {
        m_bus.unregisterService(m_service);
        m_bus.unregisterObject(QStringLiteral("/"), QDBusConnection::UnregisterTree);
        QDBusConnection::disconnectFromBus(m_bus.name());
    }

    // Empty on success, otherwise why the fake could not stand up.
    QString start() {
        if (!m_bus.isConnected()) {
            return QStringLiteral("the fake's connection failed: ") + m_bus.lastError().message();
        }
        if (!m_bus.registerVirtualObject(QStringLiteral("/"), this, QDBusConnection::SubPath)) {
            return QStringLiteral("could not register the fake's objects");
        }
        if (!m_bus.registerService(m_service)) {
            return QStringLiteral("could not take the name ") + m_service;
        }
        // The owner the bus reports, asked from the other connection, is the proof that calls
        // addressed to the name reach this object and nothing else.
        const QString owner = QDBusConnection::sessionBus().interface()->serviceOwner(m_service);
        if (owner != m_bus.baseService()) {
            return QStringLiteral("%1 is owned by %2, not by the fake (%3)").arg(m_service, owner, m_bus.baseService());
        }
        return {};
    }

    // Like start(), but queues behind the connection that owns the name: when that owner releases
    // it, the bus hands it to this fake in one NameOwnerChanged with both owners set.
    QString startQueued() {
        if (!m_bus.isConnected()) {
            return QStringLiteral("the fake's connection failed: ") + m_bus.lastError().message();
        }
        if (!m_bus.registerVirtualObject(QStringLiteral("/"), this, QDBusConnection::SubPath)) {
            return QStringLiteral("could not register the fake's objects");
        }
        const QDBusReply<QDBusConnectionInterface::RegisterServiceReply> queued = m_bus.interface()->registerService(
            m_service, QDBusConnectionInterface::QueueService, QDBusConnectionInterface::DontAllowReplacement);
        if (!queued.isValid() || queued.value() != QDBusConnectionInterface::ServiceQueued) {
            return QStringLiteral("could not queue for the name ") + m_service;
        }
        return {};
    }
    // Asked from the other connection, as start() does.
    bool owns() const {
        return QDBusConnection::sessionBus().interface()->serviceOwner(m_service).value() == m_bus.baseService();
    }

    QString service() const { return m_service; }

    // Behaviour, read at the moment each call arrives.
    Mode backendMode = Mode::Answer;
    QVariant backendAnswer = true;
    Mode configMode = Mode::Answer;
    QVariant config;
    QString configError = QStringLiteral("the fake was told to refuse");
    // The D-Bus error name every Mode::Error reply carries.
    QString errorName = QStringLiteral("org.kde.test.Error");
    Mode edidMode = Mode::Answer;
    QMap<int, QByteArray> edids;

    // What arrived.
    int backendCalls = 0;
    int configCalls = 0;
    int edidCalls = 0;
    QList<QVariantList> backendArguments;
    QList<int> edidIds;
    QSet<QString> callers;
    QList<QDBusMessage> heldBackend;
    QList<QDBusMessage> heldConfig;
    QList<QDBusMessage> heldEdid;

    void reply(const QDBusMessage& call, const QVariant& value) { m_bus.send(call.createReply(value)); }
    void replyError(const QDBusMessage& call, const QString& message) {
        m_bus.send(call.createErrorReply(errorName, message));
    }
    void emitConfigChanged() {
        QDBusMessage signal = QDBusMessage::createSignal(QStringLiteral("/backend"), QStringLiteral("org.kde.kscreen.Backend"),
                                                         QStringLiteral("configChanged"));
        signal << QVariantMap{};
        m_bus.send(signal);
    }

    QString introspect(const QString&) const override { return {}; }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection&) override {
        if (message.type() != QDBusMessage::MethodCallMessage) {
            return false;
        }
        callers.insert(message.service());
        const QString where = message.path() + QLatin1Char(' ') + message.interface() + QLatin1Char('.') + message.member();
        if (where == QLatin1String("/ org.kde.KScreen.requestBackend")) {
            ++backendCalls;
            backendArguments.append(message.arguments());
            act(message, backendMode, backendAnswer, heldBackend);
        } else if (where == QLatin1String("/backend org.kde.kscreen.Backend.getConfig")) {
            ++configCalls;
            act(message, configMode, config, heldConfig);
        } else if (where == QLatin1String("/backend org.kde.kscreen.Backend.getEdid")) {
            ++edidCalls;
            const int id = message.arguments().value(0).toInt();
            edidIds.append(id);
            act(message, edidMode, edids.value(id), heldEdid);
        } else {
            m_bus.send(message.createErrorReply(QDBusError::UnknownMethod, QStringLiteral("the fake has no ") + where));
        }
        return true;
    }

private:
    void act(const QDBusMessage& message, Mode mode, const QVariant& answer, QList<QDBusMessage>& held) {
        switch (mode) {
        case Mode::Answer: reply(message, answer); break;
        case Mode::Hold: held.append(message); break;
        case Mode::Error: replyError(message, configError); break;
        }
    }

    static inline int s_instances = 0;
    QString m_service;
    QDBusConnection m_bus;
};

} // namespace wallpaperx::apptest
