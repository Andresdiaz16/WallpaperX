// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Test-only stand-ins for the two services Apply talks to, on the private bus: the WallpaperX
// daemon (io.wallpaperx.Daemon) and Plasma's shell (org.kde.PlasmaShell.evaluateScript). Each takes
// a test-owned name, so nothing here can answer for, or reach, the real services.
//
// The shell stand-in runs the script it receives against a model of plasma-workspace 6.7.5's
// scripting API (shell/scripting/scriptengine_v1.cpp, shell/shellcorona.cpp), as read from that
// source, not checked against a running Plasma:
//   - knownWallpaperPlugins() is an object keyed by each plugin's display name (KPlugin.Name);
//   - screenForConnector(name) is ScreenPool::idForName: Plasma's own index, -1 for a name it has
//     no screen for; a Plasma without the function is modelled by leaving it out;
//   - desktops() lists every activity's desktop; one of the current activity has its `screen`,
//     one of another activity has -1 and its saved `lastScreen`, read by readConfig(key, default);
//   - print() appends to the reply; a thrown error becomes a Failed reply reading
//     "Error: <it> at line N\n\nBacktrace:\n...", as ScriptEngine::evaluateScript words it;
//   - locked widgets and a refused policy answer Failed before the script runs, with Plasma's text.

#include "kscreen_config.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QHash>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>
#include <utility>

namespace wallpaperx::apptest {

struct FakeCall {
    QString member;
    QString signature;
    QVariantList args;
};

// Every call either stand-in received, in arrival order, as "daemon:Member" or "shell:Member".
using CallLog = QStringList;

class FakeService : public QDBusVirtualObject {
public:
    FakeService(QString service, QString path, QString tag, CallLog* log)
        : m_service(std::move(service))
        , m_path(std::move(path))
        , m_tag(std::move(tag))
        , m_log(log)
        , m_bus(QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-") + m_service)) {}
    ~FakeService() override {
        stop();
        QDBusConnection::disconnectFromBus(m_bus.name());
    }
    FakeService(const FakeService&) = delete;
    FakeService& operator=(const FakeService&) = delete;

    // Empty on success, otherwise why the stand-in could not take its name.
    QString start() {
        if (!m_bus.isConnected()) {
            return QStringLiteral("the stand-in's connection failed: ") + m_bus.lastError().message();
        }
        if (!m_registered && !m_bus.registerVirtualObject(m_path, this)) {
            return QStringLiteral("could not register ") + m_path;
        }
        m_registered = true;
        if (!m_bus.registerService(m_service)) {
            return QStringLiteral("could not take the name ") + m_service;
        }
        const QString owner = QDBusConnection::sessionBus().interface()->serviceOwner(m_service);
        if (owner != m_bus.baseService()) {
            return QStringLiteral("%1 is owned by %2, not by the stand-in").arg(m_service, owner);
        }
        return {};
    }
    // Gives the name up; calls to it then fail as the bus answers for a name nobody owns.
    void stop() { m_bus.unregisterService(m_service); }

    QString service() const { return m_service; }

    QList<FakeCall> named(const QString& member) const {
        QList<FakeCall> out;
        for (const FakeCall& c : calls) {
            if (c.member == member) {
                out.append(c);
            }
        }
        return out;
    }

    // Replies held back by `hold`, sent in arrival order.
    void release() {
        const QList<QDBusMessage> pending = std::exchange(held, {});
        for (const QDBusMessage& m : pending) {
            answer(m);
        }
    }
    // The held reply at `index` alone, built now; the rest stay held.
    void releaseAt(qsizetype index) {
        if (index >= 0 && index < held.size()) {
            answer(held.takeAt(index));
        }
    }
    // The held call at `index` answered with an error instead; the rest stay held. `refuse` answers
    // at once, so a failure that must arrive after a later call was sent needs this.
    void refuseAt(qsizetype index, const QString& name, const QString& message) {
        if (index >= 0 && index < held.size()) {
            m_bus.send(held.takeAt(index).createErrorReply(name, message));
        }
    }

    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& m, const QDBusConnection&) override {
        if (m.type() != QDBusMessage::MethodCallMessage) {
            return false;
        }
        FakeCall call{m.member(), m.signature(), {}};
        for (const QVariant& a : m.arguments()) {
            call.args.append(plainVariant(a));
        }
        calls.append(call);
        if (m_log != nullptr) {
            m_log->append(m_tag + u':' + m.member());
        }
        if (const auto r = refuse.constFind(m.member()); r != refuse.cend()) {
            m_bus.send(m.createErrorReply(r->first, r->second));
            return true;
        }
        accept(call);
        if (hold.contains(m.member())) {
            held.append(m);
            return true;
        }
        answer(m);
        return true;
    }

