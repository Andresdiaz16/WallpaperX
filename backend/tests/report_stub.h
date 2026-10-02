// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A stand-in daemon for the plugin's three screen reports -- SetOutputSize, SetOutputVisible and
// SetOutputFocus -- for tests that run the item with no GPU. Every report is recorded in arrival
// order and answered with success unless a test told it otherwise. GetFrameFd is refused with
// NoFrame, so the item never imports anything and never arms its paint watch; GetAcquireTimeline is
// answered as a daemon from before it existed.
//
// Answering all three matters: Qt answers a method a virtual object declines with an error, which
// the item logs and retries, so a stub that knew only one report would put traffic and log lines
// into every test that are about nothing the test asked.
//
// GUI thread only: Qt dispatches this object on the thread the tests pump.

#include "tests/daemon_wire.h"
#include "tests/pump.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QList>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wallpaperx::testing {

// The connection a ReportStub is registered on. Not the process's default session connection,
// which the item under test uses: Qt answers a call made on a connection to an object registered
// on that same connection in-process and at once, so a reply the stub holds back (holdNext) would
// reach the item as an immediate empty success, and the item's calls would never cross the bus.
inline QDBusConnection stubConnection() {
    return QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("wallpaperx-report-stub"));
}

class ReportStub : public QDBusVirtualObject {
public:
    struct Call {
        QString method;
        QString output;
        QList<QVariant> args;
        qint64 atMs = 0;
        QString error;  // the error it was answered with; empty for success
    };

    ReportStub() { m_clock.start(); }

    QString introspect(const QString&) const override { return QString(); }

    // The clock every Call::atMs is read on, for a test that times something against a report.
    qint64 elapsedMs() const { return m_clock.elapsed(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface)) return false;
        const QString member = message.member();
        if (member == QLatin1String("GetFrameFd") || member == QLatin1String("GetFrameFdOnTimeline")) {
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Daemon.Error.NoFrame"),
                                                            QStringLiteral("this stub has no frames")));
        }
        if (member == QLatin1String("GetAcquireTimeline")) {
            return connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"),
                                                            QStringLiteral("no method GetAcquireTimeline here")));
        }
        if (member == QLatin1String("ReleaseBuffer") || member == QLatin1String("SetImportModifiers")) {
            return connection.send(message.createReply());
        }
        if (member != QLatin1String("SetOutputSize") && member != QLatin1String("SetOutputVisible") &&
            member != QLatin1String("SetOutputFocus")) {
            return false;
        }

        Call call;
        call.method = member;
        call.args = message.arguments();
        call.output = call.args.value(0).toString();
        call.atMs = m_clock.elapsed();
        const auto key = std::make_pair(member, call.output);
        if (auto always = m_alwaysRefuse.find(key); always != m_alwaysRefuse.end()) {
            call.error = always->second;
        } else if (auto queued = m_refusals.find(key); queued != m_refusals.end() && !queued->second.empty()) {
            call.error = queued->second.front();
            queued->second.pop_front();
        }
        m_calls.push_back(call);

        const QDBusMessage reply =
            call.error.isEmpty()
                ? message.createReply()
                : message.createErrorReply(call.error, QStringLiteral("this stub refuses %1 for %2").arg(member, call.output));
        if (m_holdNext == member) {
            m_holdNext.clear();
            m_held.emplace(connection, message);
            m_heldIndex = m_calls.size() - 1;
            return true;  // handled: the caller must not be given Qt's unknown-method error
        }
        return connection.send(reply);
    }

    // The next `member` call for `output` is answered with `errorName`; queued in order.
    void refuseNext(const QString& member, const QString& output, const QString& errorName) {
        m_refusals[{member, output}].push_back(errorName);
    }
    // Every `member` call for `output` is answered with `errorName`.
    void refuseAlways(const QString& member, const QString& output, const QString& errorName) {
        m_alwaysRefuse[{member, output}] = errorName;
    }

    // The next `member` call is recorded and not answered until releaseHeld().
    void holdNext(const QString& member) { m_holdNext = member; }
    bool isHolding() const { return m_held.has_value(); }
    // Answers the held call with `errorName`, or with success when it is empty. False when nothing
    // is held, which a caller must branch on.
    bool releaseHeld(const QString& errorName) {
        if (!m_held) return false;
        auto [connection, message] = std::move(*m_held);
        m_held.reset();
        m_calls[m_heldIndex].error = errorName;
        return connection.send(errorName.isEmpty()
                                   ? message.createReply()
                                   : message.createErrorReply(errorName, QStringLiteral("this stub refuses a held call")));
    }

    // A position in the record, for reading only what arrived after it.
    std::size_t mark() const { return m_calls.size(); }
    std::vector<Call> calls(const QString& member, const QString& output, std::size_t since = 0) const {
        std::vector<Call> out;
        for (std::size_t i = since; i < m_calls.size(); ++i) {
            if (m_calls[i].method == member && m_calls[i].output == output) out.push_back(m_calls[i]);
        }
        return out;
    }
    // Every `member` call whatever it named, a malformed one with no name included.
    std::vector<Call> allCalls(const QString& member) const {
        std::vector<Call> out;
        for (const Call& call : m_calls) {
            if (call.method == member) out.push_back(call);
        }
        return out;
    }
    // Every call for `output` after `since`, of any of the three methods, in arrival order.
    std::vector<Call> callsFor(const QString& output, std::size_t since = 0) const {
        std::vector<Call> out;
        for (std::size_t i = since; i < m_calls.size(); ++i) {
            if (m_calls[i].output == output) out.push_back(m_calls[i]);
        }
        return out;
    }

    // "Size 300x170 at 12 ms, Focus(true,false) at 14 ms refused ..." -- for failure messages.
    static std::string describe(const std::vector<Call>& calls) {
        std::string out;
        for (const Call& call : calls) {
            if (!out.empty()) out += ", ";
            out += call.method.mid(9).toStdString() + "(";
            for (qsizetype i = 1; i < call.args.size(); ++i) {
                out += (i > 1 ? "," : "") + call.args.at(i).toString().toStdString();
            }
            out += ") at " + std::to_string(call.atMs) + " ms";
            if (!call.error.isEmpty()) out += " refused " + call.error.toStdString();
        }
        return out.empty() ? "(none)" : out;
    }

private:
    QElapsedTimer m_clock;
    std::vector<Call> m_calls;
    std::map<std::pair<QString, QString>, std::deque<QString>> m_refusals;
    std::map<std::pair<QString, QString>, QString> m_alwaysRefuse;
    QString m_holdNext;
    std::optional<std::pair<QDBusConnection, QDBusMessage>> m_held;
    std::size_t m_heldIndex = 0;
};

// Takes the daemon's bus name on `bus` and puts `stub` at its path. False, with nothing to undo the
// caller need care about, when the bus is down or the name is held: the caller reports that as not
// verified, in its own words.
inline bool claimDaemonName(QDBusConnection& bus, ReportStub& stub) {
    return bus.isConnected() && bus.registerService(QString::fromUtf8(kService)) &&
           bus.registerVirtualObject(QString::fromUtf8(kPath), &stub);
}

// A daemon restart as the item sees it: the name given up and taken back, a real NameOwnerChanged
// pair. The gap lets the release reach the item before the new owner does.
inline bool restartDaemonName(QDBusConnection& bus, int gapMs = 100) {
    const bool gone = bus.unregisterService(QString::fromUtf8(kService));
    pumpFor(gapMs);
    return gone && bus.registerService(QString::fromUtf8(kService));
}

}  // namespace wallpaperx::testing