    QList<FakeCall> calls;
    // Member -> (error name, message): refused at once, before it changes anything.
    QHash<QString, QPair<QString, QString>> refuse;
    // Members whose replies wait for release().
    QSet<QString> hold;
    QList<QDBusMessage> held;

protected:
    // What a call changes, applied when it arrives.
    virtual void accept(const FakeCall&) {}
    // The reply, built when it is sent, so a held GetStatus reports the state at release.
    virtual void answer(const QDBusMessage& m) = 0;

    QString m_service;
    QString m_path;
    QString m_tag;
    CallLog* m_log;
    QDBusConnection m_bus;
    bool m_registered = false;
};

// io.wallpaperx.Daemon at /io/wallpaperx/Daemon: GetStatus from the fields below, and the setters
// the app calls, each changing them as the daemon would.
class FakeDaemon : public FakeService {
public:
    FakeDaemon(QString service, CallLog* log)
        : FakeService(std::move(service), QStringLiteral("/io/wallpaperx/Daemon"), QStringLiteral("daemon"), log) {}

    QMap<QString, int> outputs;
    QSet<int> wallpapers;
    // A null value is a mode this build does not know (a newer daemon's).
    QMap<int, QJsonValue> pause;
    QString persistence = QStringLiteral("ok");
    QString persistenceDetail = QStringLiteral("the disk is full");
    // Sound on every screen and per monitor as GetStatus's `sound` carries them; null is a value a
    // newer daemon wrote. With reportsSound false, GetStatus has no `sound` key, as a daemon older than
    // sound settings answers.
    bool reportsSound = true;
    QJsonValue sound = true;
    QMap<int, QJsonValue> screenSound;

    // The daemon's PersistenceChanged, from the connection owning the stand-in's name.
    bool announcePersistenceChanged() {
        return m_bus.send(QDBusMessage::createSignal(m_path, QStringLiteral("io.wallpaperx.Daemon"), QStringLiteral("PersistenceChanged")));
    }

    QString status() const {
        QJsonObject names;
        QJsonObject shown;
        QJsonObject modes;
        for (auto it = outputs.cbegin(); it != outputs.cend(); ++it) {
            names.insert(it.key(), it.value());
        }
        for (const int id : wallpapers) {
            shown.insert(QString::number(id), QJsonObject{{QStringLiteral("type"), QStringLiteral("video")}});
        }
        for (auto it = pause.cbegin(); it != pause.cend(); ++it) {
            modes.insert(QString::number(it.key()), QJsonObject{{QStringLiteral("mode"), it.value()}, {QStringLiteral("paused"), false}});
        }
        QJsonObject root{{QStringLiteral("outputs"), names},
                         {QStringLiteral("outputs_literal"), names},
                         {QStringLiteral("wallpapers"), shown},
                         {QStringLiteral("pause"), modes},
                         {QStringLiteral("persistence"),
                          QJsonObject{{QStringLiteral("status"), persistence}, {QStringLiteral("detail"), persistenceDetail}}}};
        if (reportsSound) {
            // As the daemon lists them: every monitor holding a wallpaper, and every one with a setting.
            QSet<int> listed = wallpapers;
            for (auto it = screenSound.cbegin(); it != screenSound.cend(); ++it) {
                listed.insert(it.key());
            }
            QJsonObject monitors;
            for (const int id : listed) {
                monitors.insert(QString::number(id), QJsonObject{{QStringLiteral("enabled"), screenSound.value(id, QJsonValue(true))},
                                                                 {QStringLiteral("heard"), false}});
            }
            root.insert(QStringLiteral("sound"), QJsonObject{{QStringLiteral("enabled"), sound}, {QStringLiteral("monitors"), monitors}});
        }
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

protected:
    void accept(const FakeCall& call) override {
        if (call.member == u"SetMonitorOutput") {
            const int id = call.args.value(0).toInt();
            for (auto it = outputs.begin(); it != outputs.end();) {
                it = it.value() == id ? outputs.erase(it) : std::next(it);
            }
            outputs.insert(call.args.value(1).toString(), id);
        } else if (call.member == u"SetWallpaperWithProperties") {
            wallpapers.insert(call.args.value(2).toInt());
        } else if (call.member == u"SetMonitorPause") {
            pause.insert(call.args.value(0).toInt(), call.args.value(1).toString());
        } else if (call.member == u"SetSound") {
            sound = call.args.value(0).toBool();
        } else if (call.member == u"SetMonitorSound") {
            // On is no setting; status() still lists the monitor as on while it holds a wallpaper.
            const int id = call.args.value(0).toInt();
            if (call.args.value(1).toBool()) {
                screenSound.remove(id);
            } else {
                screenSound.insert(id, false);
            }
        }
    }
    void answer(const QDBusMessage& m) override {
        m_bus.send(m.member() == u"GetStatus" ? m.createReply(status()) : m.createReply());
    }
};

// What one run of the Plasma model did.
struct PlasmaRun {
    bool threw = false;
    // Failed's message as Plasma words it, when it threw.
    QString error;
    // print()'s output, the reply when it did not throw.
    QString printed;
    // Every desktop's wallpaperPlugin after the run, in desktops() order.
    QStringList plugins;
    // Every name screenForConnector was asked for.
    QStringList asked;
    // Set by a script that broke out of a string literal (see the injection check).
    bool injected = false;
};

// One desktop of the model: `screen` is -1 for another activity's; `lastScreen` is left out of its
// config when it is a null QJsonValue.
inline QJsonObject plasmaDesktop(int screen, const QJsonValue& lastScreen, const char* activity) {
    QJsonObject config;
    if (!lastScreen.isNull()) {
        config.insert(QStringLiteral("lastScreen"), lastScreen);
    }
    return QJsonObject{{QStringLiteral("screen"), screen},
                       {QStringLiteral("cfg"), config},
                       {QStringLiteral("activity"), QLatin1StringView(activity)},
                       {QStringLiteral("wallpaperPlugin"), QStringLiteral("org.kde.image")}};
}

// The model's state: installed plugins by display name, Plasma's screen order by connector, the
// desktops, and the switches below.
struct PlasmaState {
    QStringList plugins{QStringLiteral("Image"), QStringLiteral("WallpaperX")};
    bool hasScreenForConnector = true;
    QStringList order;
    QJsonArray desktops;
    // print() reaching the reply; false models a shell that runs the script but returns nothing.
    bool printReaches = true;
    bool widgetsLocked = false;
    bool policyRefuses = false;
};

inline PlasmaRun runPlasmaModel(const PlasmaState& state, const QString& script) {
    QJSEngine js;
    const QJsonObject mock{{QStringLiteral("plugins"), QJsonArray::fromStringList(state.plugins)},
                           {QStringLiteral("hasScreenForConnector"), state.hasScreenForConnector},
                           {QStringLiteral("order"), QJsonArray::fromStringList(state.order)},
                           {QStringLiteral("desktops"), state.desktops},
                           {QStringLiteral("printReaches"), state.printReaches}};
    // JSON allows U+2028 and U+2029 raw in a string; this engine's JavaScript does not.
    QString json = QString::fromUtf8(QJsonDocument(mock).toJson(QJsonDocument::Compact));
    const QString backslash(QChar(u'\\'));
    json.replace(QChar(0x2028), backslash + QLatin1String("u2028")).replace(QChar(0x2029), backslash + QLatin1String("u2029"));
    const QJSValue setup = js.evaluate(QStringLiteral("var MOCK = %1;").arg(json) +
                                       QStringLiteral(R"(
var __printed = "";
var __asked = [];
function print(s) { if (MOCK.printReaches) __printed += String(s); }
function knownWallpaperPlugins(formFactor) {
    var o = new Array(MOCK.plugins.length);
    MOCK.plugins.forEach(function (n) { o[n] = []; });
    return o;
}
if (MOCK.hasScreenForConnector)
    this.screenForConnector = function (c) { __asked.push(String(c)); return MOCK.order.indexOf(String(c)); };
function desktops() {
    return MOCK.desktops.map(function (d) {
        d.readConfig = function (k, def) { return (k in d.cfg) ? d.cfg[k] : def; };
        return d;
    });
}
)"));
    PlasmaRun run;
    if (setup.isError()) {
        run.threw = true;
        run.error = QStringLiteral("the Plasma model itself did not load: ") + setup.toString();
        return run;
    }
    const QJSValue result = js.evaluate(script);
    const QJSValue global = js.globalObject();
    const QJSValue desktops = global.property(QStringLiteral("MOCK")).property(QStringLiteral("desktops"));
    for (int i = 0; i < desktops.property(QStringLiteral("length")).toInt(); ++i) {
        run.plugins.append(desktops.property(quint32(i)).property(QStringLiteral("wallpaperPlugin")).toString());
    }
    const QJSValue asked = global.property(QStringLiteral("__asked"));
    for (int i = 0; i < asked.property(QStringLiteral("length")).toInt(); ++i) {
        run.asked.append(asked.property(quint32(i)).toString());
    }
    run.injected = global.hasProperty(QStringLiteral("pwned"));
    if (result.isError()) {
        run.threw = true;
        run.error = QStringLiteral("Error: %1 at line %2\n\nBacktrace:\n%3")
                        .arg(result.toString())
                        .arg(result.property(QStringLiteral("lineNumber")).toInt())
                        .arg(result.property(QStringLiteral("stack")).toString());
        return run;
    }
    run.printed = global.property(QStringLiteral("__printed")).toString();
    return run;
}

// org.kde.PlasmaShell at /PlasmaShell: evaluateScript runs the script against `state`, which keeps
// what the script changed for the next call, as Plasma's containments would.
class FakeShell : public FakeService {
public:
    FakeShell(QString service, CallLog* log)
        : FakeService(std::move(service), QStringLiteral("/PlasmaShell"), QStringLiteral("shell"), log) {}

    PlasmaState state;
    QList<PlasmaRun> runs;

    // Every desktop's plugin now.
    QStringList plugins() const {
        QStringList out;
        for (const QJsonValue& d : state.desktops) {
            out.append(d.toObject().value(QStringLiteral("wallpaperPlugin")).toString());
        }
        return out;
    }

protected:
    void answer(const QDBusMessage& m) override {
        if (m.member() != u"evaluateScript" || m.interface() != u"org.kde.PlasmaShell") {
            m_bus.send(m.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"), QStringLiteral("no such method")));
            return;
        }
        const QString failed = QStringLiteral("org.freedesktop.DBus.Error.Failed");
        if (state.widgetsLocked) {
            m_bus.send(m.createErrorReply(failed, QStringLiteral("Widgets are locked")));
            return;
        }
        if (state.policyRefuses) {
            m_bus.send(m.createErrorReply(failed, QStringLiteral("Administrative policies prevent script execution")));
            return;
        }
        const PlasmaRun run = runPlasmaModel(state, m.arguments().value(0).toString());
        runs.append(run);
        QJsonArray after;
        for (qsizetype i = 0; i < state.desktops.size(); ++i) {
            QJsonObject d = state.desktops.at(i).toObject();
            d.insert(QStringLiteral("wallpaperPlugin"), run.plugins.value(i, d.value(QStringLiteral("wallpaperPlugin")).toString()));
            after.append(d);
        }
        state.desktops = after;
        m_bus.send(run.threw ? m.createErrorReply(failed, run.error) : m.createReply(run.printed));
    }
};

} // namespace wallpaperx::apptest
