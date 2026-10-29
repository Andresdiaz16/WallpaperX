// SPDX-License-Identifier: GPL-2.0-or-later

// Applying a wallpaper from the app: app/daemon/daemon_client.cpp, app/daemon/apply_controller.cpp
// and app/kde/plasma_controller.cpp, driven against stand-ins for the daemon and for plasmashell on
// the private bus (fake_wallpaperx.h), with a real KScreenClient reading a fake KScreen and a real
// LibraryManager over a scratch library. The plasmashell stand-in runs the activation script it
// receives against a model of Plasma 6.7.5's scripting API. Goals S15.226-S15.253, S15.256-S15.264
// and S15.911-S15.913 in GOALS.md.
// Built with AddressSanitizer, so a reply that reaches a destroyed caller aborts the run (S15.247).

#include "apply_controller.h"
#include "daemon_client.h"
#include "daemon_rules.h"
#include "fake_kscreen.h"
#include "fake_wallpaperx.h"
#include "kscreen_client.h"
#include "kscreen_fixtures.h"
#include "library_manager.h"
#include "plasma_controller.h"
#include "refresh_rate_reporter.h"
#include "wpx_fixtures.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>

using namespace wallpaperx;
using namespace wallpaperx::apptest;
using wallpaperx::testing::harnessFailure;
using wallpaperx::testing::notVerified;
using Progress = ApplyController::Progress;

namespace {

constexpr int kWaitMs = 5000;
const QString kWx = QStringLiteral("com.wallpaperx.wallpaper");
const QString kImage = QStringLiteral("org.kde.image");

QString joined(const QStringList& list) {
    return list.join(u',');
}

QString describeMap(const QVariantMap& map) {
    QStringList parts;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        parts.append(QStringLiteral("%1=%2(%3)").arg(it.key(), it.value().toString(), QLatin1StringView(it.value().typeName())));
    }
    return u'{' + parts.join(u", ") + u'}';
}

// `values` with the four position keys at their defaults, as the app sends them beside every
// wallpaper's own properties.
QVariantMap withPosition(QVariantMap values) {
    values.insert({{QStringLiteral("position"), QStringLiteral("cover")}, {QStringLiteral("position.zoom"), 1.0},
                   {QStringLiteral("position.x"), 0.0}, {QStringLiteral("position.y"), 0.0}});
    return values;
}

// What the daemon's own rules (daemon_rules.h) make of `values` as sent for a wallpaper of `type`:
// empty when it would show it. A value of a type other than double, bool or string is refused here,
// as the daemon refuses it on the wire.
std::string daemonVerdictOnSent(const QVariantMap& values, const char* type) {
    std::vector<std::pair<std::string, DaemonValue>> sent;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        switch (it.value().typeId()) {
        case QMetaType::Double: sent.emplace_back(it.key().toStdString(), it.value().toDouble()); break;
        case QMetaType::Bool: sent.emplace_back(it.key().toStdString(), it.value().toBool()); break;
        case QMetaType::QString: sent.emplace_back(it.key().toStdString(), it.value().toString().toStdString()); break;
        default: return "property " + it.key().toStdString() + " is a " + it.value().typeName() + ", which the daemon does not take";
        }
    }
    return daemonShowError(sent, type);
}

// Plasma's screens in an order unlike KScreen's listing (DP-1 first there), and desktops of four
// activities: A is current; B has a desktop saved on each screen; C saved nothing; D saved an empty
// lastScreen; E saved lastScreen as a number rather than as text.
PlasmaState twoScreenPlasma() {
    PlasmaState s;
    s.order = {QStringLiteral("HDMI-A-1"), QStringLiteral("DP-1")};
    s.desktops = QJsonArray{plasmaDesktop(0, 0, "A"),
                            plasmaDesktop(1, 1, "A"),
                            plasmaDesktop(-1, QStringLiteral("1"), "B"),
                            plasmaDesktop(-1, QStringLiteral("0"), "B"),
                            plasmaDesktop(-1, QJsonValue(), "C"),
                            plasmaDesktop(-1, QStringLiteral(""), "D"),
                            plasmaDesktop(-1, 1, "E")};
    return s;
}
// What twoScreenPlasma() holds after DP-1 (Plasma's screen 1), or HDMI-A-1 (screen 0), is switched.
const QStringList kAfterDp1{kImage, kWx, kWx, kImage, kImage, kImage, kWx};
const QStringList kAfterHdmi{kWx, kImage, kImage, kWx, kImage, kImage, kImage};

// Everything Apply needs, on service names of this rig's own.
struct Rig {
    CallLog log;
    std::unique_ptr<QTemporaryDir> scratch;
    std::unique_ptr<FakeKScreen> kscreenFake;
    std::unique_ptr<FakeDaemon> daemonFake;
    std::unique_ptr<FakeShell> shellFake;
    std::unique_ptr<KScreenClient> kscreen;
    std::unique_ptr<LibraryManager> library;
    std::unique_ptr<DaemonClient> client;
    std::unique_ptr<PlasmaController> plasma;
    std::unique_ptr<ApplyController> applier;
    QString ocean, river, glow;

    ~Rig() {
        applier.reset();
        plasma.reset();
        client.reset();
        library.reset();
        kscreen.reset();
        shellFake.reset();
        daemonFake.reset();
        kscreenFake.reset();
    }

    // Empty when ready, otherwise why the rig could not be built: a harness fault, never the app's.
    QString build(const QString& tag, DaemonTimeouts timeouts = {2000}, int plasmaTimeoutMs = 2000) {
        scratch = std::make_unique<QTemporaryDir>();
        if (!scratch->isValid()) {
            return QStringLiteral("no scratch folder");
        }
        const QString lib = scratch->path() + QStringLiteral("/wallpapers");
        ocean = lib + QStringLiteral("/ocean");
        river = lib + QStringLiteral("/river");
        glow = lib + QStringLiteral("/glow");
        // Ocean's video has sound and River's is silent, so a declared mute is offered for Ocean only.
        for (const auto& [path, json] : {std::pair{ocean, QByteArray(kPlanManifest)}, std::pair{river, titled("River")},
                                         std::pair{glow, shaderManifest()}}) {
            if (const QString why = writeFolder(path, planEntriesWith(json, path == ocean ? soundVideo() : silentVideo()));
                !why.isEmpty()) {
                return QStringLiteral("the library could not be written: ") + why;
            }
        }
        kscreenFake = std::make_unique<FakeKScreen>(QStringLiteral("io.wallpaperx.test.KScreen.") + tag);
        kscreenFake->config = configOf({output({.name = QStringLiteral("DP-1"), .id = 1, .priority = 1}),
                                        output({.name = QStringLiteral("HDMI-A-1"), .id = 2, .x = qlonglong(1920), .priority = 2})});
        // Held, so each screen's display name is its connector, which the sentences name.
        kscreenFake->edidMode = FakeKScreen::Mode::Hold;
        daemonFake = std::make_unique<FakeDaemon>(QStringLiteral("io.wallpaperx.test.Daemon.") + tag, &log);
        shellFake = std::make_unique<FakeShell>(QStringLiteral("io.wallpaperx.test.PlasmaShell.") + tag, &log);
        shellFake->state = twoScreenPlasma();
        for (const QString& why : {kscreenFake->start(), daemonFake->start(), shellFake->start()}) {
            if (!why.isEmpty()) {
                return why;
            }
        }
        kscreen = std::make_unique<KScreenClient>(dbusSource(kscreenFake->service()));
        library = std::make_unique<LibraryManager>(lib, 30);
        client = std::make_unique<DaemonClient>(QDBusConnection::sessionBus(), daemonFake->service(), timeouts);
        plasma = std::make_unique<PlasmaController>(QDBusConnection::sessionBus(), shellFake->service(), plasmaTimeoutMs);
        applier = std::make_unique<ApplyController>(*kscreen, *library, *client, *plasma);
        kscreen->refresh();
        library->rescan();
        client->refresh();
        if (!waitUntil([&] {
                return kscreen->state() == KScreenClient::State::Ready && library->state() == LibraryManager::State::Ready &&
                       library->wallpapers().size() == 3 && client->state() == DaemonClient::State::Ready;
            }, kWaitMs)) {
            return QStringLiteral("KScreen, the library (%1 listed) and the daemon client never all became ready")
                .arg(library->wallpapers().size());
        }
        if (!kscreen->select(QStringLiteral("DP-1")) || !library->select(ocean)) {
            return QStringLiteral("DP-1 and ocean could not be selected");
        }
        return {};
    }

    // Applies and waits for it to finish; false if it never did.
    bool apply() {
        applier->apply();
        return waitUntil([&] { return applier->applyState() != Progress::Working; }, kWaitMs);
    }

    // Every call the app queued to the daemon before this has reached the stand-in once this
    // returns true: the clients and this marker share the session bus connection, whose messages
    // reach the bus in the order they were sent. A marker of its own, not a GetStatus: a GetStatus
    // the client sent earlier on its own (the re-read after an edit) would end the wait too soon.
    bool daemonDrained() {
        static int markers = 0;
        const QString token = QString::number(++markers);
        QDBusMessage marker = QDBusMessage::createMethodCall(daemonFake->service(), QStringLiteral("/io/wallpaperx/Daemon"),
                                                             QStringLiteral("io.wallpaperx.test.Marker"), QStringLiteral("Marker"));
        marker << token;
        QDBusConnection::sessionBus().asyncCall(marker, kWaitMs);
        return waitUntil([&] {
            const auto seen = daemonFake->named(QStringLiteral("Marker"));
            return std::ranges::any_of(seen, [&](const FakeCall& c) { return c.args.value(0).toString() == token; });
        }, kWaitMs);
    }

    // Every reply the daemon stand-in sent before this has been handled by the app once this
    // returns true: the marker's reply comes back after them on the same connection, and its
    // watcher's finished signal is queued behind theirs on this thread.
    bool repliesHandled() {
        QDBusMessage marker = QDBusMessage::createMethodCall(daemonFake->service(), QStringLiteral("/io/wallpaperx/Daemon"),
                                                             QStringLiteral("io.wallpaperx.test.Marker"), QStringLiteral("Marker"));
        marker << QStringLiteral("replies");
        QDBusPendingCallWatcher watcher(QDBusConnection::sessionBus().asyncCall(marker, kWaitMs));
        bool done = false;
        QObject::connect(&watcher, &QDBusPendingCallWatcher::finished, [&] { done = true; });
        return waitUntil([&] { return done; }, kWaitMs) && !watcher.isError();
    }

    // The same for the shell, through a probe for a connector Plasma has no screen for, which
    // changes nothing; it is one of the evaluateScript calls the stand-in counts.
    bool shellDrained() {
        bool answered = false;
        QObject context;
        plasma->activate(&context, QStringLiteral("WALLPAPERX-PROBE"), [&](std::expected<int, QString>) { answered = true; });
        return waitUntil([&] { return answered; }, kWaitMs);
    }

    QList<FakeCall> sent(const char* member) const { return daemonFake->named(QString::fromLatin1(member)); }

    // Every call queued has reached the stand-in, and every reply owed has been handled, so a call made on one
    // has reached it too.
    bool settled() { return daemonDrained() && repliesHandled() && daemonDrained(); }

    // A GetStatus read that has landed, without waiting for replies a test is holding.
    bool readLanded() {
        bool read = false;
        const auto c = QObject::connect(client.get(), &DaemonClient::statusRead, [&] { read = true; });
        client->refresh();
        const bool ok = waitUntil([&] { return read; }, kWaitMs);
        QObject::disconnect(c);
        return ok && daemonDrained();
    }
    // A GetStatus read that has landed and been acted on.
    bool reread() { return readLanded() && settled(); }
};

// Builds a rig; on failure the phase's goals are reported not verified, naming why.
bool build(Rig& rig, const char* goals, const QString& tag, DaemonTimeouts timeouts = {2000}, int plasmaTimeoutMs = 2000) {
    const QString why = rig.build(tag, timeouts, plasmaTimeoutMs);
    if (!why.isEmpty()) {
        harnessFailure(std::string(goals) + ": the stand-ins could not be set up: " + why.toStdString());
        return false;
    }
    return true;
}

// The outcome of one PlasmaController::activate, or nothing when it never answered.
std::optional<std::expected<int, QString>> activate(PlasmaController& plasma, const QString& connector) {
    std::optional<std::expected<int, QString>> result;
    QObject context;
    plasma.activate(&context, connector, [&](std::expected<int, QString> r) { result = std::move(r); });
    if (!waitUntil([&] { return result.has_value(); }, kWaitMs)) {
        return std::nullopt;
    }
    return result;
}

QString outcome(const std::optional<std::expected<int, QString>>& r) {
    if (!r) {
        return QStringLiteral("(no answer)");
    }
    return *r ? QStringLiteral("switched %1").arg(**r) : QStringLiteral("refused: ") + r->error();
}

// ---- the activation script, without a bus --------------------------------------------------

void activationScriptAlone() {
    // GOAL: S15.229 -- a connector name reaches Plasma's screenForConnector exactly as given, whatever
    //       it holds: quotes, backslashes, a line or paragraph separator, a newline, a bidirectional
    //       override, non-ASCII; and none can run code of its own.
    // WHY-THIS-SHAPE: the script is run, not searched: a substring test of the literal passes an
    //       escaping that a JavaScript parser reads differently (U+2028 ends a line in old engines,
    //       `\u` needs four digits). The injected name sets a global only if it escapes the string.
    // CONTROL: app/kde/plasma_controller.cpp, jsString() returning `"\"" + text + "\""` (control_batch,
    //       2026-10-02) -> red, "S15.229: the connector "DP-1"); this.pwned = 1; ("" reached
    //       screenForConnector as [] (threw 1, injected 0): Error: SyntaxError: Unexpected token `)'
    //       at line 2", and the same for the quote, separator and newline names.
    const QStringList names{QStringLiteral("DP-1\"); this.pwned = 1; (\""), QStringLiteral("a\\\"b\\"),
                            QStringLiteral("x%1y%2z").arg(QChar(0x2028), QChar(0x2029)), QStringLiteral("x\ny\r"),
                            QString(QChar(0x202e)) + QStringLiteral("DP-1"), QString(QChar(0xc9)) + QStringLiteral("cran-1")};
    for (const QString& name : names) {
        PlasmaState state = twoScreenPlasma();
        state.order.append(name);
        state.desktops.append(plasmaDesktop(2, 2, "A"));
        const PlasmaRun run = runPlasmaModel(state, activationScript(name));
        expect(run.asked == QStringList{name} && !run.injected && !run.threw,
               QStringLiteral("S15.229: the connector \"%1\" reached screenForConnector as [%2] (threw %3, injected %4): %5")
                   .arg(name, run.asked.join(u'|')).arg(run.threw).arg(run.injected).arg(run.error.section(u'\n', 0, 0)));
    }

    // GOAL: S15.232 -- the plugin the script requires is listed under the display name the plugin's
    //       own metadata.json gives (KPlugin.Name, what knownWallpaperPlugins() lists), and the plugin
    //       it switches to is that file's KPlugin.Id; renaming either side breaks activation here.
    // WHY-THIS-SHAPE: read from backend/kde/kpackage/metadata.json at run time and handed to the
    //       model as the only installed plugin besides Image, so a copy of the name in this file
    //       cannot drift along with the controller's.
    // CONTROL: control_batch, 2026-10-02: backend/kde/kpackage/metadata.json `"Name": "WallpaperX"`
    //       made `"WallpaperX Video"` -> red, "S15.232: with only "WallpaperX Video" installed the
    //       script threw: ...The WallpaperX wallpaper plugin is not installed for Plasma..."; app/kde/
    //       plasma_controller.cpp kWallpaperPluginName made "Wallpaper X" -> red, "S15.232: with only
    //       "WallpaperX" installed the script threw: ..."; app/kde/plasma_controller.h
    //       kWallpaperPluginId made "com.wallpaperx.wp" -> red, "... the script ran and switched to
    //       com.wallpaperx.wp,com.wallpaperx.wp,com.wallpaperx.wp, not com.wallpaperx.wallpaper".
    QFile metadataFile(QStringLiteral(WALLPAPERX_PLUGIN_METADATA));
    const QJsonObject plugin = metadataFile.open(QIODevice::ReadOnly)
                                   ? QJsonDocument::fromJson(metadataFile.readAll()).object().value(QStringLiteral("KPlugin")).toObject()
                                   : QJsonObject();
    const QString name = plugin.value(QStringLiteral("Name")).toString();
    const QString id = plugin.value(QStringLiteral("Id")).toString();
    if (name.isEmpty() || id.isEmpty()) {
        harnessFailure("S15.232: KPlugin.Name and KPlugin.Id could not be read from " WALLPAPERX_PLUGIN_METADATA);
    } else {
        PlasmaState state = twoScreenPlasma();
        state.plugins = {QStringLiteral("Image"), name};
        const PlasmaRun run = runPlasmaModel(state, activationScript(QStringLiteral("DP-1")));
        QStringList switchedTo;
        for (const QString& p : run.plugins) {
            if (p != kImage) {
                switchedTo.append(p);
            }
        }
        expect(!run.threw && switchedTo.size() == 3 && switchedTo.count(id) == 3,
               QStringLiteral("S15.232: with only \"%1\" installed the script %2 and switched to %3, not %4")
                   .arg(name, run.threw ? QStringLiteral("threw: ") + run.error.section(u'\n', 0, 0) : QStringLiteral("ran"),
                        joined(switchedTo), id));
    }

    // GOAL: S15.230 -- evaluateScript answering without "wallpaperx-switched=N", N >= 1, is a
    //       failure: a shell that runs nothing is never reported as done.
    // CONTROL: app/kde/plasma_controller.cpp, activationResult()'s `found.hasMatch() ? ... : 0` made
    //       `: 1` (control_batch, 2026-10-02) -> red, "S15.230: the reply "" read as switched 1", and
    //       the same for "done" and "wallpaperx-switched=".
    const QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("x.y"), QStringLiteral("/"), QStringLiteral("x.y"), QStringLiteral("m"));
    struct Reply {
        QString printed;
        int want; // 0: a failure
    };
    const Reply replies[] = {{QString(), 0}, {QStringLiteral("done"), 0}, {QStringLiteral("wallpaperx-switched=0"), 0},
                             {QStringLiteral("wallpaperx-switched="), 0}, {QStringLiteral("wallpaperx-switched=2"), 2}};
    for (const Reply& r : replies) {
        const auto read = activationResult(call.createReply(r.printed));
        const bool right = r.want == 0 ? (!read && read.error() == u"Plasma ran the script that switches the desktop but did not say it had.")
                                       : (read && *read == r.want);
        expect(right, QStringLiteral("S15.230: the reply \"%1\" read as %2").arg(r.printed, read ? QStringLiteral("switched %1").arg(*read) : read.error()));
    }
}

// ---- PlasmaController against the stand-in shell -------------------------------------------

void plasmaShell() {
    constexpr const char* goals = "S15.226-S15.228, S15.231";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("shell"), {2000}, 1500)) {
        return;
    }
    FakeShell& shell = *rig.shellFake;

    // GOAL: S15.226, S15.227 -- the desktops switched are exactly those at the index plasmashell
    //       itself gives the connector (screenForConnector, asked inside the shell), on every
    //       activity: the current one's by `screen`, the others' by their saved lastScreen, as text
    //       or as a number; one with no lastScreen or an empty one is not touched.
    // WHY-THIS-SHAPE: Plasma's order (HDMI-A-1, DP-1) is the reverse of KScreen's listing, and both
    //       connectors are switched, so neither a fixed index nor KScreen's position matches both.
    //       The whole after-state is compared, so a desktop switched by mistake is caught as well as
    //       one missed. An empty lastScreen is tested against screen 0, where Number("") would land.
    // CONTROL: app/kde/plasma_controller.cpp, each through control_batch, 2026-10-02:
    //       `screenForConnector(connector)` made `0` -> red, "S15.226, S15.227: DP-1 (switched 2)
    //       switched [com.wallpaperx.wallpaper,org.kde.image,org.kde.image,com.wallpaperx.wallpaper,...]";
    //       made `1` -> red, "... HDMI-A-1 (switched 3) switched [org.kde.image,com.wallpaperx.wallpaper,...]";
    //       `var on = d.screen >= 0 ? ... : parseInt(...)` made `var on = d.screen;` -> red, "... DP-1
    //       (switched 1)"; `parseInt(d.readConfig("lastScreen", -1), 10)` made
    //       `Number(d.readConfig("lastScreen", -1))` -> red, "... HDMI-A-1 (switched 3) switched
    //       [...,com.wallpaperx.wallpaper,org.kde.image]" (activity D's empty lastScreen switched).
    const auto dp1 = activate(*rig.plasma, QStringLiteral("DP-1"));
    expect(dp1 && *dp1 && **dp1 == 3 && shell.plugins() == kAfterDp1 && shell.runs.size() == 1 && shell.runs.back().asked == QStringList{QStringLiteral("DP-1")},
           QStringLiteral("S15.226, S15.227: DP-1 (%1) switched [%2], not [%3]; screenForConnector was asked [%4]")
               .arg(outcome(dp1), joined(shell.plugins()), joined(kAfterDp1), shell.runs.isEmpty() ? QString() : joined(shell.runs.back().asked)));
    shell.state = twoScreenPlasma();
    const auto hdmi = activate(*rig.plasma, QStringLiteral("HDMI-A-1"));
    expect(hdmi && *hdmi && **hdmi == 2 && shell.plugins() == kAfterHdmi,
           QStringLiteral("S15.226, S15.227: HDMI-A-1 (%1) switched [%2], not [%3]").arg(outcome(hdmi), joined(shell.plugins()), joined(kAfterHdmi)));

    // GOAL: S15.228 -- a connector Plasma shows no desktop of its own on (a mirrored screen), and a
    //       screen Plasma has made no desktop for yet, each fail with their own sentence and switch
    //       nothing.
    // CONTROL: app/kde/plasma_controller.cpp, control_batch, 2026-10-02: the `|| screen < 0` refusal
    //       deleted -> red, "S15.228: DP-2 gave switched 1 and left [...]" (index -1 matched activity
    //       C's desktop, whose missing lastScreen reads as -1); the `switched === 0` refusal made
    //       `if (false)` -> red, "S15.228: DP-3 gave refused: Plasma ran the script that switches the
    //       desktop but did not say it had."
    struct NoDesktop {
        const char* connector;
        const char* sentence;
    };
    const NoDesktop none[] = {{"DP-2", "Plasma shows no desktop of its own on DP-2."}, {"DP-3", "Plasma has no desktop on DP-3 yet."}};
    for (const NoDesktop& c : none) {
        shell.state = twoScreenPlasma();
        shell.state.order.append(QStringLiteral("DP-3"));
        const QStringList before = shell.plugins();
        const auto r = activate(*rig.plasma, QString::fromLatin1(c.connector));
        expect(r && !*r && r->error() == QLatin1StringView(c.sentence) && shell.plugins() == before,
               QStringLiteral("S15.228: %1 gave %2 and left [%3]; wanted \"%4\" and nothing switched")
                   .arg(QLatin1StringView(c.connector), outcome(r), joined(shell.plugins()), QLatin1StringView(c.sentence)));
    }

    // GOAL: S15.231 -- each way Plasma can fail to switch has its own sentence: no plasmashell, no
    //       answer, locked widgets, a policy refusing scripts, the plugin not installed, a Plasma
    //       without screenForConnector, and a reply that does not confirm the switch. The script's
    //       own refusals read as the sentence alone, without Plasma's "Error: ... at line N" wrapping.
    // WHY-THIS-SHAPE: whole sentences are compared, so a refusal shown through the generic
    //       "could not run the script" branch, or with the wrapping left in, goes red.
    // CONTROL: app/kde/plasma_controller.cpp, control_batch, 2026-10-02: scriptFailure()'s marker
    //       extraction made `if (false)` -> red, "S15.231: plugin not installed gave refused: Plasma
    //       could not run the script that switches the desktop: Error: Error: [wallpaperx]The
    //       WallpaperX wallpaper plugin is not installed for Plasma[/wallpaperx] at line 3, ..." and
    //       the no-screenForConnector row; describe() reduced to `return scriptFailure(error.message());`
    //       -> red, "S15.231: no plasmashell gave refused: Plasma could not run the script that
    //       switches the desktop: The name ... was not provided by any .service files", and the
    //       not-a-Plasma-shell and no-answer rows.
    // COVERAGE-ASSERT: guards the table's length only. The loop has no exit, so the count fails just
    //       when a row is deleted without the 8 below changing with it. Controlled test-side
    //       (control_batch, 2026-10-04): the "policy" row deleted -> red, "S15.231: 7 of 8 failures
    //       were run".
    struct Failure {
        const char* what;
        std::function<void()> arrange;
        const char* sentence;
    };
    const Failure failures[] = {
        {"plugin not installed", [&] { shell.state.plugins = {QStringLiteral("Image")}; },
         "The WallpaperX wallpaper plugin is not installed for Plasma."},
        {"no screenForConnector", [&] { shell.state.hasScreenForConnector = false; },
         "This Plasma cannot find a screen by its connector name."},
        {"widgets locked", [&] { shell.state.widgetsLocked = true; },
         "Plasma could not run the script that switches the desktop: Widgets are locked"},
        {"policy", [&] { shell.state.policyRefuses = true; },
         "Plasma could not run the script that switches the desktop: Administrative policies prevent script execution"},
        {"unconfirmed", [&] { shell.state.printReaches = false; },
         "Plasma ran the script that switches the desktop but did not say it had."},
        {"not a Plasma shell", [&] { shell.refuse.insert(QStringLiteral("evaluateScript"), {QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"), QStringLiteral("no")}); },
         "The desktop shell does not take Plasma's desktop scripts."},
        {"no answer", [&] { shell.hold.insert(QStringLiteral("evaluateScript")); }, "Plasma's desktop shell did not answer."},
        {"no plasmashell", [&] { shell.stop(); }, "Plasma's desktop shell (plasmashell) is not running."},
    };
    int rows = 0;
    for (const Failure& f : failures) {
        shell.state = twoScreenPlasma();
        shell.refuse.clear();
        shell.hold.clear();
        f.arrange();
        const auto r = activate(*rig.plasma, QStringLiteral("DP-1"));
        ++rows;
        expect(r && !*r && r->error() == QLatin1StringView(f.sentence),
               QStringLiteral("S15.231: %1 gave %2, not \"%3\"").arg(QLatin1StringView(f.what), outcome(r), QLatin1StringView(f.sentence)));
        shell.release();
    }
    expect(rows == 8, QStringLiteral("S15.231: %1 of 8 failures were run").arg(rows));
}

// ---- Apply ---------------------------------------------------------------------------------

void applyOrder() {
    constexpr const char* goals = "S15.233, S15.234, S15.243";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("order"))) {
        return;
    }

    // GOAL: S15.243 -- a shader wallpaper is listed usable, Apply is blocked with the reason, and
    //       apply() sends nothing to either service.
    // WHY-THIS-SHAPE: what apply() sent is the log less the two drain markers; read after both, so
    //       a first call it queued to either stand-in is in the log.
    // CONTROL: app/daemon/apply_controller.cpp, blockedReason()'s shader branch made `if (false)`
    //       (control_batch, 2026-10-02) -> red, "S15.243: the shader is usable 1, blocked for "", and
    //       apply() sent [daemon:GetStatus,daemon:SetMonitorOutput]" (as far as it had got by then).
    if (!rig.library->select(rig.glow)) {
        expect(false, QStringLiteral("S15.243: the shader wallpaper could not be opened"));
    } else {
        rig.log.clear();
        rig.applier->apply();
        const bool drained = rig.daemonDrained() && rig.shellDrained();
        QStringList sentByApply = rig.log;
        sentByApply.removeAll(QStringLiteral("daemon:Marker"));
        if (const qsizetype probe = sentByApply.lastIndexOf(QStringLiteral("shell:evaluateScript")); probe >= 0) {
            sentByApply.removeAt(probe);
        }
        expect(drained && rig.library->selectedWallpaper().usable() &&
                   rig.applier->blockedReason() == u"WallpaperX cannot draw shader wallpapers yet, so this one cannot be applied." &&
                   sentByApply.isEmpty() && rig.applier->applyState() == Progress::Idle,
               QStringLiteral("S15.243: the shader is usable %1, blocked for \"%2\", and apply() sent [%3]")
                   .arg(rig.library->selectedWallpaper().usable()).arg(rig.applier->blockedReason(), joined(sentByApply)));
    }

    // GOAL: S15.233 -- Apply runs bind, then SetWallpaperWithProperties, then evaluateScript: the
    //       screen named as monitor 0, the wallpaper sent as (type, asset path, id, values) with the
    //       edit made before Apply and each value typed double, bool or "#rrggbb" string, then the
    //       desktops at Plasma's DP-1 index switched, and the result reported Done.
    // WHY-THIS-SHAPE: one log across both stand-ins, so an evaluateScript sent before the daemon
    //       accepted is out of order even though both arrive.
    // CONTROL: app/daemon/apply_controller.cpp, a `m_plasma.activate(...)` added before
    //       `m_daemon.setWallpaper(...)` (Plasma asked first; control_batch, 2026-10-02) -> red,
    //       "S15.233: Apply sent [GetStatus,SetMonitorOutput,evaluateScript,SetWallpaperWithProperties,
    //       evaluateScript] and ended 2: Ocean Waves is on DP-1.". The values line: the added volume's default
    //       made 0.5 in settleSoundProperties (app/library/wpx_package.cpp; control_batch, re-run 2026-10-18) ->
    //       red, "S15.233: SetWallpaperWithProperties carried ssia{sv} .../ocean/assets/video.mp4 0 {mute=true(bool),
    //       position=cover(QString), ..., volume=0.5(double)}".
    rig.library->select(rig.ocean);
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    rig.log.clear();
    const bool finished = rig.apply();
    QStringList order;
    for (const QString& entry : rig.log) {
        const QString member = entry.section(u':', 1);
        if (member != u"GetStatus" || order.isEmpty()) {
            order.append(member);
        }
    }
    const QStringList wantOrder{QStringLiteral("GetStatus"), QStringLiteral("SetMonitorOutput"), QStringLiteral("SetWallpaperWithProperties"),
                                QStringLiteral("evaluateScript")};
    expect(finished && rig.applier->applyState() == Progress::Done && rig.applier->applyMessage() == u"Ocean Waves is on DP-1." &&
               order == wantOrder,
           QStringLiteral("S15.233: Apply sent [%1] and ended %2: %3").arg(joined(order)).arg(int(rig.applier->applyState())).arg(rig.applier->applyMessage()));
    const auto bind = rig.sent("SetMonitorOutput");
    const auto set = rig.sent("SetWallpaperWithProperties");
    expect(bind.size() == 1 && bind[0].signature == u"is" && bind[0].args == QVariantList{0, QStringLiteral("DP-1")},
           QStringLiteral("S15.233: the screen was bound as %1").arg(bind.isEmpty() ? QStringLiteral("(nothing)") : bind[0].signature + u' ' + bind[0].args.value(0).toString() + u' ' + bind[0].args.value(1).toString()));
    const QVariantMap wantValues = withPosition({{QStringLiteral("speed"), 2.0}, {QStringLiteral("mute"), true},
                                                 {QStringLiteral("tint"), QStringLiteral("#ffffff")}, {QStringLiteral("volume"), 1.0}});
    const QVariantMap values = set.isEmpty() ? QVariantMap() : set[0].args.value(3).toMap();
    // Compared as text naming each type: QVariant's == takes an int 1 for the double 1.0.
    expect(set.size() == 1 && set[0].signature == u"ssia{sv}" && set[0].args.value(0).toString() == u"video" &&
               set[0].args.value(1) == QString(rig.ocean + QStringLiteral("/assets/video.mp4")) && set[0].args.value(2) == 0 &&
               describeMap(values) == describeMap(wantValues),
           QStringLiteral("S15.233: SetWallpaperWithProperties carried %1 %2 %3 %4")
               .arg(set.isEmpty() ? QStringLiteral("(nothing)") : set[0].signature, set.isEmpty() ? QString() : set[0].args.value(1).toString(),
                    set.isEmpty() ? QString() : set[0].args.value(2).toString(), describeMap(values)));
    expect(rig.shellFake->plugins() == kAfterDp1,
           QStringLiteral("S15.233: after Apply on DP-1 the desktops are [%1], not [%2]").arg(joined(rig.shellFake->plugins()), joined(kAfterDp1)));

    // GOAL: S15.473's Apply half -- a silent video's declared mute is not sent: River, whose
    //       wallpaper.json declares mute over a video with no sound, is applied with speed and tint alone.
    // CONTROL: via control_batch.sh, --phase '^applyOrder$', 2026-10-11, settleSoundProperties' `==
    //       AudioStreams::Absent` strip made `if (false)` (app/library/wpx_package.cpp) -> "FAIL: S15.473: a silent
    //       video was applied with {mute=true(bool), position=cover(QString), ..., volume=1(double)} (2 call(s))"
    //       (re-run 2026-10-18). Reverted.
    rig.library->select(rig.river);
    const bool riverFinished = rig.apply();
    const auto riverSet = rig.sent("SetWallpaperWithProperties");
    const QVariantMap riverValues = riverSet.size() == 2 ? riverSet[1].args.value(3).toMap() : QVariantMap();
    expect(riverFinished && riverSet.size() == 2 &&
               describeMap(riverValues) == describeMap(withPosition({{QStringLiteral("speed"), 1.0}, {QStringLiteral("tint"), QStringLiteral("#ffffff")}})),
           QStringLiteral("S15.473: a silent video was applied with %1 (%2 call(s))").arg(describeMap(riverValues)).arg(riverSet.size()));
    rig.library->select(rig.ocean);

    // GOAL: S15.233 -- a wallpaper the daemon refuses is reported Failed with its reason, and Plasma
    //       is never asked to switch to it.
    // CONTROL: app/daemon/apply_controller.cpp, `m_plasma.activate(this, connector, ...)` added at the
    //       top of setWallpaper's failure branch (control_batch, 2026-10-02) -> red, "S15.233: after
    //       the daemon refused (...), Plasma was asked 1 more time(s)".
    rig.daemonFake->refuse.insert(QStringLiteral("SetWallpaperWithProperties"),
                                  {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("speed must be in (0, 2]")});
    const qsizetype scriptsBefore = rig.shellFake->calls.size();
    const bool refusedFinished = rig.apply();
    const bool drained = rig.shellDrained();
    expect(refusedFinished && drained && rig.applier->applyState() == Progress::Failed && rig.shellFake->calls.size() == scriptsBefore + 1,
           QStringLiteral("S15.233: after the daemon refused (%1), Plasma was asked %2 more time(s)")
               .arg(rig.applier->applyMessage()).arg(rig.shellFake->calls.size() - scriptsBefore - 1));
    rig.daemonFake->refuse.clear();

    // GOAL: S15.234 -- a screen the daemon already knows keeps its id: no SetMonitorOutput, and the
    //       wallpaper goes to that id.
    // CONTROL: app/daemon/daemon_client.cpp, bindNext()'s known-screen test made `false`
    //       (control_batch, 2026-10-02) -> red, "S15.234: HDMI-A-1, known as 7, was bound 1 time(s)
    //       and set on [2]".
    rig.daemonFake->outputs.insert(QStringLiteral("HDMI-A-1"), 7);
    rig.daemonFake->calls.clear();
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    const bool knownFinished = rig.apply();
    QStringList setOn;
    for (const FakeCall& c : rig.sent("SetWallpaperWithProperties")) {
        setOn.append(c.args.value(2).toString());
    }
    expect(knownFinished && rig.applier->applyState() == Progress::Done && rig.sent("SetMonitorOutput").isEmpty() && setOn == QStringList{QStringLiteral("7")},
           QStringLiteral("S15.234: HDMI-A-1, known as 7, was bound %1 time(s) and set on [%2] (%3)")
               .arg(rig.sent("SetMonitorOutput").size()).arg(joined(setOn), rig.applier->applyMessage()));
}

void freeIds() {
    // GOAL: S15.235 -- the id bound to a new screen is the lowest one that holds no name, no
    //       wallpaper and no pause setting; with all 32 in use there is none.
    // CONTROL: app/daemon/daemon_client.cpp, freeMonitorId()'s `QSet<int> taken = status.occupied;`
    //       made `QSet<int> taken;` (control_batch, 2026-10-02) -> red, "S15.235: with 0 named, 1
    //       showing and 2 paused the free id is 1" and "with all 32 ids in use one was still offered".
    const QByteArray json = R"({"outputs_literal": {"DP-1": 0}, "wallpapers": {"1": {"type": "video"}},
                                "pause": {"2": {"mode": "play", "paused": false}}, "persistence": {"status": "ok"}})";
    const auto status = parseDaemonStatus(json);
    const auto id = status ? freeMonitorId(*status) : std::nullopt;
    expect(status && id == 3, QStringLiteral("S15.235: with 0 named, 1 showing and 2 paused the free id is %1 (%2)")
                                  .arg(id ? QString::number(*id) : QStringLiteral("none"), status ? QString() : status.error()));
    DaemonStatus full;
    for (int i = 0; i < kMonitorIds; ++i) {
        if (i % 2) {
            full.occupied.insert(i);
        } else {
            full.outputs.insert(QStringLiteral("X-%1").arg(i), i);
        }
    }
    expect(!freeMonitorId(full), QStringLiteral("S15.235: with all %1 ids in use one was still offered").arg(kMonitorIds));

    // GOAL: S15.475 -- a monitor GetStatus lists only for a sound setting is in use: a new screen is
    //       not given its id, which would hand that screen the setting.
    // CONTROL: via control_batch.sh, --phase '^freeIds$', 2026-10-11, `status.occupied.insert(*id);` deleted
    //       from parseDaemonStatus's sound loop (app/daemon/daemon_client.cpp) -> "FAIL: S15.475: with 0 named,
    //       1 showing and 2 holding a sound setting the free id is 2". Reverted.
    const QByteArray soundOnly = R"({"outputs_literal": {"DP-1": 0}, "wallpapers": {"1": {"type": "video"}},
                                     "sound": {"enabled": true, "monitors": {"2": {"enabled": false, "heard": false}}},
                                     "persistence": {"status": "ok"}})";
    const auto withSound = parseDaemonStatus(soundOnly);
    const auto soundId = withSound ? freeMonitorId(*withSound) : std::nullopt;
    expect(withSound && soundId == 3, QStringLiteral("S15.475: with 0 named, 1 showing and 2 holding a sound setting the free id is %1 (%2)")
                                          .arg(soundId ? QString::number(*soundId) : QStringLiteral("none"), withSound ? QString() : withSound.error()));
}

void soundStatus() {
    // GOAL: S15.476 -- GetStatus's `sound` is read as the daemon applies it: false is off; true, null (a
    //       value a newer build wrote) and an older daemon's missing `sound` are on, for every screen and
    //       per monitor; a `sound` that is not an object, a `monitors` that is not one, or a monitor id
    //       out of range refuses the whole status.
    // CONTROL: via control_batch.sh, --phase '^soundStatus$', 2026-10-11, each alone, reverted, in
    //       app/daemon/daemon_client.cpp unless named: the global `toBool(true)` made `toBool(false)` -> "FAIL:
    //       S15.476: values a newer build wrote read as sound 0, monitor 2 1"; the monitor's likewise -> "...
    //       sound 1, monitor 2 0"; `status.sound = true` whatever is read -> "... sound off, monitor 2 off read
    //       as sound 1 ..."; DaemonStatus's `bool sound = true;` made false (app/daemon/daemon_client.h) -> "...
    //       an older daemon read as sound 0 ..."; each of the three refusals deleted (the id one made
    //       `continue`) -> its row's "was accepted"; the monitor's `enabled` read from `heard` instead ->
    //       "FAIL: S15.476: sound on, monitor 2 on read as sound 1, monitor 2 0" and the newer-build row.
    struct Row {
        const char* what;
        QByteArray json;
        bool parses;
        bool sound;
        std::optional<bool> monitorTwo;
    };
    const auto with = [](const char* sound) {
        return QByteArray(R"({"outputs_literal": {}, "persistence": {"status": "ok"}, "sound": )") + QByteArray(sound) + QByteArray("}");
    };
    const Row rows[] = {
        {"sound off, monitor 2 off", with(R"({"enabled": false, "monitors": {"2": {"enabled": false, "heard": false}}})"), true, false, false},
        // As the daemon lists a monitor that holds a wallpaper and is not heard: `heard` is not the setting.
        {"sound on, monitor 2 on", with(R"({"enabled": true, "monitors": {"2": {"enabled": true, "heard": false}}})"), true, true, true},
        {"values a newer build wrote", with(R"({"enabled": null, "monitors": {"2": {"enabled": null, "heard": false}}})"), true, true, true},
        {"an older daemon", QByteArray(R"({"outputs_literal": {}, "persistence": {"status": "ok"}})"), true, true, std::nullopt},
        {"a sound that is not an object", with("false"), false, true, std::nullopt},
        {"monitors that are not an object", with(R"({"enabled": true, "monitors": [2]})"), false, true, std::nullopt},
        {"a monitor id out of range", with(R"({"enabled": true, "monitors": {"32": {"enabled": false}}})"), false, true, std::nullopt},
    };
    for (const Row& row : rows) {
        const auto status = parseDaemonStatus(row.json);
        if (!row.parses) {
            expect(!status, QStringLiteral("S15.476: %1 was accepted").arg(QLatin1StringView(row.what)));
            continue;
        }
        const std::optional<bool> two = status && status->screenSound.contains(2) ? std::optional<bool>(status->screenSound.value(2)) : std::nullopt;
        expect(status && status->sound == row.sound && two == row.monitorTwo,
               QStringLiteral("S15.476: %1 read as sound %2, monitor 2 %3 (%4)")
                   .arg(QLatin1StringView(row.what))
                   .arg(status && status->sound)
                   .arg(two ? QString::number(*two) : QStringLiteral("none"), status ? QString() : status.error()));
    }
}

void soundSetters() {
    constexpr const char* goals = "S15.477-S15.480";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("sound"))) {
        return;
    }
    // GOAL: S15.477 -- setSound(false) sends SetSound(false) typed `b`, ends Done, and the client then
    //       reports sound off as the daemon does; setSound(true) the same for on.
    // CONTROL: via control_batch.sh, --phase '^soundSetters$', 2026-10-11, DaemonClient::setSound sending `!on`
    //       (app/daemon/daemon_client.cpp) -> "FAIL: S15.477: setSound(0) sent b true and the client reads 0,
    //       the daemon 1" and the same for setSound(1). Reverted.
    for (const bool on : {false, true}) {
        rig.applier->setSound(on);
        const bool done = waitUntil([&] { return rig.applier->soundState() != Progress::Working; }, kWaitMs);
        const auto sent = rig.sent("SetSound");
        const bool reread = rig.daemonDrained() && waitUntil([&] { return rig.client->sound() == on; }, kWaitMs);
        expect(done && reread && rig.applier->soundState() == Progress::Done && !sent.isEmpty() && sent.back().signature == u"b" &&
                   sent.back().args == QVariantList{on} && rig.daemonFake->sound == QJsonValue(on),
               QStringLiteral("S15.477: setSound(%1) sent %2 and the client reads %3, the daemon %4")
                   .arg(on)
                   .arg(sent.isEmpty() ? QStringLiteral("(nothing)") : sent.back().signature + u' ' + sent.back().args.value(0).toString())
                   .arg(rig.client->sound())
                   .arg(rig.daemonFake->sound.toBool()));
    }

    // GOAL: S15.478 -- setScreenSound(false) with DP-1 selected binds it and sends SetMonitorSound(its
    //       id, false) typed `ib`, ends Done, and the client then reports DP-1 off and HDMI-A-1 on.
    // WHY-THIS-SHAPE: HDMI-A-1 holds a wallpaper on an id of its own, so GetStatus lists it as
    //       {enabled: true}, as the daemon lists every monitor holding a wallpaper; unlisted, its "on"
    //       would come from the client's default for a missing entry and not from reading one.
    // CONTROL: via control_batch.sh, --phase '^soundSetters$', 2026-10-11, ApplyController::setScreenSound
    //       sending `!on` (app/daemon/apply_controller.cpp) -> "FAIL: S15.478: setScreenSound(false) on DP-1 (id
    //       0) sent ib and the client reads DP-1 true, HDMI-A-1 true", and S15.479's line; the client reading a
    //       listed monitor's `heard` in place of `enabled` (DaemonClient's status parse,
    //       app/daemon/daemon_client.cpp) -> "... reads DP-1 false, HDMI-A-1 false", alone. Reverted.
    constexpr int kHdmiId = 7;
    rig.daemonFake->outputs.insert(QStringLiteral("HDMI-A-1"), kHdmiId);
    rig.daemonFake->wallpapers.insert(kHdmiId);
    rig.applier->setScreenSound(false);
    const bool screenDone = waitUntil([&] { return rig.applier->screenSoundState() != Progress::Working; }, kWaitMs);
    const auto screenSent = rig.sent("SetMonitorSound");
    const int dp1 = rig.daemonFake->outputs.value(QStringLiteral("DP-1"), -1);
    const bool screenRead = rig.daemonDrained() &&
                            waitUntil([&] { return rig.client->screenSound().value(QStringLiteral("DP-1")) == QVariant(false); }, kWaitMs);
    expect(screenDone && screenRead && dp1 >= 0 && rig.applier->screenSoundState() == Progress::Done && screenSent.size() == 1 &&
               screenSent[0].signature == u"ib" && screenSent[0].args == QVariantList{dp1, false} &&
               dp1 != kHdmiId && rig.client->screenSound().value(QStringLiteral("HDMI-A-1")) == QVariant(true),
           QStringLiteral("S15.478: setScreenSound(false) on DP-1 (id %1) sent %2 and the client reads DP-1 %3, HDMI-A-1 %4")
               .arg(dp1)
               .arg(screenSent.isEmpty() ? QStringLiteral("(nothing)") : screenSent[0].signature)
               .arg(rig.client->screenSound().value(QStringLiteral("DP-1")).toString(),
                    rig.client->screenSound().value(QStringLiteral("HDMI-A-1")).toString()));

    // GOAL: S15.479 -- once the daemon has answered a sound change, the client reports the new value
    //       before its status read comes back: with GetStatus held, sound and DP-1's sound already read
    //       as set.
    // WHY-THIS-SHAPE: the two setters are held first, so the screen's bind, which may read the status,
    //       is over before GetStatus is held too; then they are answered. The value read can only come
    //       from the replies, which are known handled once the controller is Done.
    // CONTROL: via control_batch.sh, --phase '^soundSetters$', 2026-10-11, `record();` deleted from
    //       DaemonClient::sendSetting (app/daemon/daemon_client.cpp) -> "FAIL: S15.479: with the status read
    //       held (1), the client read sound 0 and DP-1 false once the daemon answered", alone. Reverted.
    rig.daemonFake->hold = {QStringLiteral("SetSound"), QStringLiteral("SetMonitorSound")};
    rig.applier->setSound(false);
    rig.applier->setScreenSound(true);
    const bool settersHeld = waitUntil([&] { return rig.daemonFake->held.size() == 2; }, kWaitMs);
    rig.daemonFake->hold = {QStringLiteral("GetStatus")};
    rig.daemonFake->release();
    const bool globalEarly = waitUntil([&] { return rig.applier->soundState() == Progress::Done; }, kWaitMs) && !rig.client->sound();
    const bool screenEarly = waitUntil([&] { return rig.applier->screenSoundState() == Progress::Done; }, kWaitMs) &&
                             rig.client->screenSound().value(QStringLiteral("DP-1")) == QVariant(true);
    const bool statusHeld = settersHeld && waitUntil([&] {
        return std::ranges::any_of(rig.daemonFake->held, [](const QDBusMessage& m) { return m.member() == u"GetStatus"; });
    }, kWaitMs);
    expect(statusHeld && globalEarly && screenEarly,
           QStringLiteral("S15.479: with the status read held (%1), the client read sound %2 and DP-1 %3 once the daemon answered")
               .arg(statusHeld).arg(rig.client->sound()).arg(rig.client->screenSound().value(QStringLiteral("DP-1")).toString()));
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();

    // GOAL: S15.480 -- a sound change still running shows its value: for every screen whichever is
    //       selected, for one screen only while that screen is selected; a second change while one runs
    //       is ignored; a refused change says which setting, and the daemon's reason.
    // CONTROL: via control_batch.sh, --phase '^soundSetters$', 2026-10-11, each alone, reverted, in
    //       app/daemon/apply_controller.cpp unless named: forSelected() made `return true` -> the pending line
    //       ("... DP-1's false and false"); `Change m_sound{true}` made `{false}` (app/daemon/apply_controller.h)
    //       -> the pending line and the refusal line; setSound's Working guard deleted -> the pending line ("a
    //       second change was sent"); the every-screen sentence reworded, and then the screen's `tr("The sound
    //       setting")` -> the refusal line.
    rig.daemonFake->hold = {QStringLiteral("SetSound"), QStringLiteral("SetMonitorSound")};
    const qsizetype soundCalls = rig.sent("SetSound").size();
    rig.applier->setSound(true);
    rig.applier->setScreenSound(false);
    const bool bothHeld = waitUntil([&] { return rig.daemonFake->held.size() == 2; }, kWaitMs);
    rig.applier->setSound(false);
    const bool ignored = rig.daemonDrained() && rig.sent("SetSound").size() == soundCalls + 1;
    const QVariant globalOnDp1 = rig.applier->pendingSound();
    const QVariant screenOnDp1 = rig.applier->pendingScreenSound();
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    const QVariant globalOnHdmi = rig.applier->pendingSound();
    const QVariant screenOnHdmi = rig.applier->pendingScreenSound();
    rig.kscreen->select(QStringLiteral("DP-1"));
    expect(bothHeld && ignored && globalOnDp1 == QVariant(true) && globalOnHdmi == QVariant(true) && screenOnDp1 == QVariant(false) &&
               !screenOnHdmi.isValid(),
           QStringLiteral("S15.480: while held, every screen's pending reads %1 on DP-1 and %2 on HDMI-A-1, DP-1's %3 and %4; a second "
                          "change was %5")
               .arg(globalOnDp1.toString(), globalOnHdmi.toString(), screenOnDp1.toString(), screenOnHdmi.toString(),
                    ignored ? QStringLiteral("ignored") : QStringLiteral("sent")));
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
    if (!waitUntil([&] { return rig.applier->soundState() != Progress::Working && rig.applier->screenSoundState() != Progress::Working; }, kWaitMs)) {
        harnessFailure("S15.480: the held sound changes never finished, so the refusal case cannot start");
        return;
    }
    rig.daemonFake->refuse.insert(QStringLiteral("SetSound"), {QStringLiteral("io.wallpaperx.Daemon.Error.RenderFailed"), QStringLiteral("disk full")});
    rig.daemonFake->refuse.insert(QStringLiteral("SetMonitorSound"), {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no")});
    rig.applier->setSound(false);
    rig.applier->setScreenSound(true);
    const bool failed = waitUntil([&] { return rig.applier->soundState() == Progress::Failed && rig.applier->screenSoundState() == Progress::Failed; }, kWaitMs);
    expect(failed && rig.applier->soundMessage() == u"Sound was not changed for every screen. The WallpaperX service could not do it: disk full" &&
               rig.applier->screenSoundMessage() == u"The sound setting for DP-1 was not changed. The WallpaperX service refused it: no",
           QStringLiteral("S15.480: the refusals read \"%1\" and \"%2\"").arg(rig.applier->soundMessage(), rig.applier->screenSoundMessage()));
    rig.daemonFake->refuse.clear();
}

void bindQueue() {
    Rig rig;
    if (!build(rig, "S15.236", QStringLiteral("binds"))) {
        return;
    }
    // GOAL: S15.236 -- binds run one at a time across every caller: an Apply on one unnamed screen
    //       and a pause change on another, started together, get two different ids, the lowest
    //       free ones in the order asked (0 holds a pause setting, so 1 and 2).
    // WHY-THIS-SHAPE: every GetStatus reply is held, and the first release waits until a GetStatus
    //       sent after both requests has arrived: a client that read the status for both binds at
    //       once has then had both reads released together and picks one id twice, whatever the
    //       timing. A client that serializes has only one bind's read outstanding at a time. Each
    //       round's wait has a deadline that fails the check, so a stall is never a pass.
    // CONTROL: app/daemon/daemon_client.cpp, bindMonitor() replaced by one that runs each bind at
    //       once, with no queue (its own GetStatus, freeMonitorId, SetMonitorOutput; control_batch,
    //       2026-10-02) -> red, "S15.236: DP-1 was bound to -1 and HDMI-A-1 to 1 (stalled 0; ...)".
    //       Making the queued bindNext() unconditional is not a usable break: it pops the queue twice
    //       and aborts in std::deque::front().
    rig.daemonFake->pause.insert(0, QStringLiteral("play"));
    rig.daemonFake->hold.insert(QStringLiteral("GetStatus"));
    rig.applier->apply();
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    rig.applier->setPauseMode(QStringLiteral("always_paused"));
    rig.client->refresh();
    bool stalled = !waitUntil([&] { return rig.daemonFake->held.size() >= 2; }, kWaitMs);
    const auto busy = [&] { return rig.applier->applyState() == Progress::Working || rig.applier->pauseState() == Progress::Working; };
    for (int round = 0; !stalled && round < 40; ++round) {
        rig.daemonFake->release();
        if (!waitUntil([&] { return !busy() || !rig.daemonFake->held.isEmpty(); }, kWaitMs)) {
            stalled = true;
        }
        if (!busy()) {
            break;
        }
    }
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
    const int dp1 = rig.daemonFake->outputs.value(QStringLiteral("DP-1"), -1);
    const int hdmi = rig.daemonFake->outputs.value(QStringLiteral("HDMI-A-1"), -1);
    expect(!stalled && !busy() && rig.applier->applyState() == Progress::Done && rig.applier->pauseState() == Progress::Done && dp1 == 1 && hdmi == 2,
           QStringLiteral("S15.236: DP-1 was bound to %1 and HDMI-A-1 to %2 (stalled %3; apply: %4; pause: %5)")
               .arg(dp1).arg(hdmi).arg(stalled).arg(rig.applier->applyMessage(), rig.applier->pauseMessage()));
}

void liveEdits() {
    constexpr const char* goals = "S15.237-S15.242";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("live"))) {
        return;
    }
    const auto edits = [&] { return rig.sent("SetWallpaperProperties"); };

    // GOAL: S15.237 -- an edit made before any Apply, or after an Apply the daemon refused, stays in
    //       the app; once an Apply is accepted, each edit sends SetWallpaperProperties(id, the whole
    //       set) to that screen's id, typed `ia{sv}`, and the screen counts as live even when Plasma
    //       then fails.
    // WHY-THIS-SHAPE: "nothing sent" is read after a marker call queued behind the edit has arrived
    //       (daemonDrained), not after a pause: the edit's call, had it been made, was queued first on
    //       the same connection and is at the stand-in by then.
    // CONTROL: app/daemon/apply_controller.cpp, control_batch, re-run 2026-10-15: `startLive(...)` added
    //       right after apply()'s Working state (live before the daemon accepted) -> red, "S15.237:
    //       after a refused Apply an edit sent 1 call(s), live on "DP-1""; sendLive()'s setProperties
    //       given only `speed` (re-run 2026-10-18 and 2026-10-28) -> red, "S15.237: with Plasma refusing (live on "DP-1"),
    //       the edit after Apply sent [ia{sv} 0 {speed=0.5(double)}] (1 call(s))".
    rig.library->setPropertyValue(QStringLiteral("speed"), 0.5);
    const bool beforeDrained = rig.daemonDrained();
    expect(beforeDrained && edits().isEmpty(), QStringLiteral("S15.237: an edit before any Apply sent %1 call(s)").arg(edits().size()));
    rig.daemonFake->refuse.insert(QStringLiteral("SetWallpaperWithProperties"),
                                  {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no")});
    const bool refusedApply = rig.apply();
    rig.daemonFake->refuse.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool refusedDrained = rig.daemonDrained();
    expect(refusedApply && refusedDrained && rig.applier->applyState() == Progress::Failed && rig.applier->liveOutput().isEmpty() && edits().isEmpty(),
           QStringLiteral("S15.237: after a refused Apply an edit sent %1 call(s), live on \"%2\"").arg(edits().size()).arg(rig.applier->liveOutput()));

    rig.shellFake->state.widgetsLocked = true;
    const bool applied = rig.apply();
    rig.shellFake->state.widgetsLocked = false;
    // Held from here, so S15.238's burst below is made while this edit's call is unanswered.
    rig.daemonFake->hold.insert(QStringLiteral("SetWallpaperProperties"));
    rig.library->setPropertyValue(QStringLiteral("speed"), 0.5);
    const bool sentOne = waitUntil([&] { return !edits().isEmpty(); }, kWaitMs);
    const bool afterDrained = rig.daemonDrained();
    const QVariantMap want = withPosition({{QStringLiteral("speed"), 0.5}, {QStringLiteral("mute"), true},
                                           {QStringLiteral("tint"), QStringLiteral("#ffffff")}, {QStringLiteral("volume"), 1.0}});
    const QVariantMap got = edits().isEmpty() ? QVariantMap() : edits().back().args.value(1).toMap();
    expect(applied && sentOne && afterDrained && rig.applier->applyState() == Progress::Failed && rig.applier->liveOutput() == u"DP-1" &&
               edits().size() == 1 && edits()[0].signature == u"ia{sv}" && edits()[0].args.value(0) == 0 && describeMap(got) == describeMap(want),
           QStringLiteral("S15.237: with Plasma refusing (live on \"%1\"), the edit after Apply sent [%2 %3 %4] (%5 call(s))")
               .arg(rig.applier->liveOutput(), edits().isEmpty() ? QString() : edits()[0].signature,
                    edits().isEmpty() ? QString() : edits()[0].args.value(0).toString(), describeMap(got))
               .arg(edits().size()));

    // GOAL: S15.238 -- one edit in flight at a time, and the latest next: ten edits made while the
    //       daemon holds its reply to the last call send nothing; when it answers, exactly one more
    //       call carries the last value.
    // WHY-THIS-SHAPE: the burst edits the volume, not the speed: ten distinct values need a float that
    //       is not limited to three, so the last is told apart from every other. The 0.5 speed edit's
    //       reply is held from before that edit was made, so the burst is
    //       provably made while a call is unanswered. Draining toward the stand-in (daemonDrained)
    //       says nothing about replies coming back to the client, so an unheld reply could land
    //       mid-burst. "Exactly one more" is read once the app has handled the reply to that one
    //       (repliesHandled) and any call it sent then has arrived (daemonDrained) -- not once it
    //       re-reads the status, which is S15.242's business and would blame a break there on this.
    // CONTROL: app/daemon/apply_controller.cpp, sendLive()'s `inFlight != m_sending.end()` made `false`
    //       (control_batch T6a, reverted) -> red, "S15.238: a burst of 10 edits made while a
    //       call was unanswered sent 10 call(s)" and "after the reply 10 call(s) were sent".
    rig.daemonFake->calls.clear();
    for (int i = 1; i <= 10; ++i) {
        rig.library->setPropertyValue(QStringLiteral("volume"), 0.1 * i - 0.05);
    }
    const bool burstDrained = rig.daemonDrained();
    const qsizetype duringBurst = edits().size();
    expect(burstDrained && duringBurst == 0,
           QStringLiteral("S15.238: a burst of 10 edits made while a call was unanswered sent %1 call(s)").arg(duringBurst));
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
    const bool next = waitUntil([&] { return !edits().isEmpty(); }, kWaitMs);
    const bool quiet = next && rig.repliesHandled() && rig.daemonDrained();
    const double last = edits().isEmpty() ? -1 : edits().back().args.value(1).toMap().value(QStringLiteral("volume")).toDouble();
    expect(next && quiet && edits().size() == 1 && qFuzzyCompare(last, 0.95),
           QStringLiteral("S15.238: after the reply %1 call(s) were sent (settled %2), the last with volume %3")
               .arg(edits().size()).arg(quiet).arg(last));

    // GOAL: S15.241 -- a refused edit is shown, naming the screen and the daemon's reason, and the
    //       next accepted edit clears it.
    // CONTROL: app/daemon/apply_controller.cpp, `m_liveError = error;` in sendLive's reply made
    //       `m_liveError = QString();` (control_batch, re-run 2026-10-15 and 2026-10-28) -> red, "S15.241: the refused edit
    //       showed """.
    rig.daemonFake->refuse.insert(QStringLiteral("SetWallpaperProperties"),
                                  {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("monitor 0 holds no wallpaper")});
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const QString wantError = QStringLiteral("A change was not applied to DP-1. The WallpaperX service refused it: monitor 0 holds no wallpaper");
    const bool shown = waitUntil([&] { return !rig.applier->liveError().isEmpty(); }, kWaitMs);
    expect(shown && rig.applier->liveError() == wantError, QStringLiteral("S15.241: the refused edit showed \"%1\"").arg(rig.applier->liveError()));
    rig.daemonFake->refuse.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 1.0);
    expect(waitUntil([&] { return rig.applier->liveError().isEmpty(); }, kWaitMs),
           QStringLiteral("S15.241: an accepted edit left \"%1\" shown").arg(rig.applier->liveError()));

    // GOAL: S15.242 -- once a stream of edits settles, the daemon's persistence health is read
    //       again, so a save the daemon could not queue behind the accepted edit just made (refused,
    //       or no writer) is shown: no write follows it, so no PersistenceChanged (S15.689) will.
    // WHY-THIS-SHAPE: S15.241's accepted edit left a re-read of its own in flight, and the stand-in
    //       builds each reply when it sends it, so that read would carry the failure set here with
    //       no help from the speed edit below. It is drained first: the marker follows it on the same
    //       connection, so it was answered "ok" before the failure is set. The status must then be
    //       read again after the flip. The wait's deadline fails the check.
    // CONTROL: app/daemon/apply_controller.cpp, control_batch, re-run 2026-10-17 and 2026-10-28: sendLive()'s
    //       `m_daemon.refresh();` in the idle branch deleted -> red, "S15.242: after an accepted edit
    //       the persistence problem reads "" (settled 1, 0 status read(s) since the failure)"; the
    //       refresh moved inside `if (m_liveError != error)` (re-read only when the shown error
    //       changes) -> red, the same line (2026-10-15, not re-run). With the drain and the read-after-the-failure condition
    //       both taken out (the shape the review found), that second break left the phase green.
    const bool settled = rig.daemonDrained();
    rig.daemonFake->calls.clear();
    rig.daemonFake->persistence = QStringLiteral("fail");
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool reread = waitUntil([&] { return !rig.client->persistenceProblem().isEmpty(); }, kWaitMs);
    expect(settled && reread && !rig.sent("GetStatus").isEmpty() &&
               rig.client->persistenceProblem() ==
                   u"The WallpaperX service could not save its settings, so they may be lost when it restarts: the disk is full",
           QStringLiteral("S15.242: after an accepted edit the persistence problem reads \"%1\" (settled %2, %3 status read(s) since the failure)")
               .arg(rig.client->persistenceProblem()).arg(settled).arg(rig.sent("GetStatus").size()));
    rig.daemonFake->persistence = QStringLiteral("ok");

    // GOAL: S15.239 -- the link ends when the wallpaper is closed or another is opened: reopening it
    //       without Apply, or editing the other one, sends nothing.
    // WHY-THIS-SHAPE: drained before the call log is cleared, so a call S15.242's edit queued cannot
    //       land after the clear and read as an edit made after reopening. DP-1 is shown another file while
    //       Ocean is closed: reopening Ocean with DP-1 still showing it relinks by design (S15.994).
    // CONTROL: app/daemon/apply_controller.cpp linkSelected() keeping the link while nothing is open
    //       (control_batch 2026-10-28, reverted) -> red, "S15.239: closed (ended 0) and reopened,
    //       live on "", an edit sent 0 call(s)"; its early return not comparing the wallpaper -> red, "S15.239: with
    //       another wallpaper opened, live on "DP-1", its edit sent 1 call(s)".
    rig.library->clearSelection();
    const bool closedEnded = rig.applier->liveOutput().isEmpty();
    rig.daemonFake->screens[rig.daemonFake->outputs.value(QStringLiteral("DP-1"))].path = QStringLiteral("/elsewhere/other.mp4");
    const bool elsewhere = rig.reread();
    rig.library->select(rig.ocean);
    const bool earlierArrived = rig.daemonDrained();
    rig.daemonFake->calls.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool reopenDrained = rig.daemonDrained();
    expect(elsewhere && earlierArrived && closedEnded && reopenDrained && rig.applier->liveOutput().isEmpty() && edits().isEmpty(),
           QStringLiteral("S15.239: closed (ended %1) and reopened, live on \"%2\", an edit sent %3 call(s)")
               .arg(closedEnded).arg(rig.applier->liveOutput()).arg(edits().size()));
    const bool reapplied = rig.apply();
    rig.library->select(rig.river);
    rig.daemonFake->calls.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool otherDrained = rig.daemonDrained();
    expect(reapplied && otherDrained && rig.applier->liveOutput().isEmpty() && edits().isEmpty(),
           QStringLiteral("S15.239: with another wallpaper opened, live on \"%1\", its edit sent %2 call(s)").arg(rig.applier->liveOutput()).arg(edits().size()));

    // GOAL: S15.240 -- an edit made while Apply is running is sent once the daemon accepts it.
    // WHY-THIS-SHAPE: the daemon's reply to SetWallpaperWithProperties is held, so the edit is
    //       provably made while Apply is running. DP-1 is shown another file first, so opening Ocean links
    //       nothing and the edit can only reach the daemon through the Apply landing.
    // CONTROL: app/daemon/apply_controller.cpp, startLive()'s `sendLive();` deleted (control_batch,
    //       re-run 2026-10-28) -> red, "S15.240: the edit made during Apply was sent as [] (held 1, landed 1)".
    rig.daemonFake->screens[rig.daemonFake->outputs.value(QStringLiteral("DP-1"))].path = QStringLiteral("/elsewhere/other.mp4");
    if (!rig.reread()) {
        harnessFailure(std::string(goals) + ": S15.240: DP-1 could not be shown another file to start from");
        return;
    }
    rig.library->select(rig.ocean);
    rig.daemonFake->hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    rig.applier->apply();
    const bool heldSet = waitUntil([&] { return !rig.daemonFake->held.isEmpty(); }, kWaitMs);
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    rig.daemonFake->calls.clear();
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
    const bool landed = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs);
    const bool editSent = waitUntil([&] { return !edits().isEmpty(); }, kWaitMs);
    QStringList speeds;
    for (const FakeCall& c : edits()) {
        speeds.append(c.args.value(1).toMap().value(QStringLiteral("speed")).toString());
    }
    expect(heldSet && landed && editSent && speeds == QStringList{QStringLiteral("2")},
           QStringLiteral("S15.240: the edit made during Apply was sent as [%1] (held %2, landed %3)").arg(joined(speeds)).arg(heldSet).arg(landed));
}

// S15.732-S15.734
void positionOnTheWire() {
    constexpr const char* goals = "S15.732-S15.734";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("position"))) {
        return;
    }
    const auto withProperties = [&] { return rig.sent("SetWallpaperWithProperties"); };
    const auto edits = [&] { return rig.sent("SetWallpaperProperties"); };
    const auto lastOf = [](const QList<FakeCall>& calls, int index) { return calls.isEmpty() ? QVariantMap() : calls.back().args.value(index).toMap(); };
    // What the detail page hands over: a mode name, and numbers QML may give as integers.
    if (!rig.library->select(rig.ocean) || !rig.library->setPropertyValue(QStringLiteral("position"), QStringLiteral("free")) ||
        !rig.library->setPropertyValue(QStringLiteral("position.zoom"), 2) || !rig.library->setPropertyValue(QStringLiteral("position.x"), -0.25)) {
        harnessFailure(std::string(goals) + ": ocean could not be opened and framed");
        return;
    }
    const QVariantMap own{{QStringLiteral("speed"), 1.0}, {QStringLiteral("mute"), true}, {QStringLiteral("tint"), QStringLiteral("#ffffff")},
                          {QStringLiteral("volume"), 1.0}};
    const auto framed = [&](const char* mode, double zoom, double x, double y) {
        QVariantMap values = own;
        values.insert({{QStringLiteral("position"), QString::fromLatin1(mode)}, {QStringLiteral("position.zoom"), zoom},
                       {QStringLiteral("position.x"), x}, {QStringLiteral("position.y"), y}});
        return values;
    };

    // GOAL: S15.732 -- Apply sends the position with the wallpaper's own values in SetWallpaperWithProperties:
    //       the mode as a string, zoom and offsets as doubles even when set from integers, all four always,
    //       and the set is one the daemon's own rules would show a video with.
    // WHY-THIS-SHAPE: compared as text naming each type, since QVariant's == takes an int for a double;
    //       the daemon cross-check refuses any type but double, bool and string, as the wire does.
    // CONTROL: app/library/wpx_package.cpp Wallpaper::accept() returning `value` instead of `*number` for a position
    //       key (control_batch 2026-10-28, reverted) -> red, "S15.732: Apply sent {..., position.zoom=2(int), ...} (1 call(s)), not {..., position.zoom=2(double), ...}".
    const bool applied = rig.apply();
    const QVariantMap sent = lastOf(withProperties(), 3);
    const QVariantMap wantApplied = framed("free", 2.0, -0.25, 0.0);
    const std::string appliedVerdict = daemonVerdictOnSent(sent, "video");
    expect(applied && withProperties().size() == 1 && describeMap(sent) == describeMap(wantApplied) && appliedVerdict.empty(),
           QStringLiteral("S15.732: Apply sent %1 (%2 call(s)), not %3; the daemon would say \"%4\"")
               .arg(describeMap(sent)).arg(withProperties().size()).arg(describeMap(wantApplied), QString::fromStdString(appliedVerdict)));
    if (!applied || rig.applier->liveOutput() != u"DP-1") {
        harnessFailure(std::string(goals) + ": the Apply did not leave DP-1 live, so no live edit can be graded");
        return;
    }

    // GOAL: S15.733 -- while live, a position edit sends SetWallpaperProperties(id, the whole set) with the
    //       new value typed for the daemon: an integer offset as a double, a mode as a string.
    // CONTROL: the same break (2026-10-28) -> red, "S15.733: the live edit sent {..., position.y=1(int), position.zoom=2(int), ...} (1 call(s)), not
    //       {..., position.y=1(double), position.zoom=2(double), ...}".
    const bool settled = rig.repliesHandled() && rig.daemonDrained();
    rig.daemonFake->calls.clear();
    rig.library->setPropertyValue(QStringLiteral("position.y"), 1);
    const bool editSent = waitUntil([&] { return !edits().isEmpty(); }, kWaitMs) && rig.daemonDrained();
    const QVariantMap edited = lastOf(edits(), 1);
    const QVariantMap wantEdited = framed("free", 2.0, -0.25, 1.0);
    const std::string editVerdict = daemonVerdictOnSent(edited, "video");
    expect(settled && editSent && edits().size() == 1 && edits()[0].signature == u"ia{sv}" && describeMap(edited) == describeMap(wantEdited) &&
               editVerdict.empty(),
           QStringLiteral("S15.733: the live edit sent %1 (%2 call(s)), not %3; the daemon would say \"%4\"")
               .arg(describeMap(edited)).arg(edits().size()).arg(describeMap(wantEdited), QString::fromStdString(editVerdict)));

    // GOAL: S15.734 -- a mode changed while the daemon is gone reaches the daemon that returns in the
    //       SetWallpaperWithProperties re-send, the framing kept beside it.
    // WHY-THIS-SHAPE: graded on the call the returned stand-in received, as S15.647 is; the wait's deadline
    //       fails the check.
    // CONTROL: app/daemon/daemon_client.cpp `emit serviceStarted();` deleted (control_batch 2026-10-18 and
    //       2026-10-28, reverted) -> red, "S15.734: after the daemon returned it received {} (0 call(s)), not {..., position=fit(QString), ...}".
    const bool quiet = rig.repliesHandled() && rig.daemonDrained();
    rig.daemonFake->stop();
    const bool gone = waitUntil([&] { return rig.client->state() == DaemonClient::State::Unavailable; }, kWaitMs);
    rig.library->setPropertyValue(QStringLiteral("position"), QStringLiteral("fit"));
    const bool refusedShown = waitUntil([&] { return !rig.applier->liveError().isEmpty(); }, kWaitMs);
    rig.daemonFake->calls.clear();
    const QString restarted = rig.daemonFake->start();
    if (!quiet || !gone || !refusedShown || !restarted.isEmpty()) {
        harnessFailure(std::string(goals) + ": the daemon's restart could not be staged: " + restarted.toStdString());
        return;
    }
    const QVariantMap wantResent = framed("fit", 2.0, -0.25, 1.0);
    const bool resent = waitUntil([&] { return describeMap(lastOf(withProperties(), 3)) == describeMap(wantResent); }, kWaitMs);
    expect(resent && daemonVerdictOnSent(lastOf(withProperties(), 3), "video").empty(),
           QStringLiteral("S15.734: after the daemon returned it received %1 (%2 call(s)), not %3")
               .arg(describeMap(lastOf(withProperties(), 3))).arg(withProperties().size()).arg(describeMap(wantResent)));
}

void failureSentences() {
    constexpr const char* goals = "S15.244, S15.231";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("errors"), {1500}, 1500)) {
        return;
    }
    // GOAL: S15.244, S15.231 -- Apply's failure says which half failed and why, as one sentence:
    //       the daemon not running, not answering, older than the app, refusing the values; Plasma
    //       not running, its widgets locked, the plugin missing, the switch unconfirmed. A failed
    //       daemon half leaves whatever was live as it was; a failed Plasma half leaves the screen live.
    // WHY-THIS-SHAPE: the whole sentence is compared, so a reason dropped, or the wrong half named,
    //       goes red; each row restores the stand-ins first, so one row's state cannot pass another.
    //       The Plasma rows run first, so DP-1 is live when the daemon rows run and "as it was"
    //       means kept: rows before any accepted Apply would pass a controller that drops the link.
    // CONTROL: control_batch, 2026-10-02: app/daemon/daemon_client.cpp describe() reduced to its
    //       `default:` sentence -> red, "S15.244: daemon not running gave "Ocean Waves was not applied
    //       to DP-1. The WallpaperX service could not do it: The name ... was not provided by any
    //       .service files"", and the not-answering, older and refusing rows; app/kde/plasma_controller.cpp
    //       describe() reduced to `return scriptFailure(error.message());` -> red, "S15.244: no
    //       plasmashell gave "WallpaperX has Ocean Waves ready for DP-1, ... Plasma could not run the
    //       script that switches the desktop: The name ... was not provided by any .service files"".
    //       app/daemon/apply_controller.cpp apply() resetting the live link before the bind
    //       (control_batch, 2026-10-04) -> red, the four daemon rows; re-run 2026-10-28 -> red, the
    //       "daemon not running" row only: the bind's GetStatus relinked DP-1 in the other three. So each daemon
    //       row now stores another speed for DP-1 in the stand-in, which only a rebuilt link adopts; re-run
    //       (control_batch T7a, reverted) -> red, all four daemon rows, e.g. "S15.244: daemon refusing gave "..."
    //       (state 3, live on "DP-1" before "DP-1", panel speed 0.5 before 2, marked 1)".
    // COVERAGE-ASSERT: all eight rows ran.
    const QString daemonHalf = QStringLiteral("Ocean Waves was not applied to DP-1. ");
    const QString plasmaHalf = QStringLiteral("WallpaperX has Ocean Waves ready for DP-1, but the desktop there was not switched to WallpaperX. ");
    struct Row {
        const char* what;
        std::function<void()> arrange;
        QString sentence;
        bool daemonAccepted;
    };
    FakeDaemon& d = *rig.daemonFake;
    FakeShell& s = *rig.shellFake;
    const Row rows[] = {
        {"no plasmashell", [&] { s.stop(); }, plasmaHalf + QStringLiteral("Plasma's desktop shell (plasmashell) is not running."), true},
        {"widgets locked", [&] { s.state.widgetsLocked = true; },
         plasmaHalf + QStringLiteral("Plasma could not run the script that switches the desktop: Widgets are locked"), true},
        {"plugin missing", [&] { s.state.plugins = {QStringLiteral("Image")}; },
         plasmaHalf + QStringLiteral("The WallpaperX wallpaper plugin is not installed for Plasma."), true},
        {"switch unconfirmed", [&] { s.state.printReaches = false; },
         plasmaHalf + QStringLiteral("Plasma ran the script that switches the desktop but did not say it had."), true},
        {"daemon not running", [&] { d.stop(); }, daemonHalf + QStringLiteral("The WallpaperX service (wallpaperx-daemon) is not running."), false},
        {"daemon not answering", [&] { d.hold.insert(QStringLiteral("SetWallpaperWithProperties")); },
         daemonHalf + QStringLiteral("The WallpaperX service did not answer."), false},
        {"daemon older", [&] { d.refuse.insert(QStringLiteral("SetWallpaperWithProperties"), {QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"), QStringLiteral("no")}); },
         daemonHalf + QStringLiteral("The WallpaperX service is older than this app: it does not know SetWallpaperWithProperties."), false},
        {"daemon refusing", [&] { d.refuse.insert(QStringLiteral("SetWallpaperWithProperties"), {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("speed must be in (0, 2]")}); },
         daemonHalf + QStringLiteral("The WallpaperX service refused it: speed must be in (0, 2]"), false},
    };
    int ran = 0;
    for (const Row& row : rows) {
        d.refuse.clear();
        d.hold.clear();
        s.state = twoScreenPlasma();
        if (const QString why = d.start() + s.start(); !why.isEmpty() ||
                                                       !waitUntil([&] { return rig.client->state() == DaemonClient::State::Ready; }, kWaitMs)) {
            harnessFailure("S15.244: the stand-ins could not be restored before \"" + std::string(row.what) + "\": " + why.toStdString());
            break;
        }
        const QString liveBefore = rig.applier->liveOutput();
        // A daemon row stores another speed for DP-1 than the panel shows: a kept link ignores it, a link
        // rebuilt from the bind's GetStatus adopts it, so the panel tells the two apart.
        const int dp1 = d.outputs.value(QStringLiteral("DP-1"), -1);
        const QVariantMap panelBefore = rig.library->propertyValues();
        const QVariantMap storedBefore = d.screens.value(dp1).properties;
        const bool marked = !row.daemonAccepted && d.screens.contains(dp1);
        if (marked) {
            d.screens[dp1].properties.insert(QStringLiteral("speed"), panelBefore.value(QStringLiteral("speed")).toDouble() == 0.5 ? 2.0 : 0.5);
        }
        row.arrange();
        const bool finished = rig.apply();
        ++ran;
        const QString liveAfter = rig.applier->liveOutput();
        const bool liveRight = row.daemonAccepted ? liveAfter == u"DP-1" : liveAfter == liveBefore && marked;
        const bool panelKept = row.daemonAccepted || rig.library->propertyValues() == panelBefore;
        expect(finished && rig.applier->applyState() == Progress::Failed && rig.applier->applyMessage() == row.sentence && liveRight && panelKept,
               QStringLiteral("S15.244: %1 gave \"%2\" (state %3, live on \"%4\" before \"%5\", panel speed %7 before %8, marked %9), not \"%6\"")
                   .arg(QLatin1StringView(row.what), rig.applier->applyMessage())
                   .arg(int(rig.applier->applyState()))
                   .arg(liveAfter, liveBefore, row.sentence)
                   .arg(rig.library->propertyValues().value(QStringLiteral("speed")).toDouble())
                   .arg(panelBefore.value(QStringLiteral("speed")).toDouble())
                   .arg(marked));
        d.release();
        if (marked) {
            d.screens[dp1].properties = storedBefore;
        }
    }
    expect(ran == 8, QStringLiteral("S15.244: %1 of 8 rows ran").arg(ran));
}

void pauseModes() {
    Rig rig;
    if (!build(rig, "S15.245", QStringLiteral("pause"))) {
        return;
    }
    // GOAL: S15.245 -- the app's pause modes are exactly the daemon's names (backend/daemon/
    //       pause_mode.h), in its order; each is sent as SetMonitorPause(id, name) typed `is` and
    //       then shown for the screen; any other name is ignored and sends nothing.
    // WHY-THIS-SHAPE: the daemon's names come from its own header through daemon_rules.cpp, not from
    //       a list in this file, so the app and the check cannot drift together. Each mode's read-back
    //       is the screen's shown mode; that it comes from GetStatus is graded by the null case below.
    // CONTROL: control_batch, 2026-10-02: app/daemon/daemon_client.cpp isPauseMode() made `return
    //       true || ...` -> red, "S15.245: the unknown mode "bogus" left the pause 1 and sent 0
    //       SetMonitorPause" (Working: the change was taken); app/daemon/daemon_client.h kPauseModes'
    //       "paused_while_focused" made "paused_when_focused" -> red, "S15.245: the app's modes are
    //       [play,always_paused,paused_when_focused,paused_while_maximized], the daemon's [...]".
    QStringList daemonNames;
    for (const std::string& n : daemonPauseModeNames()) {
        daemonNames.append(QString::fromStdString(n));
    }
    QStringList appNames;
    for (const char* n : kPauseModes) {
        appNames.append(QString::fromLatin1(n));
    }
    expect(appNames == daemonNames && daemonNames.size() == 4,
           QStringLiteral("S15.245: the app's modes are [%1], the daemon's [%2]").arg(joined(appNames), joined(daemonNames)));
    for (const QString& bad : {QStringLiteral("bogus"), QStringLiteral("Play"), QString()}) {
        rig.applier->setPauseMode(bad);
        const bool drained = rig.daemonDrained();
        expect(drained && rig.applier->pauseState() == Progress::Idle && rig.sent("SetMonitorPause").isEmpty(),
               QStringLiteral("S15.245: the unknown mode \"%1\" left the pause %2 and sent %3 SetMonitorPause")
                   .arg(bad).arg(int(rig.applier->pauseState())).arg(rig.sent("SetMonitorPause").size()));
    }
    for (const QString& mode : daemonNames) {
        rig.applier->setPauseMode(mode);
        const bool done = waitUntil([&] { return rig.applier->pauseState() != Progress::Working; }, kWaitMs);
        const auto sent = rig.sent("SetMonitorPause");
        const bool reread = rig.daemonDrained() && waitUntil([&] { return rig.client->pauseModes().value(QStringLiteral("DP-1")) == mode; }, kWaitMs);
        expect(done && reread && rig.applier->pauseState() == Progress::Done && !sent.isEmpty() && sent.back().signature == u"is" &&
                   sent.back().args == QVariantList{0, mode},
               QStringLiteral("S15.245: %1 was sent as %2 and reads back as \"%3\"")
                   .arg(mode, sent.isEmpty() ? QStringLiteral("(nothing)") : sent.back().signature + u' ' + sent.back().args.value(1).toString(),
                        rig.client->pauseModes().value(QStringLiteral("DP-1")).toString()));
    }

    // GOAL: S15.245 -- a mode the daemon reports that this app does not know (a newer daemon's,
    //       null in GetStatus) reads as "", never as one of the four; a screen with no setting plays.
    // CONTROL: app/daemon/daemon_client.cpp, pauseModes()'s `mode->value_or(QString())` made
    //       `mode->value_or(QStringLiteral("play"))` (control_batch, 2026-10-02) -> red, "S15.245: a
    //       mode the daemon does not name reads as "play"".
    rig.daemonFake->pause.insert(0, QJsonValue::Null);
    rig.client->refresh();
    const bool unknown = waitUntil([&] { const QVariant m = rig.client->pauseModes().value(QStringLiteral("DP-1")); return m.isValid() && m.toString().isEmpty(); }, kWaitMs);
    expect(unknown, QStringLiteral("S15.245: a mode the daemon does not name reads as \"%1\"").arg(rig.client->pauseModes().value(QStringLiteral("DP-1")).toString()));

    // A refused change is reported with the daemon's reason.
    rig.daemonFake->refuse.insert(QStringLiteral("SetMonitorPause"), {QStringLiteral("io.wallpaperx.Daemon.Error.RenderFailed"), QStringLiteral("could not queue the save")});
    rig.applier->setPauseMode(QStringLiteral("always_paused"));
    const bool failed = waitUntil([&] { return rig.applier->pauseState() == Progress::Failed; }, kWaitMs);
    expect(failed && rig.applier->pauseMessage() == u"The pause setting for DP-1 was not changed. The WallpaperX service could not do it: could not queue the save",
           QStringLiteral("S15.245: a refused pause change reads \"%1\"").arg(rig.applier->pauseMessage()));
}

void daemonPresence() {
    Rig rig;
    if (!build(rig, "S15.246", QStringLiteral("presence"))) {
        return;
    }
    // GOAL: S15.246 -- the daemon leaving the bus makes the status Unavailable, saying it is not
    //       running; its return makes the client read the status again, back to Ready.
    // CONTROL: app/daemon/daemon_client.cpp, the owner-change handler's `refresh();` deleted
    //       (control_batch, 2026-10-02) -> red, "S15.246: after the daemon came back the client is state 2".
    rig.daemonFake->stop();
    const bool gone = waitUntil([&] { return rig.client->state() == DaemonClient::State::Unavailable; }, kWaitMs);
    expect(gone && rig.client->unavailableReason() == u"The WallpaperX service (wallpaperx-daemon) is not running.",
           QStringLiteral("S15.246: after the daemon left the client is state %1: \"%2\"").arg(int(rig.client->state())).arg(rig.client->unavailableReason()));
    const QString restarted = rig.daemonFake->start();
    const bool back = restarted.isEmpty() && waitUntil([&] { return rig.client->state() == DaemonClient::State::Ready; }, kWaitMs);
    if (!restarted.isEmpty()) {
        harnessFailure("S15.246: the daemon stand-in could not take its name back: " + restarted.toStdString());
    } else {
        expect(back, QStringLiteral("S15.246: after the daemon came back the client is state %1").arg(int(rig.client->state())));
    }
}

void daemonReturn() {
    constexpr const char* goals = "S15.647, S15.648";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("return"))) {
        return;
    }
    // The re-send names the wallpaper (SetWallpaperWithProperties, properties third); a later edit
    // on the same daemon addresses the monitor alone (SetWallpaperProperties).
    const auto resends = [&] { return rig.sent("SetWallpaperWithProperties"); };
    const auto edits = [&] { return rig.sent("SetWallpaperProperties"); };
    const QString asset = rig.ocean + QStringLiteral("/assets/video.mp4");
    const auto speedResent = [&] {
        const auto r = resends();
        if (r.isEmpty() || r.back().args.value(0).toString() != u"video" || r.back().args.value(1).toString() != asset ||
            r.back().args.value(2).toInt() != 0) {
            return QVariant();
        }
        return r.back().args.value(3).toMap().value(QStringLiteral("speed"));
    };
    const auto describeResends = [&] {
        QStringList out;
        for (const FakeCall& c : resends()) {
            out.append(c.args.value(1).toString() + u' ' + c.args.value(2).toString() + u' ' + describeMap(c.args.value(3).toMap()));
        }
        return u'[' + joined(out) + u']';
    };
    const bool applied = rig.apply();
    const bool live = applied && rig.applier->liveOutput() == u"DP-1";
    if (!live) {
        harnessFailure(std::string(goals) + ": the Apply this phase starts from did not leave DP-1 live");
        return;
    }

    // GOAL: S15.647 -- a speed changed while the daemon is gone reaches the daemon that returns, before
    //       the user touches anything again, as SetWallpaperWithProperties naming the open wallpaper and
    //       its monitor, and the "not running" sentence that change left is cleared.
    // WHY-THIS-SHAPE: graded on the call the stand-in daemon received after its name came back, not on
    //       the app's state: an app that only cleared its sentence would pass a check of the app alone
    //       while the daemon plays the old speed. The wallpaper is named because a restarted daemon may
    //       hold another one on that monitor. The wait's deadline fails the check.
    // CONTROL: control_batch, 2026-10-17, each reverted: app/daemon/daemon_client.cpp, `emit
    //       serviceStarted();` deleted (re-run 2026-10-28) -> red, "S15.647: after the daemon returned it received [] naming the
    //       wallpaper and 0 SetWallpaperProperties, the app shows "... is not running.""; app/daemon/
    //       apply_controller.cpp sendLive()'s `if (m_live->sent)` made `if (true)` (re-sent by monitor alone)
    //       -> red, "... received [] naming the wallpaper and 1 SetWallpaperProperties ...".
    rig.daemonFake->stop();
    const bool gone = waitUntil([&] { return rig.client->state() == DaemonClient::State::Unavailable; }, kWaitMs);
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool failedShown = waitUntil([&] { return !rig.applier->liveError().isEmpty(); }, kWaitMs);
    if (!gone || !failedShown) {
        harnessFailure(std::string(goals) + ": the edit made with the daemon gone was not refused, so there is nothing to re-send");
        return;
    }
    rig.daemonFake->calls.clear();
    const QString restarted = rig.daemonFake->start();
    if (!restarted.isEmpty()) {
        harnessFailure(std::string(goals) + ": the daemon stand-in could not take its name back: " + restarted.toStdString());
        return;
    }
    const bool resent = waitUntil([&] { return speedResent() == QVariant(2.0); }, kWaitMs);
    const bool cleared = waitUntil([&] { return rig.applier->liveError().isEmpty(); }, kWaitMs);
    expect(resent && cleared && edits().isEmpty(),
           QStringLiteral("S15.647: after the daemon returned it received %1 naming the wallpaper and %2 SetWallpaperProperties, the app shows \"%3\"")
               .arg(describeResends()).arg(edits().size()).arg(rig.applier->liveError()));

    // GOAL: S15.688 -- once the re-send naming the wallpaper is accepted, the next edit addresses the
    //       monitor alone (SetWallpaperProperties) and does not set the wallpaper again.
    // WHY-THIS-SHAPE: read once the re-send's reply is handled and the edit has reached the stand-in, so
    //       an edit sent before the reply landed, which may rightly be a second full call, is not graded.
    // CONTROL: app/daemon/apply_controller.cpp sendLive()'s `if (m_live->sent)` made `if (false)` (control_batch,
    //       2026-10-17, reverted) -> red, "S15.688: after the accepted re-send an edit sent 0
    //       SetWallpaperProperties (speed "") and [.../ocean/assets/video.mp4 0 {... speed=0.5 ...}]".
    const bool resendSettled = rig.repliesHandled() && rig.daemonDrained() && rig.repliesHandled();
    rig.daemonFake->calls.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 0.5);
    const bool edited = waitUntil([&] { return !edits().isEmpty(); }, kWaitMs) && rig.daemonDrained();
    const QVariant editSpeed = edits().isEmpty() ? QVariant() : edits().back().args.value(1).toMap().value(QStringLiteral("speed"));
    expect(resendSettled && edited && editSpeed == QVariant(0.5) && resends().isEmpty(),
           QStringLiteral("S15.688: after the accepted re-send an edit sent %1 SetWallpaperProperties (speed \"%2\") and %3")
               .arg(edits().size()).arg(editSpeed.toString()).arg(describeResends()));

    // GOAL: S15.648 -- the open wallpaper's values are re-sent on every daemon start, even when the
    //       app's last change was accepted: a restarted daemon holds only what it saved.
    // WHY-THIS-SHAPE: the second restart follows an accepted change, which is the case an app keeping
    //       "what the daemon accepted" across the restart would skip as already sent.
    // CONTROL: app/daemon/apply_controller.cpp, `m_live->sent.reset();` in the serviceStarted handler
    //       deleted (control_batch, re-run 2026-10-17 and 2026-10-28) -> red, "S15.648: after a restart following an accepted
    //       change the daemon received [] naming the wallpaper and 0 SetWallpaperProperties", beside S15.647's.
    const bool drained = rig.repliesHandled() && rig.daemonDrained();
    rig.daemonFake->stop();
    const bool goneAgain = waitUntil([&] { return rig.client->state() == DaemonClient::State::Unavailable; }, kWaitMs);
    rig.daemonFake->calls.clear();
    const QString again = rig.daemonFake->start();
    if (!drained || !goneAgain || !again.isEmpty()) {
        harnessFailure(std::string(goals) + ": the second restart could not be staged: " + again.toStdString());
        return;
    }
    const bool resentAgain = waitUntil([&] { return speedResent() == QVariant(0.5); }, kWaitMs);
    expect(resentAgain, QStringLiteral("S15.648: after a restart following an accepted change the daemon received %1 naming the wallpaper and %2 SetWallpaperProperties")
                            .arg(describeResends()).arg(edits().size()));
}

void persistenceSignal() {
    constexpr const char* goals = "S15.689";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("persisted"))) {
        return;
    }
    const QString failing =
        QStringLiteral("The WallpaperX service could not save its settings, so they may be lost when it restarts: the disk is full");
    const bool applied = rig.apply();
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    // The edit's reply, the status read it starts, and that read's reply: nothing of the app's own is
    // left in flight when the failure is set.
    const bool settled = applied && waitUntil([&] { return !rig.sent("SetWallpaperProperties").isEmpty(); }, kWaitMs) &&
                         rig.repliesHandled() && rig.daemonDrained() && rig.repliesHandled();
    if (!settled || !rig.client->persistenceProblem().isEmpty()) {
        harnessFailure(std::string(goals) + ": the accepted edit this phase starts from did not settle with no problem shown");
        return;
    }

    // GOAL: S15.689 -- a save that fails after the daemon has answered (its write runs 250 ms later) is
    //       shown once the daemon announces PersistenceChanged, and a failure cleared later stops showing
    //       on the next announcement.
    // WHY-THIS-SHAPE: the app is idle when the stand-in's status flips -- every reply handled -- so the
    //       signal is the only thing that can make it read GetStatus again; the read is asserted to have
    //       happened. The wait's deadline fails the check.
    // CONTROL: control_batch, each reverted: app/daemon/daemon_client.cpp, the constructor's PersistenceChanged
    //       connection deleted (re-run 2026-10-28) -> red, "S15.689: after PersistenceChanged announcing a failed write the app shows \"\" (0 status read(s))"; (2026-10-17) DaemonClient::refresh() returning at once while
    //       persistence reads "fail" -> red, "S15.689: after PersistenceChanged announcing a cleared failure
    //       the app shows "The WallpaperX service could not save its settings, ...: the disk is full"".
    rig.daemonFake->calls.clear();
    rig.daemonFake->persistence = QStringLiteral("fail");
    const bool sentFail = rig.daemonFake->announcePersistenceChanged();
    const bool shown = waitUntil([&] { return !rig.client->persistenceProblem().isEmpty(); }, kWaitMs);
    expect(sentFail && shown && rig.client->persistenceProblem() == failing && !rig.sent("GetStatus").isEmpty(),
           QStringLiteral("S15.689: after PersistenceChanged announcing a failed write the app shows \"%1\" (%2 status read(s))")
               .arg(rig.client->persistenceProblem()).arg(rig.sent("GetStatus").size()));

    const bool idle = rig.daemonDrained() && rig.repliesHandled();
    rig.daemonFake->persistence = QStringLiteral("ok");
    const bool sentOk = rig.daemonFake->announcePersistenceChanged();
    const bool gone = waitUntil([&] { return rig.client->persistenceProblem().isEmpty(); }, kWaitMs);
    expect(idle && sentOk && gone,
           QStringLiteral("S15.689: after PersistenceChanged announcing a cleared failure the app shows \"%1\"").arg(rig.client->persistenceProblem()));
}

void lateReplies() {
    Rig rig;
    if (!build(rig, "S15.247", QStringLiteral("late"))) {
        return;
    }
    // GOAL: S15.247 -- a reply arriving after its caller's context is destroyed runs nothing: for a
    //       daemon call, a bind, and a Plasma call.
    // WHY-THIS-SHAPE: each reply is held until the context is gone, and the verdict is read once a
    //       later call on the same connection has been answered, so the stale reply has been
    //       delivered by then. The counter makes a breach a FAIL line; the ApplyController case
    //       below relies on AddressSanitizer instead.
    // CONTROL: control_batch, 2026-10-04: app/daemon/daemon_client.cpp send()'s
    //       `whenAnswered(context, ...)` given `this` -> red, "S15.247: 1 callback(s) ran for a
    //       destroyed caller (daemon call 1, bind 0, Plasma 0)"; DaemonClient::finishBind()'s
    //       `if (finished.context)` made `if (true)` -> red, "(daemon call 0, bind 1, Plasma 0)";
    //       app/kde/plasma_controller.cpp activate()'s `whenAnswered(context, ...)` given `this` ->
    //       red, "(daemon call 0, bind 0, Plasma 1)".
    int callRan = 0, bindRan = 0, plasmaRan = 0;
    {
        rig.daemonFake->hold = {QStringLiteral("SetWallpaperProperties"), QStringLiteral("GetStatus")};
        rig.shellFake->hold.insert(QStringLiteral("evaluateScript"));
        auto context = std::make_unique<QObject>();
        rig.client->setProperties(context.get(), 0, {{QStringLiteral("speed"), 1.0}}, [&](std::expected<void, QString>) { ++callRan; });
        rig.client->bindMonitor(context.get(), QStringLiteral("DP-9"), [&](std::expected<int, QString>) { ++bindRan; });
        rig.plasma->activate(context.get(), QStringLiteral("DP-1"), [&](std::expected<int, QString>) { ++plasmaRan; });
        const bool allHeld = waitUntil([&] { return rig.daemonFake->held.size() == 2 && rig.shellFake->held.size() == 1; }, kWaitMs);
        context.reset();
        rig.daemonFake->hold.clear();
        rig.shellFake->hold.clear();
        rig.daemonFake->release();
        rig.shellFake->release();
        // Answered after the stale ones, on the same connections.
        bool laterCall = false, laterBind = false;
        QObject alive;
        rig.client->setProperties(&alive, 0, {{QStringLiteral("speed"), 1.0}}, [&](std::expected<void, QString>) { laterCall = true; });
        rig.client->bindMonitor(&alive, QStringLiteral("DP-8"), [&](std::expected<int, QString>) { laterBind = true; });
        const bool settled = allHeld && waitUntil([&] { return laterCall && laterBind; }, kWaitMs) && rig.shellDrained();
        if (!settled) {
            harnessFailure("S15.247: the held replies were never all delivered");
        } else {
            expect(callRan + bindRan + plasmaRan == 0,
                   QStringLiteral("S15.247: %1 callback(s) ran for a destroyed caller (daemon call %2, bind %3, Plasma %4)")
                       .arg(callRan + bindRan + plasmaRan).arg(callRan).arg(bindRan).arg(plasmaRan));
        }
    }

    // GOAL: S15.247 -- an ApplyController destroyed with an Apply's reply outstanding is never
    //       called back.
    // WHY-THIS-SHAPE: the callback would touch the freed controller, which only AddressSanitizer
    //       can see, so this file is built with it, as app_kscreen_launcher is; the failure is
    //       ASan's heap-use-after-free report and a non-zero exit, not a FAIL line. The line below
    //       records that the run got past the delivery.
    // CONTROL: app/daemon/daemon_client.cpp, send()'s `whenAnswered(context, ...)` given `this`
    //       (control_batch, 2026-10-04) -> red, "AddressSanitizer: heap-use-after-free ... in
    //       wallpaperx::ApplyController::startLive(...)", after the counter line above.
    rig.library->select(rig.ocean);
    rig.kscreen->select(QStringLiteral("DP-1"));
    rig.daemonFake->hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    rig.applier->apply();
    if (!waitUntil([&] { return !rig.daemonFake->held.isEmpty(); }, kWaitMs)) {
        harnessFailure("S15.247: the Apply never reached SetWallpaperWithProperties");
        return;
    }
    rig.applier.reset();
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
    bool later = false;
    QObject alive;
    rig.client->setProperties(&alive, 0, {{QStringLiteral("speed"), 1.0}}, [&](std::expected<void, QString>) { later = true; });
    if (!waitUntil([&] { return later; }, kWaitMs)) {
        harnessFailure("S15.247: the reply after the destroyed Apply's was never delivered");
    } else {
        std::printf("apply: S15.247: the reply to a destroyed ApplyController's Apply was delivered and nothing touched it\n");
    }
}

void pauseScope() {
    constexpr const char* goals = "S15.248, S15.249";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("pausescope"))) {
        return;
    }
    // GOAL: S15.248 -- a pause change still running shows its mode only for the screen it is for:
    //       with DP-1's change held, HDMI-A-1 shows no pending mode, and DP-1 shows the mode being set.
    // CONTROL: app/daemon/apply_controller.cpp, forSelected() -- which pendingPauseMode() reads -- made
    //       `return true` (control_batch, re-run 2026-10-11) -> red, "S15.248: with DP-1's change held,
    //       HDMI-A-1 shows "always_paused" pending and DP-1 "always_paused" (held 1, state 1)".
    rig.daemonFake->hold.insert(QStringLiteral("SetMonitorPause"));
    rig.applier->setPauseMode(QStringLiteral("always_paused"));
    const bool held = waitUntil([&] { return !rig.daemonFake->held.isEmpty(); }, kWaitMs);
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    const QString onHdmi = rig.applier->pendingPauseMode();
    rig.kscreen->select(QStringLiteral("DP-1"));
    const QString onDp1 = rig.applier->pendingPauseMode();
    expect(held && rig.applier->pauseState() == Progress::Working && onHdmi.isEmpty() && onDp1 == u"always_paused",
           QStringLiteral("S15.248: with DP-1's change held, HDMI-A-1 shows \"%1\" pending and DP-1 \"%2\" (held %3, state %4)")
               .arg(onHdmi, onDp1).arg(held).arg(int(rig.applier->pauseState())));
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
    if (!waitUntil([&] { return rig.applier->pauseState() != Progress::Working; }, kWaitMs)) {
        harnessFailure("S15.249: the held pause change never finished, so the failure case cannot start");
        return;
    }

    // GOAL: S15.249 -- a failed pause change's sentence shows only while the screen it was for is
    //       selected: not under HDMI-A-1, and again on returning to DP-1.
    // CONTROL: app/daemon/apply_controller.h, pauseMessage() returning `m_pause.message` whatever is
    //       selected (control_batch, re-run 2026-10-11) -> red, "S15.249: DP-1's refused change reads "The
    //       pause setting for DP-1 was not changed. ..." on DP-1, the same on HDMI-A-1 and back on DP-1".
    rig.daemonFake->refuse.insert(QStringLiteral("SetMonitorPause"), {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no")});
    rig.applier->setPauseMode(QStringLiteral("paused_while_focused"));
    const bool failed = waitUntil([&] { return rig.applier->pauseState() == Progress::Failed; }, kWaitMs);
    const QString want = QStringLiteral("The pause setting for DP-1 was not changed. The WallpaperX service refused it: no");
    const QString first = rig.applier->pauseMessage();
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    const QString elsewhere = rig.applier->pauseMessage();
    rig.kscreen->select(QStringLiteral("DP-1"));
    const QString back = rig.applier->pauseMessage();
    expect(failed && first == want && elsewhere.isEmpty() && back == want,
           QStringLiteral("S15.249: DP-1's refused change reads \"%1\" on DP-1, \"%2\" on HDMI-A-1 and \"%3\" back on DP-1")
               .arg(first, elsewhere, back));
}

void applyScope() {
    Rig rig;
    if (!build(rig, "S15.250", QStringLiteral("applyscope"))) {
        return;
    }
    // GOAL: S15.250 -- a finished Apply's sentence shows only while the wallpaper and screen it was
    //       for are both selected: empty with River open, empty with HDMI-A-1 selected, back on
    //       returning to each.
    // CONTROL: app/daemon/apply_controller.cpp, applyMessage()'s `selected` made `true`
    //       (control_batch, 2026-10-04) -> red, "S15.250: Ocean Waves on DP-1 reads "Ocean Waves is on
    //       DP-1." there, "Ocean Waves is on DP-1." with River open, ..." and the same on HDMI-A-1.
    const bool done = rig.apply();
    const auto message = [&] { return rig.applier->applyMessage(); };
    const QString want = QStringLiteral("Ocean Waves is on DP-1.");
    const QString there = message();
    rig.library->select(rig.river);
    const QString onRiver = message();
    rig.library->select(rig.ocean);
    const QString backOcean = message();
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    const QString onHdmi = message();
    rig.kscreen->select(QStringLiteral("DP-1"));
    const QString backDp1 = message();
    expect(done && rig.applier->applyState() == Progress::Done && there == want && onRiver.isEmpty() && backOcean == want &&
               onHdmi.isEmpty() && backDp1 == want,
           QStringLiteral("S15.250: Ocean Waves on DP-1 reads \"%1\" there, \"%2\" with River open, \"%3\" back on Ocean, \"%4\" "
                          "on HDMI-A-1, \"%5\" back on DP-1")
               .arg(there, onRiver, backOcean, onHdmi, backDp1));

    // GOAL: S15.250 -- while an Apply runs its sentence shows whatever is selected, so the disabled
    //       button is explained everywhere.
    // CONTROL: app/daemon/apply_controller.cpp, applyMessage()'s `m_apply == Progress::Working ||`
    //       deleted (control_batch, 2026-10-04) -> red, "S15.250: while Ocean Waves is being applied to
    //       DP-1, River reads "" and HDMI-A-1 "" (held 1, state 1)".
    rig.daemonFake->hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    rig.applier->apply();
    const bool held = waitUntil([&] { return !rig.daemonFake->held.isEmpty(); }, kWaitMs);
    rig.library->select(rig.river);
    const QString runningOnRiver = message();
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    const QString runningOnHdmi = message();
    const QString applying = QStringLiteral("Applying Ocean Waves to DP-1…");
    expect(held && rig.applier->applyState() == Progress::Working && runningOnRiver == applying && runningOnHdmi == applying,
           QStringLiteral("S15.250: while Ocean Waves is being applied to DP-1, River reads \"%1\" and HDMI-A-1 \"%2\" (held %3, state %4)")
               .arg(runningOnRiver, runningOnHdmi).arg(held).arg(int(rig.applier->applyState())));
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
    waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs);
}

void bindAndRefresh() {
    constexpr const char* goals = "S15.251, S15.252";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("bindread"))) {
        return;
    }
    const QString failing = QStringLiteral("The WallpaperX service could not save its settings, so they may be lost when it restarts: the disk is full");
    QObject context;

    // GOAL: S15.251 -- a bind's status read never replaces the answer to a refresh sent after it:
    //       the bind's read answered with the save healthy, the save then failing, and a refresh
    //       sent before the bind's answer is handled, the client shows the failure.
    // WHY-THIS-SHAPE: every GetStatus is held, so the order is the test's own: the bind's read is
    //       answered before the refresh is sent, the refresh's only after the client has had the
    //       bind's answer; both come back on one connection, in that order. The stand-in builds a
    //       reply when it sends it, so each carries the state at its release.
    // CONTROL: app/daemon/daemon_client.cpp, bindNext()'s `if (generation == m_generation)` made
    //       `if (true)` (control_batch, 2026-10-04) -> red, "S15.251: with a refresh sent after the
    //       bind's read, the client shows "" for the save (bind held 1, refresh held 1)".
    rig.daemonFake->hold.insert(QStringLiteral("GetStatus"));
    bool bound = false;
    rig.client->bindMonitor(&context, QStringLiteral("DP-7"), [&](std::expected<int, QString>) { bound = true; });
    const bool bindHeld = waitUntil([&] { return rig.daemonFake->held.size() == 1; }, kWaitMs);
    rig.daemonFake->release();
    rig.daemonFake->persistence = QStringLiteral("fail");
    rig.client->refresh();
    const bool refreshHeld = bindHeld && waitUntil([&] { return rig.daemonFake->held.size() == 1; }, kWaitMs);
    rig.daemonFake->release();
    const bool shown = refreshHeld && waitUntil([&] { return !rig.client->persistenceProblem().isEmpty(); }, kWaitMs);
    expect(shown && rig.client->persistenceProblem() == failing,
           QStringLiteral("S15.251: with a refresh sent after the bind's read, the client shows \"%1\" for the save (bind held %2, refresh held %3)")
               .arg(rig.client->persistenceProblem()).arg(bindHeld).arg(refreshHeld));
    if (!waitUntil([&] { return bound; }, kWaitMs)) {
        harnessFailure("S15.252: the first bind never finished, so the second case cannot start");
        return;
    }

    // GOAL: S15.252 -- a refresh sent before a bind is superseded by the bind's newer read: answered
    //       after the bind's, with older state, it is discarded.
    // WHY-THIS-SHAPE: both reads are held and released one at a time, the bind's first; the verdict
    //       is read once a later call on the same connection has been answered, so the refresh's
    //       reply has been handled by then.
    // CONTROL: app/daemon/daemon_client.cpp, bindNext()'s `++m_generation;` inside that `if` deleted
    //       (control_batch, 2026-10-04) -> red, "S15.252: the bind read the failure, the earlier
    //       refresh the healthy state, and the client shows "" (both held 1, later call answered 1)".
    rig.client->refresh();
    bool boundAgain = false;
    rig.client->bindMonitor(&context, QStringLiteral("DP-6"), [&](std::expected<int, QString>) { boundAgain = true; });
    const bool bothHeld = waitUntil([&] { return rig.daemonFake->held.size() == 2; }, kWaitMs);
    rig.daemonFake->persistence = QStringLiteral("fail");
    rig.daemonFake->releaseAt(1);
    const bool bindDone = bothHeld && waitUntil([&] { return boundAgain; }, kWaitMs);
    rig.daemonFake->persistence = QStringLiteral("ok");
    rig.daemonFake->releaseAt(0);
    bool later = false;
    rig.client->setProperties(&context, 0, {{QStringLiteral("speed"), 1.0}}, [&](std::expected<void, QString>) { later = true; });
    const bool answered = bindDone && waitUntil([&] { return later; }, kWaitMs);
    expect(answered && rig.client->persistenceProblem() == failing,
           QStringLiteral("S15.252: the bind read the failure, the earlier refresh the healthy state, and the client shows \"%1\" (both held %2, later call answered %3)")
               .arg(rig.client->persistenceProblem()).arg(bothHeld).arg(answered));
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
}

void longConnectors() {
    Rig rig;
    if (!build(rig, "S15.253", QStringLiteral("longnames"))) {
        return;
    }
    // Byte lengths as QtDBus sends them, UTF-8: 256 and 257 ASCII bytes; 128 and 129 two-byte
    // characters, 256 and 258 bytes but only 128 and 129 characters.
    const QString ascii256 = QStringLiteral("DP-") + QString(253, u'x');
    const QString ascii257 = QStringLiteral("DP-") + QString(254, u'x');
    const QString wide256(128, QChar(0xe9));
    const QString wide258(129, QChar(0xe9));
    rig.kscreenFake->config = configOf({output({.name = ascii256, .id = 1}), output({.name = ascii257, .id = 2, .x = qlonglong(1920)}),
                                        output({.name = wide256, .id = 3, .x = qlonglong(3840)}),
                                        output({.name = wide258, .id = 4, .x = qlonglong(5760)})});
    rig.kscreen->refresh();
    if (!waitUntil([&] { return rig.kscreen->state() == KScreenClient::State::Ready && rig.kscreen->monitors().size() == 4; }, kWaitMs)) {
        harnessFailure("S15.253: KScreen never listed the four long connectors");
        return;
    }
    const QString refusal = QStringLiteral("WallpaperX cannot use this monitor: its connector name is longer than the 256 bytes the "
                                           "WallpaperX service accepts.");

    // GOAL: S15.253 -- Apply is blocked, with the reason, on a screen whose connector name is over
    //       256 bytes of UTF-8, and allowed at exactly 256; bytes are counted, not characters.
    // CONTROL: app/daemon/apply_controller.cpp, control_batch, 2026-10-04: outputRefusal()'s `>
    //       kMaxOutputNameBytes` made `>= kMaxOutputNameBytes` -> red, "S15.253: 256 ASCII bytes is
    //       blocked for "WallpaperX cannot use this monitor: ..."", the 128-character row and the
    //       256-byte pause change below; measured with `connector.size()` -> red, "S15.253: 129
    //       two-byte characters is blocked for """; blockedReason()'s `return outputRefusal(...)`
    //       made `return {}` -> red, the 257-byte and 129-character rows.
    struct Row {
        const char* what;
        QString connector;
        bool refused;
    };
    const Row rows[] = {{"256 ASCII bytes", ascii256, false},
                        {"257 ASCII bytes", ascii257, true},
                        {"128 two-byte characters", wide256, false},
                        {"129 two-byte characters", wide258, true}};
    for (const Row& row : rows) {
        const bool selected = rig.kscreen->select(row.connector);
        const QString reason = rig.applier->blockedReason();
        expect(selected && reason == (row.refused ? refusal : QString()),
               QStringLiteral("S15.253: %1 is blocked for \"%2\" (selected %3)").arg(QLatin1StringView(row.what), reason).arg(selected));
    }

    // GOAL: S15.253 -- a pause change on such a screen fails at once with the same sentence and
    //       sends nothing to the daemon; at exactly 256 bytes it is sent.
    // WHY-THIS-SHAPE: "nothing" is the whole call log less the drain marker, read once the marker
    //       has arrived, so a bind's GetStatus counts as much as SetMonitorPause.
    // CONTROL: app/daemon/apply_controller.cpp, the refusal block setPauseMode() now reaches through
    //       changeScreenSetting() deleted (control_batch, re-run 2026-10-11) -> red, "S15.253: a pause change
    //       on 257 bytes ...".
    rig.kscreen->select(ascii257);
    rig.daemonFake->calls.clear();
    rig.applier->setPauseMode(QStringLiteral("always_paused"));
    const bool drained = waitUntil([&] { return rig.applier->pauseState() != Progress::Working; }, kWaitMs) && rig.daemonDrained();
    QStringList sentForLong;
    for (const FakeCall& c : rig.daemonFake->calls) {
        if (c.member != u"Marker") {
            sentForLong.append(c.member);
        }
    }
    expect(drained && rig.applier->pauseState() == Progress::Failed && rig.applier->pauseMessage() == refusal && sentForLong.isEmpty(),
           QStringLiteral("S15.253: a pause change on 257 bytes ended %1 reading \"%2\" and sent [%3]")
               .arg(int(rig.applier->pauseState())).arg(rig.applier->pauseMessage(), joined(sentForLong)));
    rig.kscreen->select(ascii256);
    rig.applier->setPauseMode(QStringLiteral("always_paused"));
    const bool finished = waitUntil([&] { return rig.applier->pauseState() != Progress::Working; }, kWaitMs);
    const auto bind = rig.sent("SetMonitorOutput");
    expect(finished && rig.applier->pauseState() == Progress::Done && !bind.isEmpty() && bind.back().args.value(1) == ascii256,
           QStringLiteral("S15.253: a pause change on 256 bytes ended %1 (\"%2\") and bound %3 screen(s)")
               .arg(int(rig.applier->pauseState())).arg(rig.applier->pauseMessage()).arg(bind.size()));
}

// The monitor id and speed of every SetWallpaperProperties and SetWallpaperWithProperties the
// daemon stand-in received, in arrival order, as "Member id speed".
QStringList wallpaperCalls(const FakeDaemon& daemon) {
    QStringList out;
    for (const FakeCall& c : daemon.calls) {
        if (c.member == u"SetWallpaperProperties") {
            out.append(QStringLiteral("SetWallpaperProperties %1 %2").arg(c.args.value(0).toString(), c.args.value(1).toMap().value(QStringLiteral("speed")).toString()));
        } else if (c.member == u"SetWallpaperWithProperties") {
            out.append(QStringLiteral("SetWallpaperWithProperties %1 %2").arg(c.args.value(2).toString(), c.args.value(3).toMap().value(QStringLiteral("speed")).toString()));
        }
    }
    return out;
}

void liveAcrossApplies() {
    constexpr const char* goals = "S15.256-S15.258";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("relink"))) {
        return;
    }
    FakeDaemon& d = *rig.daemonFake;
    const auto edits = [&] { return rig.sent("SetWallpaperProperties"); };
    if (!rig.apply() || rig.applier->applyState() != Progress::Done || rig.applier->liveOutput() != u"DP-1") {
        harnessFailure(std::string(goals) + ": Ocean Waves could not be applied to DP-1 to start from: " +
                       rig.applier->applyMessage().toStdString());
        return;
    }
    const int dp1 = d.outputs.value(QStringLiteral("DP-1"), -1);

    // GOAL: S15.256 -- an Apply to
    //       HDMI-A-1, which shows nothing, that fails at the bind or at the daemon leaves HDMI-A-1 unlinked
    //       (live ""), and selecting DP-1 again links it from GetStatus: an edit then sends exactly one
    //       SetWallpaperProperties, to DP-1's id.
    // WHY-THIS-SHAPE: both failure points, because a failed Apply could still link the screen it aimed at.
    //       The stand-in reports each screen's file, so DP-1's link can only come back
    //       from GetStatus. The count is read once every reply has been handled.
    // CONTROL: control_batch: in linkSelected (app/daemon/apply_controller.cpp) the GetStatus
    //       relink made `return;` -> red, "S15.256: after an Apply to HDMI-A-1 refused at SetMonitorOutput
    //       (failed 1), live on "" and an edit on DP-1 sent [], not [0]" and the same at SetWallpaperWithProperties.
    struct Refusal {
        const char* at;
        double speed;
    };
    for (const Refusal r : {Refusal{"SetMonitorOutput", 0.5}, Refusal{"SetWallpaperWithProperties", 2.0}}) {
        rig.kscreen->select(QStringLiteral("HDMI-A-1"));
        d.refuse.insert(QString::fromLatin1(r.at), {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no")});
        const bool failed = rig.apply() && rig.applier->applyState() == Progress::Failed;
        d.refuse.clear();
        const QString live = rig.applier->liveOutput();
        rig.kscreen->select(QStringLiteral("DP-1"));
        const bool quiet = rig.settled();
        d.calls.clear();
        rig.library->setPropertyValue(QStringLiteral("speed"), r.speed);
        const bool after = rig.settled();
        QStringList sent;
        for (const FakeCall& c : edits()) {
            sent.append(c.args.value(0).toString());
        }
        expect(failed && quiet && after && live.isEmpty() && sent == QStringList{QString::number(dp1)},
               QStringLiteral("S15.256: after an Apply to HDMI-A-1 refused at %1 (failed %2), live on \"%3\" and an edit on DP-1 "
                              "sent [%4], not [%5]")
                   .arg(QLatin1StringView(r.at)).arg(failed).arg(live, joined(sent)).arg(dp1));
    }

    // GOAL: S15.257 -- an Apply to another screen that succeeds moves the link there: HDMI-A-1 is
    //       live, and an edit goes to its id only.
    // CONTROL: app/daemon/apply_controller.cpp, startLive() assigning m_live only when it is empty
    //       (control_batch, re-run 2026-10-15) -> red, "S15.257: after Ocean Waves was applied to HDMI-A-1
    //       (Ocean Waves is on HDMI-A-1.), live on "DP-1" and an edit sent to [0], not [1]".
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    const bool moved = rig.apply() && rig.applier->applyState() == Progress::Done;
    const int hdmi = d.outputs.value(QStringLiteral("HDMI-A-1"), -1);
    const bool quiet = rig.settled();
    d.calls.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 0.5);
    const bool after = rig.settled();
    QStringList sentTo;
    for (const FakeCall& c : edits()) {
        sentTo.append(c.args.value(0).toString());
    }
    expect(moved && quiet && after && hdmi >= 0 && hdmi != dp1 && rig.applier->liveOutput() == u"HDMI-A-1" &&
               sentTo == QStringList{QString::number(hdmi)},
           QStringLiteral("S15.257: after Ocean Waves was applied to HDMI-A-1 (%1), live on \"%2\" and an edit sent to [%3], not [%4]")
               .arg(rig.applier->applyMessage(), rig.applier->liveOutput(), joined(sentTo)).arg(hdmi));

    // GOAL: S15.258 -- an edit made while an Apply to the live screen is running ends with the
    //       latest value at the daemon, even though the edit reaches it before the Apply's older
    //       values: the last SetWallpaper* call it receives carries the edit.
    // WHY-THIS-SHAPE: the bind's GetStatus is held, so the Apply has not sent its values when the
    //       edit is made; the edit is seen to arrive (over the old link) before the release, so the
    //       Apply's SetWallpaperWithProperties, carrying the snapshot from before the edit, lands
    //       after it. Only a resend after the Apply lands puts the edit back on top.
    // CONTROL: app/daemon/apply_controller.cpp, startLive() recording m_library.propertyValues() as
    //       sent instead of the Apply's snapshot (control_batch, re-run 2026-10-15) -> red, "S15.258: the
    //       daemon received [SetWallpaperProperties 1 2, SetWallpaperWithProperties 1 0.5]; the last should
    //       carry speed 2 (bind held 1, edit first 1, landed 1)".
    d.calls.clear();
    d.hold.insert(QStringLiteral("GetStatus"));
    rig.applier->apply();
    const bool bindHeld = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool editFirst = waitUntil([&] { return !edits().isEmpty(); }, kWaitMs);
    d.hold.clear();
    d.release();
    const bool landed = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs) && rig.settled();
    const QStringList calls = wallpaperCalls(d);
    const auto applyCall = std::ranges::find_if(calls, [](const QString& c) { return c.startsWith(u"SetWallpaperWithProperties "); });
    const qsizetype applyAt = applyCall == calls.end() ? -1 : applyCall - calls.begin();
    expect(bindHeld && editFirst && landed && rig.applier->applyState() == Progress::Done && applyAt > 0 &&
               !calls.isEmpty() && calls.back().endsWith(QStringLiteral(" 2")),
           QStringLiteral("S15.258: the daemon received [%1]; the last should carry speed 2 (bind held %2, edit first %3, landed %4)")
               .arg(calls.join(QStringLiteral(", "))).arg(bindHeld).arg(editFirst).arg(landed));
}

// Every SetWallpaperProperties and SetWallpaperWithProperties the daemon stand-in received, as
// "Member id paused=<true|false|unset> speed=<speed>", sorted: which order two screens' sends leave in is
// not part of any contract here.
QStringList pausedCalls(const FakeDaemon& daemon) {
    QStringList out;
    for (const FakeCall& c : daemon.calls) {
        const bool replace = c.member == u"SetWallpaperProperties";
        if (!replace && c.member != u"SetWallpaperWithProperties") {
            continue;
        }
        const QVariantMap values = c.args.value(replace ? 1 : 3).toMap();
        const QVariant paused = values.value(QStringLiteral("paused"));
        out.append(QStringLiteral("%1 %2 paused=%3 speed=%4")
                       .arg(c.member, c.args.value(replace ? 0 : 2).toString(),
                            paused.isValid() ? (paused.toBool() ? QStringLiteral("true") : QStringLiteral("false")) : QStringLiteral("unset"),
                            values.value(QStringLiteral("speed")).toString()));
    }
    out.sort();
    return out;
}

void selectedScreenEdits() {
    constexpr const char* goals = "S15.994, S15.995, S15.1014, S15.1072-S15.1075, S15.1079, S15.1083";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("selected"))) {
        return;
    }
    FakeDaemon& d = *rig.daemonFake;
    const QString hdmiName = QStringLiteral("HDMI-A-1");
    const QString dpName = QStringLiteral("DP-1");
    const QString pausedKey = QStringLiteral("paused");
    const QString speedKey = QStringLiteral("speed");
    const auto shownPaused = [&] { return rig.library->propertyValues().value(pausedKey).toBool(); };
    const auto shownSpeed = [&] { return rig.library->propertyValues().value(speedKey).toDouble(); };
    // Ocean on DP-1 at 2x, then on HDMI-A-1 at 0.5x, each set as a draft before its Apply.
    rig.library->setPropertyValue(speedKey, 2.0);
    const bool onDp = rig.apply() && rig.applier->applyState() == Progress::Done;
    rig.kscreen->select(hdmiName);
    rig.library->setPropertyValue(speedKey, 0.5);
    const bool onHdmi = rig.apply() && rig.applier->applyState() == Progress::Done;
    const int dp1 = d.outputs.value(dpName, -1);
    const int hdmi = d.outputs.value(hdmiName, -1);
    if (!onDp || !onHdmi || dp1 < 0 || hdmi < 0 || !rig.reread() || rig.client->status().wallpapers.size() != 2) {
        harnessFailure(std::string(goals) + ": Ocean Waves could not be put on both screens to start from: " +
                       rig.applier->applyMessage().toStdString());
        return;
    }
    const QString dp = QString::number(dp1);
    const QString hd = QString::number(hdmi);

    // GOAL: S15.994 -- selecting a screen that shows the open wallpaper links it and shows its own values,
    //       sending nothing; an edit then goes to that screen's id only, and another screen keeps its value.
    // WHY-THIS-SHAPE: the two screens hold different speeds, so showing the other screen's value, or the
    //       last Apply's, is red; "nothing sent" is read once every reply has been handled.
    // CONTROL: control_batch, app/daemon/apply_controller.cpp linkSelected: the GetStatus relink made
    //       `return;` -> red, "S15.994: selecting DP-1 linked "" ..."; adoptPropertyValues(...) deleted -> red,
    //       "S15.994: selecting DP-1 linked "DP-1", showed speed 0.5".
    d.calls.clear();
    rig.kscreen->select(dpName);
    bool quiet = rig.settled();
    expect(quiet && rig.applier->liveOutput() == dpName && shownSpeed() == 2.0 && pausedCalls(d).isEmpty(),
           QStringLiteral("S15.994: selecting DP-1 linked \"%1\", showed speed %2 and sent [%3]; expected DP-1, 2, []")
               .arg(rig.applier->liveOutput()).arg(shownSpeed()).arg(joined(pausedCalls(d))));
    rig.kscreen->select(hdmiName);
    quiet = rig.settled();
    expect(quiet && rig.applier->liveOutput() == hdmiName && shownSpeed() == 0.5 && pausedCalls(d).isEmpty(),
           QStringLiteral("S15.994: selecting HDMI-A-1 linked \"%1\", showed speed %2 and sent [%3]; expected HDMI-A-1, 0.5, []")
               .arg(rig.applier->liveOutput()).arg(shownSpeed()).arg(joined(pausedCalls(d))));
    rig.library->setPropertyValue(speedKey, 1.0);
    bool afterEdit = rig.settled();
    const QStringList toHdmi = pausedCalls(d);
    rig.kscreen->select(dpName);
    afterEdit = rig.settled() && afterEdit;
    expect(afterEdit && toHdmi == QStringList{QStringLiteral("SetWallpaperProperties %1 paused=unset speed=1").arg(hd)} && shownSpeed() == 2.0,
           QStringLiteral("S15.994: a speed edit on HDMI-A-1 sent [%1], and DP-1 then shows speed %2; expected one call to %3, and 2")
               .arg(joined(toHdmi)).arg(shownSpeed()).arg(hd));

    // GOAL: S15.1072 (D12-a) -- with the open video on both screens, Paused on while DP-1 is selected sends one
    //       SetWallpaperProperties to DP-1's id only, carrying DP-1's values; HDMI-A-1 is sent nothing and stays playing.
    // WHY-THIS-SHAPE: the screens hold different speeds, so a send built from the other screen's values is red;
    //       HDMI-A-1's paused is read from the stand-in, where a send to it would have landed.
    // CONTROL: app/daemon/apply_controller.cpp sendLive()'s `if (values == m_live->sent)` made `if (true)` (control_batch
    //       T6a, reverted) -> red, "D12-a: Paused on with DP-1 selected sent [] ...".
    const QString other = QStringLiteral("/elsewhere/other.mp4");
    d.calls.clear();
    rig.library->setPropertyValue(pausedKey, true);
    bool sent = rig.settled();
    const auto hdmiPaused = [&] { return d.screens.value(hdmi).properties.value(pausedKey).toBool(); };
    expect(sent && pausedCalls(d) == QStringList{QStringLiteral("SetWallpaperProperties %1 paused=true speed=2").arg(dp)} && !hdmiPaused() &&
               shownPaused(),
           QStringLiteral("D12-a: Paused on with DP-1 selected sent [%1], left HDMI-A-1 paused %2 and shows %3; expected only "
                          "[SetWallpaperProperties %4 paused=true speed=2], false and true")
               .arg(joined(pausedCalls(d))).arg(hdmiPaused()).arg(shownPaused()).arg(dp));

    // GOAL: S15.1073 (D12-b) -- with DP-1 paused and HDMI-A-1 playing, selecting each shows that screen's own Paused and speed,
    //       and sends nothing.
    // CONTROL: app/daemon/apply_controller.cpp linkSelected()'s `m_library.adoptPropertyValues(status.wallpapers.value(*id).properties);`
    //       deleted (control_batch T6a, reverted) -> red, "D12-b: selecting HDMI-A-1 showed paused 1, speed 1 ..."
    //       (S15.994 goes red under the same break).
    d.calls.clear();
    rig.kscreen->select(hdmiName);
    quiet = rig.settled();
    const bool hdmiOwn = !shownPaused() && shownSpeed() == 1.0 && rig.applier->liveOutput() == hdmiName;
    const QString hdmiSeen = QStringLiteral("paused %1, speed %2").arg(shownPaused()).arg(shownSpeed());
    rig.kscreen->select(dpName);
    quiet = rig.settled() && quiet;
    const bool dpOwn = shownPaused() && shownSpeed() == 2.0 && rig.applier->liveOutput() == dpName;
    expect(quiet && hdmiOwn && dpOwn && pausedCalls(d).isEmpty(),
           QStringLiteral("D12-b: selecting HDMI-A-1 showed %1, then DP-1 paused %2, speed %3, sending [%4]; expected HDMI-A-1's "
                          "false and 1, DP-1's true and 2, and []").arg(hdmiSeen).arg(shownPaused()).arg(shownSpeed()).arg(joined(pausedCalls(d))));
    rig.library->setPropertyValue(pausedKey, false);
    quiet = rig.settled();

    // GOAL: S15.1014 -- while a screen is linked the switch shows that screen's own Paused value, not another
    //       screen's, and an unrelated live edit carries that value.
    // WHY-THIS-SHAPE: DP-1 (linked) plays while HDMI-A-1 is paused behind the app; the edit is graded on the wire,
    //       where a false record shows.
    // CONTROL: app/daemon/apply_controller.cpp linkSelected()'s already-linked `return;` preceded by an adopt of
    //       HDMI-A-1's GetStatus properties (control_batch T6a, reverted) -> red, "S15.1014: with DP-1 linked and playing,
    //       HDMI-A-1 paused, the switch shows 1 ...".
    d.screens[hdmi].properties.insert(pausedKey, true);
    bool read = rig.reread();
    const bool linkedOwn = !shownPaused();
    d.calls.clear();
    rig.library->setPropertyValue(speedKey, 1.0);
    bool after = rig.settled();
    expect(quiet && read && linkedOwn,
           QStringLiteral("S15.1014: with DP-1 linked and playing, HDMI-A-1 paused, the switch shows %1; expected DP-1's own false").arg(!linkedOwn));
    expect(read && after && pausedCalls(d) == QStringList{QStringLiteral("SetWallpaperProperties %1 paused=false speed=1").arg(dp)},
           QStringLiteral("S15.1014: a speed edit on playing DP-1, HDMI-A-1 paused, sent [%1]; expected paused=false with it").arg(joined(pausedCalls(d))));

    // GOAL: S15.1079 (D12-h) -- Reset to defaults clears Paused, and the resulting send goes to the linked screen only.
    // WHY-THIS-SHAPE: DP-1 is paused first, so a Reset that kept Paused shows true and sends paused=true;
    //       HDMI-A-1 is paused behind the app, so a send reaching it would turn it off.
    // CONTROL: app/library/library_manager.cpp resetPropertyValues() made to keep `paused` in m_edits (control_batch
    //       T6a, reverted) -> red, "D12-h: after Reset the switch shows 1 and sent [... paused=true ...]".
    rig.library->setPropertyValue(pausedKey, true);
    quiet = rig.settled();
    d.calls.clear();
    rig.library->resetPropertyValues();
    after = rig.settled();
    const QStringList resetSent = pausedCalls(d);
    expect(quiet && after && !shownPaused() && resetSent.size() == 1 &&
               resetSent.value(0).startsWith(QStringLiteral("SetWallpaperProperties %1 ").arg(dp)) && !resetSent.value(0).contains(u"paused=true") &&
               hdmiPaused(),
           QStringLiteral("D12-h: after Reset the switch shows %1 and sent [%2], HDMI-A-1 paused %3; expected false, one call to %4 "
                          "without paused=true, and HDMI-A-1 still paused").arg(shownPaused()).arg(joined(resetSent)).arg(hdmiPaused()).arg(dp));

    // GOAL: S15.1083 (D12) -- a refused Paused change shows as the live error, naming the screen.
    // CONTROL: app/daemon/apply_controller.cpp sendTo()'s error made `QString()` whatever the reply (control_batch T6a,
    //       reverted) -> red, "D12: a refused Paused on left the live error \"\" ...".
    d.refuse.insert(QStringLiteral("SetWallpaperProperties"), {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no")});
    rig.library->setPropertyValue(pausedKey, true);
    after = rig.settled();
    d.refuse.clear();
    const QString refusal = rig.applier->liveError();
    expect(after && refusal.contains(dpName),
           QStringLiteral("D12: a refused Paused on left the live error \"%1\"; expected one naming DP-1").arg(refusal));
    rig.library->setPropertyValue(pausedKey, false);
    quiet = rig.settled();

    // GOAL: S15.995 -- with no screen showing the open video, Paused is a draft: nothing is sent, and the next
    //       Apply carries it.
    // CONTROL: app/daemon/apply_controller.cpp apply()'s `const QVariantMap values = m_library.propertyValues();` made
    //       an empty map (control_batch T6a, reverted) -> red, "S15.995: Paused on River, shown nowhere, sent [] and the Apply sent
    //       [SetWallpaperProperties 0 paused=true speed=1,SetWallpaperWithProperties 0 paused=unset speed=]".
    if (!rig.library->select(rig.river)) {
        harnessFailure(std::string(goals) + ": River could not be opened");
        return;
    }
    quiet = rig.settled() && quiet;
    d.calls.clear();
    rig.library->setPropertyValue(pausedKey, true);
    after = rig.settled();
    const QStringList draftSent = pausedCalls(d);
    const bool applied = rig.apply() && rig.applier->applyState() == Progress::Done && rig.settled();
    const QStringList applySent = pausedCalls(d);
    expect(quiet && after && applied && draftSent.isEmpty() && applySent.size() == 1 &&
               applySent.value(0).startsWith(QStringLiteral("SetWallpaperWithProperties %1 paused=true").arg(dp)),
           QStringLiteral("S15.995: Paused on River, shown nowhere, sent [%1] and the Apply sent [%2]; expected [] and paused=true to %3")
               .arg(joined(draftSent), joined(applySent), dp));

    // GOAL: S15.1074 (D12-c) -- a screen that does not show the open wallpaper shows its declared defaults, coming from a
    //       linked screen with other values and from another unlinked screen with a draft; nothing is sent.
    // WHY-THIS-SHAPE: River is linked on DP-1 paused, and HDMI-A-1 then holds a speed-2 draft, so keeping either
    //       screen's values is red; the defaults are River's own declared ones, compared as a whole map.
    // CONTROL: app/daemon/apply_controller.cpp linkSelected()'s `fresh` made always false (control_batch T6a, reverted)
    //       -> red, "D12-c: HDMI-A-1, coming from linked DP-1, showed {... paused=true ...}".
    const QVariantMap defaults = rig.library->selectedWallpaper().defaultValues();
    const auto shown = [&] { return rig.library->propertyValues(); };
    d.calls.clear();
    rig.kscreen->select(hdmiName);
    quiet = rig.settled();
    const QVariantMap fromLinked = shown();
    rig.library->setPropertyValue(speedKey, 2.0);
    d.screens[dp1].path = other;
    read = rig.reread();
    rig.kscreen->select(dpName);
    quiet = rig.settled() && quiet;
    const QVariantMap fromDraft = shown();
    expect(applied && quiet && read && fromLinked == defaults && fromDraft == defaults && rig.applier->liveOutput().isEmpty() && pausedCalls(d).isEmpty(),
           QStringLiteral("D12-c: HDMI-A-1, coming from linked DP-1, showed %1; DP-1, showing another file, coming from HDMI-A-1's "
                          "draft, showed %2 (live \"%3\", sent [%4]); expected the defaults %5 both times and nothing sent")
               .arg(describeMap(fromLinked), describeMap(fromDraft), rig.applier->liveOutput(), joined(pausedCalls(d)), describeMap(defaults)));

    // GOAL: S15.1075 (D12-d; the KScreen-rename case is not graded here) -- a draft on an unlinked selected screen survives status reads and a failed Apply, and the next
    //       Apply sends it; leaving the screen and coming back shows the defaults.
    // CONTROL: app/daemon/apply_controller.cpp linkSelected()'s `fresh` made always true (control_batch T6a, reverted)
    //       -> red, "D12-d: the draft speed 0.5 on DP-1 became 1 ...".
    rig.library->setPropertyValue(speedKey, 0.5);
    read = rig.reread() && rig.reread();
    d.refuse.insert(QStringLiteral("SetWallpaperWithProperties"), {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no")});
    const bool refused = rig.apply() && rig.applier->applyState() == Progress::Failed;
    d.refuse.clear();
    read = rig.settled() && rig.reread() && read;
    const double kept = shownSpeed();
    d.calls.clear();
    const bool draftApplied = rig.apply() && rig.applier->applyState() == Progress::Done && rig.settled();
    const auto applies = rig.sent("SetWallpaperWithProperties");
    const double appliedSpeed = applies.isEmpty() ? -1 : applies.back().args.value(3).toMap().value(speedKey).toDouble();
    expect(read && refused && draftApplied && kept == 0.5 && applies.size() == 1 && appliedSpeed == 0.5,
           QStringLiteral("D12-d: the draft speed 0.5 on DP-1 became %1 after reads and a failed Apply (refused %2, read %3), "
                          "and the next Apply sent speed %4 in %5 call(s); expected 0.5 and 0.5 in one")
               .arg(kept).arg(refused).arg(read).arg(appliedSpeed).arg(applies.size()));
    d.screens[dp1].path = other;
    read = rig.reread() && rig.applier->liveOutput().isEmpty();
    rig.library->setPropertyValue(speedKey, 2.0);
    rig.kscreen->select(hdmiName);
    quiet = rig.settled();
    rig.kscreen->select(dpName);
    quiet = rig.settled() && quiet;
    expect(read && quiet && shown() == defaults,
           QStringLiteral("D12-d: leaving DP-1's draft and coming back showed %1 (unlinked %2); expected the defaults %3")
               .arg(describeMap(shown())).arg(read).arg(describeMap(defaults)));
}

// Puts Ocean Waves on DP-1 at 2x and HDMI-A-1 at 0.5x, then selects DP-1 and links it; false on a harness fault.
bool oceanOnBoth(Rig& rig, int& dp1, int& hdmi) {
    const QString speedKey = QStringLiteral("speed");
    rig.library->setPropertyValue(speedKey, 2.0);
    const bool onDp = rig.apply() && rig.applier->applyState() == Progress::Done;
    rig.kscreen->select(QStringLiteral("HDMI-A-1"));
    rig.library->setPropertyValue(speedKey, 0.5);
    const bool onHdmi = rig.apply() && rig.applier->applyState() == Progress::Done;
    rig.kscreen->select(QStringLiteral("DP-1"));
    dp1 = rig.daemonFake->outputs.value(QStringLiteral("DP-1"), -1);
    hdmi = rig.daemonFake->outputs.value(QStringLiteral("HDMI-A-1"), -1);
    return onDp && onHdmi && dp1 >= 0 && hdmi >= 0 && rig.reread() && rig.applier->liveOutput() == u"DP-1";
}

// Edits queued behind a send, and links held back while a send or this app's own Apply runs.
void linkHandOver() {
    constexpr const char* goals = "S15.1015, S15.1082";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("handover"))) {
        return;
    }
    FakeDaemon& d = *rig.daemonFake;
    const QString hdmiName = QStringLiteral("HDMI-A-1");
    const QString speedKey = QStringLiteral("speed");
    const QString other = QStringLiteral("/elsewhere/other.mp4");
    const auto shownSpeed = [&] { return rig.library->propertyValues().value(speedKey).toDouble(); };
    const QString volumeKey = QStringLiteral("volume");
    const auto live = [&] { return rig.applier->liveOutput(); };
    const auto shownVolume = [&] { return rig.library->propertyValues().value(volumeKey).toDouble(); };
    // Every SetWallpaper* call since the log was cleared, as "Member id volume=v", sorted.
    const auto volumeCalls = [&] {
        QStringList out;
        for (const FakeCall& c : d.calls) {
            const bool replace = c.member == u"SetWallpaperProperties";
            if (replace || c.member == u"SetWallpaperWithProperties") {
                out.append(QStringLiteral("%1 %2 volume=%3").arg(c.member, c.args.value(replace ? 0 : 2).toString(),
                                                                c.args.value(replace ? 1 : 3).toMap().value(volumeKey).toString()));
            }
        }
        out.sort();
        return out;
    };
    int dp1 = -1;
    int hdmi = -1;
    if (!oceanOnBoth(rig, dp1, hdmi)) {
        harnessFailure(std::string(goals) + ": Ocean Waves could not be put on both screens with DP-1 linked to start from");
        return;
    }
    const QString dp = QString::number(dp1);
    const QString hd = QString::number(hdmi);

    // GOAL: S15.1082 (D11 c1-3) -- with a send to DP-1 in flight, selecting HDMI-A-1 (showing Ocean) links it at once with its own
    //       values, and an edit made then reaches HDMI-A-1 and survives the following read. S15.1015 -- the screen
    //       left still gets the last value set there.
    // WHY-THIS-SHAPE: SetWallpaperProperties is held, so the second DP-1 edit provably queues, and the selection and
    //       the HDMI-A-1 edit provably happen while DP-1's send is in flight; the screen left is graded on the wire.
    // CONTROL: app/daemon/apply_controller.cpp, control_batch T6a, each reverted: sendTo()'s reply
    //       `else if (next && next->values != next->live.sent)` made `else if (false)` -> red, "S15.1015: edits on DP-1 then
    //       HDMI-A-1 ... sent [...]"; linkSelected()'s `m_sending.constFind(*monitor)` made to find any screen's send
    //       -> red, "c1-3: with a send to DP-1 in flight, selecting HDMI-A-1 linked \"\" ...".
    d.calls.clear();
    d.hold.insert(QStringLiteral("SetWallpaperProperties"));
    rig.library->setPropertyValue(volumeKey, 0.25);
    const bool firstHeld = waitUntil([&] { return d.held.size() == 1; }, kWaitMs);
    rig.library->setPropertyValue(volumeKey, 0.75);
    rig.kscreen->select(hdmiName);
    const QString liveDuring = live();
    const double speedDuring = shownSpeed();
    rig.library->setPropertyValue(volumeKey, 0.6);
    const bool drained = rig.daemonDrained();
    const qsizetype heldDuring = d.held.size();
    d.hold.clear();
    d.release();
    const bool linked = rig.settled() && rig.reread();
    expect(firstHeld && drained && liveDuring == hdmiName && speedDuring == 0.5 && heldDuring == 2,
           QStringLiteral("c1-3: with a send to DP-1 in flight, selecting HDMI-A-1 linked \"%1\" showing speed %2 (held %3, first held %4); "
                          "expected HDMI-A-1 at once, 0.5, and its edit sent beside DP-1's (2 held)")
               .arg(liveDuring).arg(speedDuring).arg(heldDuring).arg(firstHeld));
    QStringList leftWith{QStringLiteral("SetWallpaperProperties %1 volume=0.25").arg(dp), QStringLiteral("SetWallpaperProperties %1 volume=0.75").arg(dp),
                         QStringLiteral("SetWallpaperProperties %1 volume=0.6").arg(hd)};
    leftWith.sort();
    expect(linked && volumeCalls() == leftWith && live() == hdmiName && shownVolume() == 0.6,
           QStringLiteral("S15.1015: edits on DP-1 then HDMI-A-1, around a held send, sent [%1], left live on \"%2\" showing volume %3; "
                          "expected [%4], HDMI-A-1 and 0.6").arg(joined(volumeCalls()), live()).arg(shownVolume()).arg(joined(leftWith)));

    // GOAL: S15.1015 -- an edit queued behind a send is dropped when the link ends because the screen shows another
    //       wallpaper: it would land on that wallpaper.
    // CONTROL: app/daemon/apply_controller.cpp followStatus()'s `inFlight.next.reset();` deleted (control_batch
    //       T6a, reverted) -> red, "S15.1015: with HDMI-A-1 found showing another file while an edit was queued, it was sent
    //       [SetWallpaperProperties 1 volume=0.3,SetWallpaperProperties 1 volume=0.4] (live \"\"); expected only the first edit".
    d.calls.clear();
    d.hold.insert(QStringLiteral("SetWallpaperProperties"));
    rig.library->setPropertyValue(volumeKey, 0.3);
    const bool secondHeld = waitUntil([&] { return d.held.size() == 1; }, kWaitMs);
    rig.library->setPropertyValue(volumeKey, 0.4);
    d.screens[hdmi].path = other;
    const bool otherRead = rig.readLanded();
    const QString liveOther = live();
    d.hold.clear();
    d.release();
    const bool quiet = rig.settled();
    expect(secondHeld && otherRead && quiet && liveOther.isEmpty() &&
               volumeCalls() == QStringList{QStringLiteral("SetWallpaperProperties %1 volume=0.3").arg(hd)},
           QStringLiteral("S15.1015: with HDMI-A-1 found showing another file while an edit was queued, it was sent [%1] "
                          "(live \"%2\"); expected only the first edit").arg(joined(volumeCalls()), liveOther));

    // GOAL: S15.1015 -- no link forms from a read during this app's own Apply of the open wallpaper to the selected
    //       screen, so the edits made meanwhile are not replaced, and they reach the screen once the Apply lands.
    //       D12-e -- HDMI-A-1's link ended above, so the Apply carries the defaults (volume 1), not 0.4.
    // WHY-THIS-SHAPE: the Apply's SetWallpaperWithProperties is held, and the stand-in shows Ocean on HDMI-A-1 from
    //       its arrival, so the read provably shows the wallpaper there while the Apply runs.
    // CONTROL: app/daemon/apply_controller.cpp linkSelected()'s `if (applying) {` made `if (false) {` (control_batch T6a,
    //       reverted) -> red, "S15.1015: a read during this app's Apply of Ocean to HDMI-A-1 (held 1, read 1) linked \"HDMI-A-1\" ...".
    d.calls.clear();
    d.hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    rig.applier->apply();
    const bool applyHeld = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    rig.library->setPropertyValue(volumeKey, 0.9);
    const bool readDuring = rig.readLanded();
    const QString liveApplying = live();
    const double volumeApplying = shownVolume();
    d.hold.clear();
    d.release();
    const bool landed = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs) && rig.settled();
    expect(applyHeld && readDuring && liveApplying.isEmpty() && volumeApplying == 0.9,
           QStringLiteral("S15.1015: a read during this app's Apply of Ocean to HDMI-A-1 (held %1, read %2) linked \"%3\" and showed "
                          "volume %4; expected \"\" and the 0.9 set meanwhile").arg(applyHeld).arg(readDuring).arg(liveApplying).arg(volumeApplying));
    const QStringList applied{QStringLiteral("SetWallpaperProperties %1 volume=0.9").arg(hd),
                              QStringLiteral("SetWallpaperWithProperties %1 volume=1").arg(hd)};
    expect(landed && live() == hdmiName && volumeCalls() == applied,
           QStringLiteral("S15.1015: the Apply to HDMI-A-1 with an edit made meanwhile sent [%1] and left live on \"%2\"; expected [%3]")
               .arg(joined(volumeCalls()), live(), joined(applied)));

    // GOAL: S15.1015 -- after a failed Apply, a selected screen that already shows the open wallpaper links.
    // WHY-THIS-SHAPE: refuseAt answers the held call after the stand-in applied it, so the read taken during the
    //       Apply shows Ocean on HDMI-A-1; no read follows the failure, so only the Apply's own end can link.
    // CONTROL: app/daemon/apply_controller.cpp settleApply()'s `linkSelected();` deleted (control_batch
    //       T6a, reverted) -> red, "S15.1015: after a failed Apply to HDMI-A-1, which shows Ocean (unlinked first 1, held 1, read 1), it ended 3
    //       with live \"\"; expected Failed and HDMI-A-1".
    d.screens[hdmi].path = other;
    const bool unlinked = rig.reread() && live().isEmpty();
    d.hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    rig.applier->apply();
    const bool failHeld = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    const bool readBefore = rig.readLanded() && live().isEmpty();
    d.hold.clear();
    d.refuseAt(0, QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no"));
    const bool failed = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs) && rig.daemonDrained();
    expect(unlinked && failHeld && readBefore && failed && rig.applier->applyState() == Progress::Failed && live() == hdmiName,
           QStringLiteral("S15.1015: after a failed Apply to HDMI-A-1, which shows Ocean (unlinked first %1, held %2, read %3), it ended "
                          "%4 with live \"%5\"; expected Failed and HDMI-A-1").arg(unlinked).arg(failHeld).arg(readBefore)
               .arg(int(rig.applier->applyState())).arg(live()));
}

// Coming back to a screen while this app's Apply to it runs, and an Apply landing while another screen is selected.
void applyReturn() {
    constexpr const char* goals = "S15.1077, S15.1078, S15.1081";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("applyreturn"))) {
        return;
    }
    FakeDaemon& d = *rig.daemonFake;
    const QString dpName = QStringLiteral("DP-1");
    const QString hdmiName = QStringLiteral("HDMI-A-1");
    const QString speedKey = QStringLiteral("speed");
    const auto shownSpeed = [&] { return rig.library->propertyValues().value(speedKey).toDouble(); };
    const auto live = [&] { return rig.applier->liveOutput(); };
    int dp1 = -1;
    int hdmi = -1;
    if (!oceanOnBoth(rig, dp1, hdmi)) {
        harnessFailure(std::string(goals) + ": Ocean Waves could not be put on both screens with DP-1 linked to start from");
        return;
    }
    const QString dp = QString::number(dp1);
    const QString hd = QString::number(hdmi);

    // GOAL: S15.1077 (D12-f) -- during this app's Apply of the open wallpaper to DP-1, selecting HDMI-A-1 and then
    //       DP-1 again shows the values the Apply sent, and an edit made then reaches DP-1 once the Apply lands.
    // WHY-THIS-SHAPE: the Apply's SetWallpaperWithProperties is held, so both selections provably land while it runs;
    //       the Apply sends DP-1's speed 2 and HDMI-A-1 holds 0.5, so carrying HDMI-A-1's values back is red, as is the defaults' speed 1.
    // CONTROL: app/daemon/apply_controller.cpp linkSelected()'s `draft(m_applyValues);` made `draft();` (control_batch
    //       T6a, reverted) -> red, "S15.1077: back on DP-1 during its Apply the controls showed speed 1 ...".
    d.calls.clear();
    d.hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    rig.applier->apply();
    const bool held = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    rig.kscreen->select(hdmiName);
    const bool away = rig.daemonDrained() && live() == hdmiName && shownSpeed() == 0.5;
    rig.kscreen->select(dpName);
    const double back = shownSpeed();
    rig.library->setPropertyValue(speedKey, 1.0);
    d.hold.clear();
    d.release();
    const bool landed = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs) && rig.settled();
    const QStringList sent = pausedCalls(d);
    const QString edit = QStringLiteral("SetWallpaperProperties %1 paused=unset speed=1").arg(dp);
    expect(held && away && landed && back == 2.0 && sent.contains(edit) && live() == dpName && shownSpeed() == 1.0,
           QStringLiteral("S15.1077: back on DP-1 during its Apply the controls showed speed %1 (held %2, HDMI-A-1 linked at 0.5 %3); "
                          "after it landed it sent [%4], live \"%5\"; expected 2, then [%6] among them and DP-1")
               .arg(back).arg(held).arg(away).arg(joined(sent), live(), edit));

    // GOAL: S15.1078 (D12-g) -- an Apply that lands while another screen is selected neither links nor changes the
    //       controls, and the link the selected screen holds is kept.
    // WHY-THIS-SHAPE: the Apply to DP-1 is held while HDMI-A-1 is selected and edited, so it provably lands after;
    //       the edit is sent first, so a landing that relinked would leave live on DP-1.
    // CONTROL: app/daemon/apply_controller.cpp startLive()'s selection test made `if (false)` (control_batch T6a,
    //       reverted) -> red, "S15.1078: an Apply to DP-1 landing with HDMI-A-1 selected ... sent [...SetWallpaperProperties 0 ...".
    //       The end state alone cannot show it: the Apply's own end relinks the selected screen.
    d.calls.clear();
    d.hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    rig.applier->apply();
    const bool heldAgain = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    rig.kscreen->select(hdmiName);
    rig.library->setPropertyValue(speedKey, 2.0);
    const bool edited = rig.daemonDrained();
    d.hold.clear();
    d.release();
    const bool landedAway = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs) && rig.settled();
    const bool noneToDp = std::ranges::none_of(pausedCalls(d), [&](const QString& c) { return c.startsWith(QStringLiteral("SetWallpaperProperties %1 ").arg(dp)); });
    expect(heldAgain && edited && landedAway && live() == hdmiName && shownSpeed() == 2.0 && noneToDp &&
               pausedCalls(d).contains(QStringLiteral("SetWallpaperProperties %1 paused=unset speed=2").arg(hd)),
           QStringLiteral("S15.1078: an Apply to DP-1 landing with HDMI-A-1 selected left live on \"%1\" showing speed %2 and sent [%3] "
                          "(held %4); expected HDMI-A-1, 2 and only its edit to %5, nothing to DP-1").arg(live()).arg(shownSpeed()).arg(joined(pausedCalls(d)))
               .arg(heldAgain).arg(hd));

    // GOAL: S15.1081 (D11 c1-2) -- values queued on a link that has ended are not sent once an Apply to that screen
    //       has started: they would land on the wallpaper the Apply put there.
    // WHY-THIS-SHAPE: the first edit's send is held so the second provably queues behind it; opening River ends the
    //       link while HDMI-A-1 still shows Ocean, so without the Apply the queue would rightly be sent (r1-4). River's
    //       SetWallpaperWithProperties stays held while the edit's reply is released alone, so no status read showing
    //       River can drop the queue instead (that is followStatus's guard, graded in linkHandOver).
    // CONTROL: app/daemon/apply_controller.cpp apply()'s `inFlight.next.reset();` at the Apply's start deleted
    //       (control_batch T6a; re-run T7a beside the later drop at success, reverted) -> red, "S15.1081: an edit queued
    //       on HDMI-A-1 before River's Apply there sent [...speed=0.5,...speed=1,SetWallpaperWithProperties 1 ...]".
    d.calls.clear();
    d.hold = {QStringLiteral("SetWallpaperProperties"), QStringLiteral("SetWallpaperWithProperties")};
    rig.library->setPropertyValue(speedKey, 0.5);
    const bool firstHeld = waitUntil([&] { return d.held.size() == 1; }, kWaitMs);
    rig.library->setPropertyValue(speedKey, 1.0);
    const bool river = rig.library->select(rig.river);
    rig.applier->apply();
    const bool riverSent = waitUntil([&] { return d.held.size() == 2; }, kWaitMs) && rig.daemonDrained();
    d.releaseAt(0);
    const bool editReplied = rig.repliesHandled() && rig.daemonDrained();
    const QStringList afterRiver = pausedCalls(d);
    d.hold.clear();
    d.release();
    const bool riverDone = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs) && rig.settled() && editReplied;
    const auto propertySends = std::ranges::count_if(afterRiver, [](const QString& c) { return c.startsWith(u"SetWallpaperProperties "); });
    expect(firstHeld && river && riverSent && riverDone && propertySends == 1,
           QStringLiteral("S15.1081: an edit queued on HDMI-A-1 before River's Apply there sent [%1] (held %2, River opened %3, "
                          "applied %4); expected only the first SetWallpaperProperties").arg(joined(afterRiver)).arg(firstHeld).arg(river).arg(riverDone));
}

// Re-selecting a screen or a wallpaper while a send or this app's Apply is still in flight.
void applyReopen() {
    constexpr const char* goals = "S15.1130-S15.1133";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("applyreopen"))) {
        return;
    }
    FakeDaemon& d = *rig.daemonFake;
    const QString dpName = QStringLiteral("DP-1");
    const QString hdmiName = QStringLiteral("HDMI-A-1");
    const QString speedKey = QStringLiteral("speed");
    const QString withProps = QStringLiteral("SetWallpaperWithProperties");
    const QString props = QStringLiteral("SetWallpaperProperties");
    const auto shownSpeed = [&] { return rig.library->propertyValues().value(speedKey).toDouble(); };
    const auto live = [&] { return rig.applier->liveOutput(); };
    const auto applied = [&] { return waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs) && rig.settled(); };
    int dp1 = -1;
    int hdmi = -1;
    if (!oceanOnBoth(rig, dp1, hdmi)) {
        harnessFailure(std::string(goals) + ": Ocean Waves could not be put on both screens with DP-1 linked to start from");
        return;
    }
    // Every SetWallpaper* call to DP-1 logged from index `from` on, in arrival order, as "Member speed".
    const auto toDp = [&](qsizetype from) {
        QStringList out;
        for (qsizetype i = from; i < d.calls.size(); ++i) {
            const FakeCall& c = d.calls.at(i);
            const bool replace = c.member == props;
            if ((replace || c.member == withProps) && c.args.value(replace ? 0 : 2).toInt() == dp1) {
                out.append(c.member + u' ' + c.args.value(replace ? 1 : 3).toMap().value(speedKey).toString());
            }
        }
        return out;
    };

    // GOAL: S15.1133 (D11 c1-3, second sentence) -- re-selecting a screen whose send for the open wallpaper is
    //       still in flight shows the values that send sets, not what the last status read holds there.
    // WHY-THIS-SHAPE: the send is held, so GetStatus provably still says speed 2 on DP-1 while it carries 0.5; going
    //       via HDMI-A-1 makes the return a real relink, which an unchanged selection would not be.
    // CONTROL: app/daemon/apply_controller.cpp linkSelected()'s in-flight branch adopting the status's properties
    //       instead of `latest.values` (control_batch T7a, reverted) -> red, "S15.1133: back on DP-1 ... showed speed 2 ...".
    d.calls.clear();
    d.hold.insert(props);
    rig.library->setPropertyValue(speedKey, 0.5);
    const bool sendHeld = waitUntil([&] { return d.held.size() == 1; }, kWaitMs);
    rig.kscreen->select(hdmiName);
    const bool away = rig.daemonDrained() && live() == hdmiName;
    rig.kscreen->select(dpName);
    const double back = shownSpeed();
    const QString backLive = live();
    d.hold.clear();
    d.release();
    const bool sendDone = rig.settled();
    expect(sendHeld && away && sendDone && back == 0.5 && backLive == dpName,
           QStringLiteral("S15.1133: back on DP-1 while its send of speed 0.5 was in flight the controls showed speed %1 linked \"%2\" "
                          "(held %3, away %4, done %5); expected 0.5 and DP-1").arg(back).arg(backLive).arg(sendHeld).arg(away).arg(sendDone));

    // GOAL: S15.1130 (D13-e) -- during this app's Apply of Ocean to DP-1 at a non-default speed, opening River and
    //       then Ocean again shows the Apply's values, and nothing sends the defaults to DP-1 once the Apply lands.
    // WHY-THIS-SHAPE: the Apply's SetWallpaperWithProperties is held so both openings land while it runs; the library
    //       drops Ocean's edits when River opens, so a draft keyed on the screen alone comes back at the defaults
    //       (speed 1) and the landing link then sends them. Selecting another screen instead is S15.1077.
    // CONTROL: app/daemon/apply_controller.cpp linkSelected()'s `DraftFor selected{connector, open.filePath}` keyed on
    //       the connector alone (control_batch T7a, reverted) -> red, "S15.1130: reopened during its Apply ... speed 1 ...".
    d.calls.clear();
    d.hold.insert(withProps);
    rig.applier->apply();
    const bool applyHeld = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    const qsizetype afterApply = d.calls.size();
    const bool reopened = rig.library->select(rig.river) && rig.library->select(rig.ocean);
    const double reopenedSpeed = shownSpeed();
    d.hold.clear();
    d.release();
    const bool reopenLanded = applied();
    const QStringList laterToDp = toDp(afterApply);
    expect(applyHeld && reopened && reopenLanded && reopenedSpeed == 0.5 && laterToDp.isEmpty() && live() == dpName && shownSpeed() == 0.5,
           QStringLiteral("S15.1130: reopened during its Apply of speed 0.5 to DP-1, Ocean showed speed %1, and after it landed DP-1 was "
                          "sent [%2], live \"%3\" at %4 (held %5, reopened %6, landed %7); expected 0.5, nothing, DP-1 at 0.5")
               .arg(reopenedSpeed).arg(laterToDp.join(u", "), live()).arg(shownSpeed()).arg(applyHeld).arg(reopened).arg(reopenLanded));

    // GOAL: S15.1131 (D12-g, other-wallpaper case) -- an Apply that lands while another wallpaper is open neither
    //       links nor changes that wallpaper's controls, and sends DP-1 nothing after it.
    // WHY-THIS-SHAPE: River is opened and edited while the Apply of Ocean is held, so it provably lands after; the end
    //       state alone cannot show a wrong link (the Apply's end relinks the selection), the stray send can.
    // CONTROL: app/daemon/apply_controller.cpp startLive()'s selection test made `if (false)` (control_batch T7a,
    //       reverted) -> red, "S15.1131: an Apply of Ocean landing with River open ... sent [SetWallpaperProperties 2] ...".
    d.calls.clear();
    d.hold.insert(withProps);
    rig.applier->apply();
    const bool otherHeld = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    const qsizetype afterOther = d.calls.size();
    const bool riverOpen = rig.library->select(rig.river);
    rig.library->setPropertyValue(speedKey, 2.0);
    const bool riverEdited = rig.daemonDrained();
    d.hold.clear();
    d.release();
    const bool otherLanded = applied();
    const QStringList otherToDp = toDp(afterOther);
    expect(otherHeld && riverOpen && riverEdited && otherLanded && live().isEmpty() && shownSpeed() == 2.0 &&
               rig.library->selectedPath() == rig.river && otherToDp.isEmpty(),
           QStringLiteral("S15.1131: an Apply of Ocean landing with River open left live \"%1\", River at speed %2, and sent DP-1 [%3] "
                          "(held %4, opened %5, landed %6); expected no link, 2 and nothing")
               .arg(live()).arg(shownSpeed()).arg(otherToDp.join(u", ")).arg(otherHeld).arg(riverOpen).arg(otherLanded));

    if (!rig.library->select(rig.ocean) || !rig.reread() || live() != dpName || shownSpeed() != 0.5) {
        harnessFailure(std::string(goals) + ": Ocean Waves could not be linked again on DP-1 at speed 0.5 before S15.1132");
        return;
    }

    // GOAL: S15.1132 (D13-f) -- with an edit queued behind a send in flight on the current link to DP-1, an Apply to
    //       DP-1 whose selection moves away before it lands is the last SetWallpaper* DP-1 receives: nothing queued
    //       before it is sent after it.
    // WHY-THIS-SHAPE: the edit's send stays held until after the Apply's reply, so the queue is still pending when the
    //       Apply lands (released first, by index); the Apply's start exempts the current link's queue, so only the drop
    //       at success can stop it. With HDMI-A-1 selected, startLive() queues nothing afresh, so any later call is the stale one.
    // CONTROL: app/daemon/apply_controller.cpp apply()'s drop loop at success deleted (control_batch T7a, reverted)
    //       -> red, "S15.1132: ... DP-1 was sent [SetWallpaperProperties 1] after the Apply ...".
    d.calls.clear();
    d.hold = {props, withProps};
    rig.library->setPropertyValue(speedKey, 2.0);
    const bool editHeld = waitUntil([&] { return d.held.size() == 1; }, kWaitMs);
    rig.library->setPropertyValue(speedKey, 1.0);
    rig.applier->apply();
    const bool bothHeld = waitUntil([&] { return d.held.size() == 2; }, kWaitMs) && d.held.at(1).member() == withProps;
    const qsizetype afterQueued = d.calls.size();
    rig.kscreen->select(hdmiName);
    const bool movedAway = rig.daemonDrained() && live() == hdmiName;
    d.releaseAt(1);
    const bool queuedLanded = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs);
    d.hold.clear();
    d.release();
    const bool queuedSettled = rig.settled();
    const QStringList staleToDp = toDp(afterQueued);
    expect(editHeld && bothHeld && movedAway && queuedLanded && queuedSettled && staleToDp.isEmpty() &&
               rig.applier->applyState() == Progress::Done,
           QStringLiteral("S15.1132: with speed 1 queued behind a held send to DP-1, DP-1 was sent [%1] after the Apply there "
                          "(held %2, both held %3, moved away %4, landed %5, settled %6, state %7); expected nothing")
               .arg(staleToDp.join(u", ")).arg(editHeld).arg(bothHeld).arg(movedAway).arg(queuedLanded).arg(queuedSettled)
               .arg(int(rig.applier->applyState())));
}

// The live link follows what GetStatus says the linked screen shows.
void liveFollowsStatus() {
    constexpr const char* goals = "S15.1008-S15.1014, S15.1017, S15.1076";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("follow"))) {
        return;
    }
    FakeDaemon& d = *rig.daemonFake;
    const QString dpName = QStringLiteral("DP-1");
    const QString speedKey = QStringLiteral("speed");
    const QString volumeKey = QStringLiteral("volume");
    const QString pausedKey = QStringLiteral("paused");
    const QString other = QStringLiteral("/elsewhere/other.mp4");
    const auto value = [&](const QString& key) { return rig.library->propertyValues().value(key); };
    const auto live = [&] { return rig.applier->liveOutput(); };
    // Every SetWallpaper* call since the log was cleared, as "Member id".
    const auto targets = [&] {
        QStringList out;
        for (const FakeCall& c : d.calls) {
            if (c.member == u"SetWallpaperProperties" || c.member == u"SetWallpaperWithProperties") {
                out.append(c.member + u' ' + c.args.value(c.member == u"SetWallpaperProperties" ? 0 : 2).toString());
            }
        }
        return out;
    };

    // GOAL: S15.1011 -- right after an Apply lands, before any read that follows it, the link holds: an edit
    //       made in that gap is sent to the applied screen's id.
    // WHY-THIS-SHAPE: GetStatus is held from the moment the Apply's SetWallpaperWithProperties reached the stand-in,
    //       so the client's last read provably predates the Apply (checked: it shows nothing on DP-1). Unheld, the
    //       re-read the reply asks for shows Ocean there and hides a controller that judges the new link on the stale read.
    // CONTROL: app/daemon/apply_controller.cpp startLive()'s `m_live = std::move(live);` deleted (control_batch T6a,
    //       reverted) -> red, "S15.1011: an edit right after the Apply landed ...".
    d.hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    rig.applier->apply();
    const bool setHeld = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    d.hold = {QStringLiteral("GetStatus")};
    d.release();
    const bool landed = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs);
    const int dp1 = d.outputs.value(dpName, -1);
    const bool stale = !rig.client->status().wallpapers.contains(dp1);
    const QString liveLanded = live();
    d.calls.clear();
    rig.library->setPropertyValue(speedKey, 2.0);
    const bool gapSent = waitUntil([&] { return !targets().isEmpty(); }, kWaitMs) && rig.daemonDrained();
    expect(setHeld && landed && stale && gapSent && liveLanded == dpName &&
               targets() == QStringList{QStringLiteral("SetWallpaperProperties %1").arg(dp1)},
           QStringLiteral("S15.1011: an edit right after the Apply landed (held %1, landed %2, read predates it %3) was sent [%4] "
                          "with live \"%5\"; expected one SetWallpaperProperties to %6")
               .arg(setHeld).arg(landed).arg(stale).arg(joined(targets()), liveLanded).arg(dp1));
    d.hold.clear();
    d.release();
    if (dp1 < 0 || !rig.settled() || !rig.reread() || live() != dpName) {
        harnessFailure(std::string(goals) + ": Ocean Waves could not be left live on DP-1 to start from");
        return;
    }
    const FakeDaemon::Shown ocean = d.screens.value(dp1);

    // GOAL: S15.1008 -- with DP-1 linked, a read showing another file, another type or nothing there empties
    //       liveOutput, and the next speed, volume and Paused edits send no SetWallpaper* call.
    //       S15.1076 (D12-e) -- that read leaves the controls showing the wallpaper's declared defaults.
    // WHY-THIS-SHAPE: each row relinks from GetStatus first, so every row starts linked at values other than the
    //       defaults (checked); "nothing sent" is read once every reply has been handled (settled).
    // CONTROL: control_batch 2026-10-28, each reverted, app/daemon/apply_controller.cpp: followStatus()'s unlink
    //       made `if (false && ...)` -> red, "S15.1008: with DP-1 showing another file, speed, volume and Paused edits sent
    //       [SetWallpaperProperties 0,SetWallpaperProperties 0]" (every row); showing() without its path test -> red, "... showing another file, the read
    //       left live on "DP-1""; without its type test -> red, "... showing another type, ..."; returning the id when the
    //       monitor shows nothing -> red, "... showing nothing, ..."; unlink()'s `m_draftFor.reset();` deleted
    //       (control_batch T6a; re-run T7a with the draft keyed on screen and wallpaper, reverted) -> red, all three
    //       rows, "D12-e: with DP-1 showing another file, the read left the controls at {... speed=2 ...}".
    // COVERAGE-ASSERT: all three rows graded.
    struct Row {
        const char* what;
        std::function<void()> change;
    };
    const Row rows[] = {
        {"another file", [&] { d.screens[dp1].path = other; }},
        {"another type", [&] { d.screens[dp1].type = QStringLiteral("image"); }},
        {"nothing", [&] { d.screens.remove(dp1); d.wallpapers.remove(dp1); }},
    };
    int graded = 0;
    for (const Row& row : rows) {
        d.wallpapers.insert(dp1);
        d.screens.insert(dp1, ocean);
        if (!rig.reread() || live() != dpName) {
            harnessFailure(std::string(goals) + ": DP-1 could not be linked again before the \"" + row.what + "\" row");
            break;
        }
        const QVariantMap before = rig.library->propertyValues();
        const QVariantMap defaults = rig.library->selectedWallpaper().defaultValues();
        row.change();
        const bool read = rig.reread();
        const QString after = live();
        const QVariantMap kept = rig.library->propertyValues();
        expect(read && after.isEmpty(), QStringLiteral("S15.1008: with DP-1 showing %1, the read left live on \"%2\" (read %3); expected \"\"")
                                             .arg(QLatin1StringView(row.what), after).arg(read));
        expect(read && before != defaults && kept == defaults,
               QStringLiteral("D12-e: with DP-1 showing %1, the read left the controls at %2 (linked at %3); expected the defaults %4")
                   .arg(QLatin1StringView(row.what), describeMap(kept), describeMap(before), describeMap(defaults)));
        d.calls.clear();
        const bool edited = rig.library->setPropertyValue(speedKey, 0.5) && rig.library->setPropertyValue(volumeKey, 0.25);
        const bool paused = rig.library->setPropertyValue(pausedKey, true);
        const bool quiet = paused && rig.settled();
        expect(edited && quiet && value(pausedKey).toBool() && targets().isEmpty(),
               QStringLiteral("S15.1008: with DP-1 showing %1, speed, volume and Paused edits sent [%2] (edited %3, settled %4); expected []")
                   .arg(QLatin1StringView(row.what), joined(targets())).arg(edited).arg(quiet));
        ++graded;
    }
    expect(graded == 3, QStringLiteral("S15.1008: %1 of 3 rows graded").arg(graded));

    // GOAL: S15.1009 -- edits made while unlinked stay through a later read, and the next Apply sends them.
    // CONTROL: control_batch 2026-10-28, reverted: app/daemon/apply_controller.cpp linkSelected() calling
    //       m_library.resetPropertyValues() after its unlink() -> red, "S15.1009: after another read while unlinked ..."
    //       and (separate run, same break) "S15.1009: the Apply after the unlinked edits sent ...".
    const bool readAgain = rig.reread();
    expect(readAgain && live().isEmpty() && value(speedKey).toDouble() == 0.5 && value(volumeKey).toDouble() == 0.25 && value(pausedKey).toBool(),
           QStringLiteral("S15.1009: after another read while unlinked the controls show speed %1, volume %2, paused %3 (live \"%4\"); "
                          "expected 0.5, 0.25, true")
               .arg(value(speedKey).toString(), value(volumeKey).toString(), value(pausedKey).toString(), live()));
    d.calls.clear();
    const bool applied = rig.apply() && rig.applier->applyState() == Progress::Done && rig.settled();
    const auto applies = rig.sent("SetWallpaperWithProperties");
    const QVariantMap applySent = applies.isEmpty() ? QVariantMap() : applies.back().args.value(3).toMap();
    // D12-e: the defaults the link ended at, plus the edits made after.
    QVariantMap wantSent = rig.library->selectedWallpaper().defaultValues();
    wantSent.insert(speedKey, 0.5);
    wantSent.insert(volumeKey, 0.25);
    wantSent.insert(pausedKey, true);
    expect(applied && applies.size() == 1 && describeMap(applySent) == describeMap(wantSent),
           QStringLiteral("S15.1009/D12-e: the Apply after the unlinked edits sent %1 (%2 call(s), applied %3); expected %4")
               .arg(describeMap(applySent)).arg(applies.size()).arg(applied).arg(describeMap(wantSent)));

    // GOAL: S15.1010 -- a later read showing the open wallpaper on DP-1 again relinks it, shows DP-1's own values
    //       and sends nothing; the next edit goes to DP-1's id only.
    //       S15.1017 -- a draft made while unlinked and never sent is dropped for the screen's values.
    // WHY-THIS-SHAPE: DP-1 comes back at values the controls do not show (speed 2, volume 0.75), so showing the
    //       draft instead of the screen's values is red.
    // CONTROL: control_batch 2026-10-28, reverted, app/daemon/apply_controller.cpp: followStatus()'s
    //       `linkSelected();` deleted (re-run T6a) -> red, "S15.1010: with Ocean back on DP-1 ..., the read linked """;
    //       linkSelected()'s adoptPropertyValues(...) deleted (re-run 2026-10-28 with an unsent draft of speed 1 standing in the controls) -> red, "S15.1010: with Ocean back on DP-1 at speed 2 (unlinked first 1), the read linked \"DP-1\", showed speed 1, volume 0.25 ...";
    //       sendLive()'s setProperties given `m_live->monitorId + 1` -> red, "S15.1010: the next edit sent [SetWallpaperProperties 1]".
    d.screens[dp1].path = other;
    const bool unlinkedRead = rig.reread() && live().isEmpty();
    // An unsent draft made while unlinked; the screen's values must replace it.
    const bool drafted = rig.library->setPropertyValue(speedKey, 1.0);
    FakeDaemon::Shown back = d.screens.value(dp1);
    back.path = ocean.path;
    back.properties.insert(speedKey, 2.0);
    back.properties.insert(volumeKey, 0.75);
    d.screens.insert(dp1, back);
    d.calls.clear();
    const bool backRead = rig.reread();
    expect(unlinkedRead && drafted && backRead && live() == dpName && value(speedKey).toDouble() == 2.0 && value(volumeKey).toDouble() == 0.75 &&
               targets().isEmpty(),
           QStringLiteral("S15.1010: with Ocean back on DP-1 at speed 2 (unlinked first %1), the read linked \"%2\", showed speed %3, "
                          "volume %4 and sent [%5]; expected DP-1, 2, 0.75, []")
               .arg(unlinkedRead).arg(live(), value(speedKey).toString(), value(volumeKey).toString(), joined(targets())));
    rig.library->setPropertyValue(speedKey, 1.0);
    const bool editSettled = rig.settled();
    expect(editSettled && targets() == QStringList{QStringLiteral("SetWallpaperProperties %1").arg(dp1)},
           QStringLiteral("S15.1010: the next edit sent [%1]; expected one SetWallpaperProperties to %2").arg(joined(targets())).arg(dp1));

    // GOAL: S15.1012 -- after a daemon restart, the read that lands while the app is setting the linked wallpaper
    //       again does not end the link, nor does the read after an accepted re-send.
    // WHY-THIS-SHAPE: the restarted stand-in holds nothing, and the client's own read on the restart is sent before
    //       the re-send on the same connection, so it provably predates it; the re-send's reply is held, so that
    //       read provably lands while the send runs. Reads are counted, and every wait's deadline fails the check.
    // CONTROL: app/daemon/apply_controller.cpp followStatus(): `if (m_live->sent || replaced)` made `if (true)`
    //       (control_batch 2026-10-28, reverted) -> red, "S15.1012: a read landing while the restarted daemon was being sent Ocean again (read 1, nothing on DP-1 1)
    //       left live on \"\"; expected DP-1".
    int reads = 0;
    const auto counter = QObject::connect(rig.client.get(), &DaemonClient::statusRead, [&] { ++reads; });
    const auto restart = [&] {
        const bool quiet = rig.settled();
        d.stop();
        const bool gone = waitUntil([&] { return rig.client->state() == DaemonClient::State::Unavailable; }, kWaitMs);
        d.wallpapers.clear();
        d.screens.clear();
        d.calls.clear();
        reads = 0;
        return quiet && gone && d.start().isEmpty();
    };
    d.hold.insert(QStringLiteral("SetWallpaperWithProperties"));
    const bool restarted = restart();
    const bool resendHeld = waitUntil([&] { return !d.held.isEmpty(); }, kWaitMs);
    const bool readDuring = waitUntil([&] { return reads >= 1; }, kWaitMs) && rig.repliesHandled();
    const bool shownNothing = !rig.client->status().wallpapers.contains(dp1);
    const QString liveDuring = live();
    d.hold.clear();
    d.release();
    if (!restarted || !resendHeld) {
        QObject::disconnect(counter);
        harnessFailure(std::string(goals) + ": the daemon's restart could not be staged");
        return;
    }
    expect(readDuring && shownNothing && liveDuring == dpName,
           QStringLiteral("S15.1012: a read landing while the restarted daemon was being sent Ocean again (read %1, nothing on DP-1 %2) "
                          "left live on \"%3\"; expected DP-1").arg(readDuring).arg(shownNothing).arg(liveDuring));
    const bool acceptedRead = waitUntil([&] { return reads >= 2; }, kWaitMs) && rig.settled();
    expect(acceptedRead && live() == dpName,
           QStringLiteral("S15.1012: after the re-send was accepted the next read (landed %1) left live on \"%2\"; expected DP-1")
               .arg(acceptedRead).arg(live()));
    // GOAL: S15.1014 -- after a refused re-send: a read showing the monitor empty keeps the link, and the next edit
    //       sets the wallpaper again (SetWallpaperWithProperties); a read showing the wallpaper there takes what the
    //       screen holds as sent, so the next edit is a SetWallpaperProperties; another wallpaper there ends the link.
    // WHY-THIS-SHAPE: `refuse` answers before the stand-in changes anything, so the restarted daemon provably holds
    //       nothing on DP-1; the wallpaper is then put there behind the app's back, as only that read can explain it.
    // CONTROL: app/daemon/apply_controller.cpp, control_batch 2026-10-28, each reverted: followStatus()'s
    //       `if (m_live->sent || replaced)` made `if (true)` -> red, "S15.1014: after the re-send was refused, the read showing DP-1 empty (2 read(s)) left live on \"\"; ..."; sendTo()'s `if (to.sent)` made
    //       `if (true)` -> red, "S15.1014: an edit on DP-1 left empty by a refused re-send sent [SetWallpaperProperties 0] ..." (`if (true)` alone
    //       stopped the restart's own re-send, a harness fault, so the break sends by monitor only once the read shows the
    //       monitor empty: `if (to.sent || (m_daemon.state() == Ready && !status.wallpapers.contains(to.monitorId)))`); followStatus()'s `m_live->sent = status.wallpapers...` deleted -> red,
    //       "S15.1014: with Ocean found on DP-1 after a refused re-send, the read sent [SetWallpaperWithProperties 0] ..."; `replaced`'s `|| status.wallpapers.contains(*id)` dropped -> red, "S15.1014: with another file found on DP-1 after a refused re-send (still linked first 1), the read left live
    //       on \"DP-1\"; expected \"\"".
    const QString refused = QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs");
    d.refuse.insert(QStringLiteral("SetWallpaperWithProperties"), {refused, QStringLiteral("no")});
    bool restartedAgain = restart();
    const bool refusedRead = restartedAgain && waitUntil([&] { return reads >= 2; }, kWaitMs) && rig.settled();
    expect(refusedRead && !rig.sent("SetWallpaperWithProperties").isEmpty() && live() == dpName,
           QStringLiteral("S15.1014: after the re-send was refused, the read showing DP-1 empty (%1 read(s)) left live on \"%2\"; expected DP-1")
               .arg(reads).arg(live()));
    d.calls.clear();
    rig.library->setPropertyValue(speedKey, 0.5);
    const bool emptyEdit = rig.settled();
    expect(emptyEdit && targets() == QStringList{QStringLiteral("SetWallpaperWithProperties %1").arg(dp1)} && live() == dpName,
           QStringLiteral("S15.1014: an edit on DP-1 left empty by a refused re-send sent [%1] with live \"%2\"; expected one "
                          "SetWallpaperWithProperties to %3").arg(joined(targets()), live()).arg(dp1));
    d.refuse.clear();
    d.wallpapers.insert(dp1);
    d.screens.insert(dp1, FakeDaemon::Shown{ocean.type, ocean.path, rig.library->propertyValues()});
    d.calls.clear();
    const bool shownRead = rig.reread();
    const QStringList onRead = targets();
    rig.library->setPropertyValue(speedKey, 2.0);
    const bool shownEdit = rig.settled();
    expect(shownRead && shownEdit && onRead.isEmpty() && targets() == QStringList{QStringLiteral("SetWallpaperProperties %1").arg(dp1)},
           QStringLiteral("S15.1014: with Ocean found on DP-1 after a refused re-send, the read sent [%1] and the next edit [%2]; "
                          "expected [] and one SetWallpaperProperties to %3").arg(joined(onRead), joined(targets())).arg(dp1));
    d.refuse.insert(QStringLiteral("SetWallpaperWithProperties"), {refused, QStringLiteral("no")});
    restartedAgain = restart();
    const bool refusedAgain = restartedAgain && waitUntil([&] { return reads >= 2; }, kWaitMs) && rig.settled() && live() == dpName;
    d.refuse.clear();
    d.wallpapers.insert(dp1);
    d.screens.insert(dp1, FakeDaemon::Shown{ocean.type, other, {}});
    const bool otherRead = rig.reread();
    QObject::disconnect(counter);
    expect(refusedAgain && otherRead && live().isEmpty(),
           QStringLiteral("S15.1014: with another file found on DP-1 after a refused re-send (still linked first %1), the read "
                          "left live on \"%2\"; expected \"\"").arg(refusedAgain).arg(live()));

    // GOAL: S15.1013 -- when the linked connector is re-bound to another monitor id that does not show the open
    //       wallpaper, the read ends the link, though the old id still shows it.
    // CONTROL: control_batch 2026-10-28, reverted, app/daemon/apply_controller.cpp followStatus() judging the
    //       link by its monitor id alone (`status.wallpapers.value(m_live->monitorId).path`) -> red, "S15.1013: ...".
    d.wallpapers.insert(dp1);
    d.screens.insert(dp1, ocean);
    if (!rig.reread() || live() != dpName) {
        harnessFailure(std::string(goals) + ": DP-1 could not be linked again before the re-bind");
        return;
    }
    const int rebound = dp1 + 5;
    d.outputs.insert(dpName, rebound);
    const bool reboundRead = rig.reread();
    expect(reboundRead && rig.client->status().outputs.value(dpName) == rebound && rig.client->status().wallpapers.contains(dp1) && live().isEmpty(),
           QStringLiteral("S15.1013: with DP-1 re-bound to monitor %1, which shows nothing, the read (landed %2) left live on \"%3\"; expected \"\"")
               .arg(rebound).arg(reboundRead).arg(live()));
}

void bindReadFails() {
    constexpr const char* goals = "S15.259, S15.260";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("bindfail"))) {
        return;
    }
    FakeDaemon& d = *rig.daemonFake;
    const QString noAnswer = QStringLiteral("The WallpaperX service did not answer.");

    // GOAL: S15.259 -- a bind whose fresh status read fails makes the client Unavailable with the
    //       reason, and drops the pause modes it showed, as a failed refresh does.
    // CONTROL: app/daemon/daemon_client.cpp, bindNext()'s guarded `settleRead(status);` made
    //       `if (status) settleRead(status);` (control_batch, 2026-10-04) -> red, "S15.259: after the
    //       bind's read failed the client is state 1 reading "" with modes {} (pause: ...)". The modes
    //       are empty either way here (the stand-in names no screen yet); the state is what goes red.
    d.refuse.insert(QStringLiteral("GetStatus"), {QStringLiteral("org.freedesktop.DBus.Error.NoReply"), QStringLiteral("no")});
    rig.applier->setPauseMode(QStringLiteral("always_paused"));
    const bool failed = waitUntil([&] { return rig.applier->pauseState() == Progress::Failed; }, kWaitMs);
    d.refuse.clear();
    expect(failed && rig.client->state() == DaemonClient::State::Unavailable && rig.client->unavailableReason() == noAnswer &&
               rig.client->pauseModes().isEmpty() &&
               rig.applier->pauseMessage() == QStringLiteral("The pause setting for DP-1 was not changed. ") + noAnswer,
           QStringLiteral("S15.259: after the bind's read failed the client is state %1 reading \"%2\" with modes %3 (pause: \"%4\")")
               .arg(int(rig.client->state())).arg(rig.client->unavailableReason(), describeMap(rig.client->pauseModes()),
                                                   rig.applier->pauseMessage()));
    rig.client->refresh();
    if (!waitUntil([&] { return rig.client->state() == DaemonClient::State::Ready; }, kWaitMs)) {
        harnessFailure("S15.260: the client never read the status again after S15.259's failure");
        return;
    }

    // GOAL: S15.260 -- a bind's status read that fails never replaces the answer to a refresh sent
    //       after it: the refresh's healthy read showing the failed save is what the client shows.
    // WHY-THIS-SHAPE: every GetStatus is held, so the bind's read is outstanding when the refresh is
    //       sent, and it is failed (refuseAt) before the refresh is answered; both come back on one
    //       connection in that order. The refresh's read carries a failed save, so "Ready" alone
    //       from before cannot pass.
    // CONTROL: app/daemon/daemon_client.cpp, bindNext()'s `if (generation == m_generation)` made
    //       `if (true)` (control_batch, 2026-10-04) -> red, "S15.260: with the bind's read failed and
    //       a refresh sent after it, the client is state 2 ("The WallpaperX service did not answer.")
    //       showing "" for the save".
    const QString failing = QStringLiteral("The WallpaperX service could not save its settings, so they may be lost when it restarts: the disk is full");
    d.hold.insert(QStringLiteral("GetStatus"));
    QObject context;
    bool bound = false;
    rig.client->bindMonitor(&context, QStringLiteral("DP-5"), [&](std::expected<int, QString>) { bound = true; });
    const bool bindHeld = waitUntil([&] { return d.held.size() == 1; }, kWaitMs);
    rig.client->refresh();
    const bool bothHeld = bindHeld && waitUntil([&] { return d.held.size() == 2; }, kWaitMs);
    d.persistence = QStringLiteral("fail");
    d.refuseAt(0, QStringLiteral("org.freedesktop.DBus.Error.NoReply"), QStringLiteral("no"));
    d.releaseAt(0);
    d.hold.clear();
    const bool handled = bothHeld && waitUntil([&] { return bound; }, kWaitMs) && rig.repliesHandled();
    expect(handled && rig.client->state() == DaemonClient::State::Ready && rig.client->persistenceProblem() == failing,
           QStringLiteral("S15.260: with the bind's read failed and a refresh sent after it, the client is state %1 (\"%2\") showing "
                          "\"%3\" for the save (both held %4, handled %5)")
               .arg(int(rig.client->state())).arg(rig.client->unavailableReason(), rig.client->persistenceProblem()).arg(bothHeld).arg(handled));
    d.persistence = QStringLiteral("ok");
}

void errorText() {
    constexpr const char* goals = "S15.261, S15.262";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("errtext"))) {
        return;
    }
    const QString noOwner = QStringLiteral("org.freedesktop.DBus.Error.NameHasNoOwner");

    // GOAL: S15.261 -- a NameHasNoOwner error reads as the service not running, for the daemon and
    //       for Plasma (KScreen's is in kscreen_client_test).
    // WHY-THIS-SHAPE: the private bus answers a call to an absent name with ServiceUnknown, so the
    //       stand-ins send NameHasNoOwner themselves; otherwise only ServiceUnknown is ever graded.
    // CONTROL: app/kde/dbus_reply.h, `|| error.name() == u"org.freedesktop.DBus.Error.NameHasNoOwner"`
    //       deleted from isUnreachable (control_batch, 2026-10-04) -> red, "S15.261: the daemon's
    //       NameHasNoOwner reads "... could not do it: gone"" and the Plasma line.
    rig.daemonFake->refuse.insert(QStringLiteral("SetMonitorPause"), {noOwner, QStringLiteral("gone")});
    rig.applier->setPauseMode(QStringLiteral("always_paused"));
    const bool pauseFailed = waitUntil([&] { return rig.applier->pauseState() == Progress::Failed; }, kWaitMs);
    rig.daemonFake->refuse.clear();
    const QString daemonWant = QStringLiteral("The pause setting for DP-1 was not changed. The WallpaperX service (wallpaperx-daemon) is not running.");
    expect(pauseFailed && rig.applier->pauseMessage() == daemonWant,
           QStringLiteral("S15.261: the daemon's NameHasNoOwner reads \"%1\"").arg(rig.applier->pauseMessage()));
    rig.shellFake->refuse.insert(QStringLiteral("evaluateScript"), {noOwner, QStringLiteral("gone")});
    const auto plasma = activate(*rig.plasma, QStringLiteral("DP-1"));
    rig.shellFake->refuse.clear();
    expect(plasma && !*plasma && plasma->error() == u"Plasma's desktop shell (plasmashell) is not running.",
           QStringLiteral("S15.261: Plasma's NameHasNoOwner reads \"%1\"").arg(outcome(plasma)));

    // GOAL: S15.262 -- a daemon's error message is shown trimmed of the whitespace around it.
    // CONTROL: app/kde/dbus_reply.h, `message = message.trimmed();` deleted from clamped()
    //       (control_batch, 2026-10-04) -> red, "S15.262: a refusal padded with whitespace reads
    //       "... refused it: \n  speed must be in (0, 2]\t \n"".
    rig.daemonFake->refuse.insert(QStringLiteral("SetMonitorPause"),
                                  {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("\n  speed must be in (0, 2]\t \n")});
    rig.applier->setPauseMode(QStringLiteral("play"));
    const bool refused = waitUntil([&] { return rig.applier->pauseState() == Progress::Failed && rig.applier->pauseMessage() != daemonWant; }, kWaitMs);
    rig.daemonFake->refuse.clear();
    QString shown = rig.applier->pauseMessage();
    expect(refused && shown == u"The pause setting for DP-1 was not changed. The WallpaperX service refused it: speed must be in (0, 2]",
           QStringLiteral("S15.262: a refusal padded with whitespace reads \"%1\"")
               .arg(shown.replace(u'\n', QStringLiteral("\\n")).replace(u'\t', QStringLiteral("\\t"))));
}

// ---- how long a status read may take ----------------------------------------------------------

// S15.263's limits: a read answered after kSlowHoldMs, more than half of kSlowCallMs and less than
// all of it, so a client giving status reads any shorter limit than calls gets it wrong.
constexpr int kSlowCallMs = 4000;
constexpr int kSlowHoldMs = 2500;

// Releases the one held GetStatus once kSlowHoldMs have passed since `sent`, and returns once the
// client has handled the answer: the release time, or nothing after reporting a harness fault.
// Sets `unavailable` if the client showed Unavailable meanwhile.
std::optional<qint64> answerSlowly(Rig& rig, const QElapsedTimer& sent, bool& unavailable, const char* what) {
    FakeDaemon& d = *rig.daemonFake;
    QObject watch;
    QObject::connect(rig.client.get(), &DaemonClient::statusChanged, &watch, [&] {
        unavailable = unavailable || rig.client->state() == DaemonClient::State::Unavailable;
    });
    if (!waitUntil([&] { return d.held.size() == 1; }, kWaitMs)) {
        harnessFailure(std::string("S15.263: ") + what + "'s GetStatus never reached the stand-in");
        return std::nullopt;
    }
    if (const qint64 left = kSlowHoldMs - sent.elapsed(); left > 0) {
        pump(int(left));
    }
    const qint64 at = sent.elapsed();
    d.hold.clear();
    d.release();
    if (!rig.repliesHandled()) {
        harnessFailure(std::string("S15.263: the marker sent after ") + what + "'s answer was never answered");
        return std::nullopt;
    }
    return at;
}

// A release this late could be timed out by a correct client too, so it grades nothing.
bool releasedInTime(const char* goal, qint64 at) {
    if (at < kSlowCallMs - 750) {
        return true;
    }
    harnessFailure(std::string(goal) + ": the test released the read only after " + std::to_string(at) +
                   " ms, too close to the " + std::to_string(kSlowCallMs) + " ms limit to grade");
    return false;
}

void slowStatusRefresh() {
    Rig rig;
    if (!build(rig, "S15.263", QStringLiteral("slowrefresh"), {kSlowCallMs})) {
        return;
    }
    // GOAL: S15.263 -- a refresh() whose GetStatus is answered after more than half of callMs, but
    //       within it, is shown: the client never turns Unavailable, and ends Ready with the answer.
    // WHY-THIS-SHAPE: the held answer reports a failed save, which the earlier answer did not, so a
    //       client that dropped the slow reply and kept its old Ready cannot pass. "Ever Unavailable"
    //       is recorded from statusChanged, not read at the end, so a time-out followed by a later
    //       recovery is still caught. A test of the old 5 s figure itself would take 5 s per read; the
    //       property is that status reads get callMs, so callMs is injected short.
    // CONTROL: app/daemon/daemon_client.cpp, refresh()'s GetStatus given `m_timeouts.callMs / 2`
    //       (control_batch, 2026-10-04) -> red, "S15.263: a refresh answered after 2500 ms of a
    //       4000 ms limit left the client state 2 ("The WallpaperX service did not answer."),
    //       Unavailable meanwhile 1, showing "" for the save".
    rig.daemonFake->persistence = QStringLiteral("fail");
    rig.daemonFake->hold.insert(QStringLiteral("GetStatus"));
    bool unavailable = false;
    QElapsedTimer sent;
    sent.start();
    rig.client->refresh();
    const auto at = answerSlowly(rig, sent, unavailable, "the refresh");
    if (!at) {
        return;
    }
    if (releasedInTime("S15.263", *at)) {
        expect(!unavailable && rig.client->state() == DaemonClient::State::Ready && !rig.client->persistenceProblem().isEmpty(),
               QStringLiteral("S15.263: a refresh answered after %1 ms of a %2 ms limit left the client state %3 (\"%4\"), "
                              "Unavailable meanwhile %5, showing \"%6\" for the save")
                   .arg(*at).arg(kSlowCallMs).arg(int(rig.client->state())).arg(rig.client->unavailableReason())
                   .arg(unavailable).arg(rig.client->persistenceProblem()));
    }
}

void slowStatusBind() {
    Rig rig;
    if (!build(rig, "S15.263", QStringLiteral("slowbind"), {kSlowCallMs})) {
        return;
    }
    // GOAL: S15.263 -- a bind whose fresh GetStatus is answered after more than half of callMs, but
    //       within it, succeeds: the screen is named at the daemon and the client never turns
    //       Unavailable.
    // CONTROL: app/daemon/daemon_client.cpp, bindNext()'s GetStatus given `m_timeouts.callMs / 2`
    //       (control_batch, 2026-10-04) -> red, "S15.263: a bind whose read was answered after 2500 ms
    //       of a 4000 ms limit gave refused: The WallpaperX service did not answer., ...".
    FakeDaemon& d = *rig.daemonFake;
    d.hold.insert(QStringLiteral("GetStatus"));
    bool unavailable = false;
    std::optional<std::expected<int, QString>> bound;
    QObject context;
    QElapsedTimer sent;
    sent.start();
    rig.client->bindMonitor(&context, QStringLiteral("DP-7"), [&](std::expected<int, QString> r) { bound = std::move(r); });
    const auto at = answerSlowly(rig, sent, unavailable, "the bind");
    if (!at) {
        return;
    }
    const bool answered = waitUntil([&] { return bound.has_value(); }, kWaitMs);
    if (releasedInTime("S15.263", *at)) {
        expect(answered && *bound && d.outputs.value(QStringLiteral("DP-7"), -1) == **bound && !unavailable &&
                   rig.client->state() == DaemonClient::State::Ready,
               QStringLiteral("S15.263: a bind whose read was answered after %1 ms of a %2 ms limit gave %3, the daemon names "
                              "DP-7 as %4, Unavailable meanwhile %5, the client ends state %6")
                   .arg(*at).arg(kSlowCallMs).arg(outcome(bound)).arg(d.outputs.value(QStringLiteral("DP-7"), -1))
                   .arg(unavailable).arg(int(rig.client->state())));
    }
}

void statusNoAnswer() {
    constexpr int callMs = 2000;
    Rig rig;
    if (!build(rig, "S15.264", QStringLiteral("noanswer"), {callMs})) {
        return;
    }
    // GOAL: S15.264 -- a GetStatus never answered makes the client Unavailable with "did not
    //       answer" once callMs has passed; a hung daemon is still reported.
    // WHY-THIS-SHAPE: the wait is callMs plus kWaitMs and has a failure branch: running out of it is
    //       the check going red, never a pass.
    // CONTROL: app/daemon/daemon_client.cpp, refresh()'s GetStatus given `m_timeouts.callMs * 10`
    //       (control_batch, 2026-10-04) -> red, "S15.264: a read never answered left the client state 1
    //       ("") after 7001 ms with a 2000 ms limit".
    rig.daemonFake->hold.insert(QStringLiteral("GetStatus"));
    QElapsedTimer sent;
    sent.start();
    rig.client->refresh();
    const bool gone = waitUntil([&] { return rig.client->state() == DaemonClient::State::Unavailable; }, callMs + kWaitMs);
    expect(gone && rig.client->unavailableReason() == u"The WallpaperX service did not answer.",
           QStringLiteral("S15.264: a read never answered left the client state %1 (\"%2\") after %3 ms with a %4 ms limit")
               .arg(int(rig.client->state())).arg(rig.client->unavailableReason()).arg(sent.elapsed()).arg(callMs));
    rig.daemonFake->hold.clear();
    rig.daemonFake->release();
}

// ---- frame rates: the notice, the refresh-rate reporter and the setters (S15.911-S15.913) ----

// `out` showing a mode at `hz`, as KScreen lists the current mode.
QVariantMap atRate(QVariantMap out, double hz) {
    out.insert(QStringLiteral("modes"),
               QVariantList{QVariantMap{{QStringLiteral("id"), QStringLiteral("1")},
                                        {QStringLiteral("refreshRate"), hz},
                                        {QStringLiteral("size"), QVariantMap{{QStringLiteral("width"), qlonglong(1920)},
                                                                             {QStringLiteral("height"), qlonglong(1080)}}}}});
    out.insert(QStringLiteral("currentModeId"), QStringLiteral("1"));
    return out;
}

enum class Hdmi { Shown, Mirrored, ZeroArea, Gone };

// The rig's two screens at the given rates; HDMI-A-1 as `hdmi` says.
QVariantMap screensAt(double dp1Hz, double hdmiHz, Hdmi hdmi = Hdmi::Shown) {
    QVariantList outs{atRate(output({.name = QStringLiteral("DP-1"), .id = 1, .priority = 1}), dp1Hz)};
    if (hdmi != Hdmi::Gone) {
        OutputSpec spec{.name = QStringLiteral("HDMI-A-1"), .id = 2, .x = qlonglong(1920), .priority = 2};
        if (hdmi == Hdmi::Mirrored) {
            spec.replicationSource = qlonglong(1);
        }
        // An enabled screen with no area makes KScreen's whole answer unreadable (parseKScreenConfig),
        // so the one that reaches the reporter is a disabled screen KScreen lists with no size.
        if (hdmi == Hdmi::ZeroArea) {
            spec.width = qlonglong(0);
            spec.enabled = false;
        }
        outs.append(atRate(output(spec), hdmiHz));
    }
    return configOf(outs);
}

// Serves `config` from the fake KScreen and waits until the client holds it.
bool showScreens(Rig& rig, const QVariantMap& config) {
    const auto want = parseKScreenConfig(config);
    rig.kscreenFake->config = config;
    rig.kscreen->refresh();
    return want && waitUntil([&] {
        return rig.kscreen->state() == KScreenClient::State::Ready && rig.kscreen->monitors() == *want;
    }, kWaitMs);
}

// Every call the app makes in answer to what it last read, and their replies, handled: each round
// drains what the previous round's handlers sent.
bool quiet(Rig& rig) {
    for (int i = 0; i < 4; ++i) {
        if (!rig.daemonDrained() || !rig.repliesHandled()) {
            return false;
        }
    }
    return true;
}

bool reread(Rig& rig) {
    rig.client->refresh();
    return quiet(rig);
}

// The approved sentence, written out here rather than taken from the app.
QString approvedNotice(int n) {
    return QString::fromUtf8("Your screens refresh at most %1 times a second. Pictures above that can't be shown — they only "
                             "cost power and memory — so WallpaperX plays at your screen's rate. You can keep a higher rate anyway.")
        .arg(n);
}

QString describeReports(const QList<FakeCall>& calls) {
    QStringList out;
    for (const FakeCall& c : calls) {
        out.append(c.args.value(0).toString() + u' ' + QString::number(c.args.value(1).toDouble()));
    }
    return u'[' + out.join(QStringLiteral(", ")) + u']';
}

void frameRateNotice() {
    constexpr const char* goals = "S15.911";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("notice"))) {
        return;
    }
    // KScreen says HDMI-A-1 is 60 Hz throughout, so a notice decided from KScreen rather than from the
    // daemon's status shows on the rows below where the daemon does not slow HDMI-A-1.
    if (!showScreens(rig, screensAt(165, 60))) {
        harnessFailure(std::string(goals) + ": the fake KScreen's 165 Hz and 60 Hz screens never reached the client");
        return;
    }
    FakeDaemon& fake = *rig.daemonFake;
    const QString dp1 = QStringLiteral("DP-1");
    const QString hdmi = QStringLiteral("HDMI-A-1");
    const QMap<QString, int> both{{dp1, 0}, {hdmi, 1}};
    struct Row {
        const char* what;
        QMap<QString, int> outputs;
        QSet<int> wallpapers;
        QMap<QString, double> refresh;
        int global;
        QMap<int, QJsonValue> own;
        bool keepHigher;
        bool above;
        int n;  // 0: no notice
    };
    // GOAL: S15.911 -- the notice is the approved sentence exactly when a screen holding a wallpaper,
    //       named by the daemon, has a frame rate above the refresh rate the daemon holds for it and
    //       keep-higher is off; N is the highest rounded refresh among only those screens; the "Keep a
    //       higher rate anyway" row shows whenever such a screen exists, keep-higher on or off.
    // WHY-THIS-SHAPE: one row per way the answer could come from the wrong input. HDMI-A-1 is 60 Hz in
    //       KScreen on every row, so the rows where the daemon holds no rate, the screen holds no
    //       wallpaper or has no daemon name tell a status-based notice from a KScreen-based one. The
    //       165 Hz + 60 Hz row tells "N among the over-rate screens" from "N among all screens".
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^frameRateNotice$',
    //       in app/daemon/apply_controller.cpp: N taken as the highest refresh rate of every screen ->
    //       "FAIL: S15.911: 165 Hz and 60 Hz both at 120: ... at most 165 times a second"; the wallpaper
    //       test deleted -> "... the 60 Hz screen holds no wallpaper: the row shows 1"; the loop reading
    //       KScreen's monitors instead of the daemon's refresh rates -> "... the daemon holds no refresh
    //       rate for the 60 Hz screen: the row shows 1", and with the wallpaper test also gone -> also
    //       "... the 60 Hz screen has no daemon name"; frameRateNotice() ignoring keep-higher -> "...
    //       keep-higher on: the row shows 1 and the notice reads "Your screens ..."; screensAboveRefresh()
    //       giving nothing under keep-higher -> "... keep-higher on: the row shows 0".
    // COVERAGE-ASSERT: every row was read by the client and graded.
    const Row rows[] = {
        {"165 Hz and 60 Hz both at 120", both, {0, 1}, {{dp1, 165}, {hdmi, 60}}, 120, {}, false, true, 60},
        {"100 Hz and 60 Hz both at 120", both, {0, 1}, {{dp1, 100}, {hdmi, 60}}, 120, {}, false, true, 100},
        {"the 60 Hz screen's own rate 120, every screen's 60", both, {0, 1}, {{dp1, 165}, {hdmi, 60}}, 60, {{1, 120}}, false, true, 60},
        {"the daemon holds no refresh rate for the 60 Hz screen", both, {0, 1}, {{dp1, 165}}, 120, {}, false, false, 0},
        {"the 60 Hz screen holds no wallpaper", both, {0}, {{dp1, 165}, {hdmi, 60}}, 120, {}, false, false, 0},
        {"the 60 Hz screen has no daemon name", {{dp1, 0}}, {0, 1}, {{dp1, 165}, {hdmi, 60}}, 120, {}, false, false, 0},
        {"a 59.94 Hz screen at 60", both, {0, 1}, {{dp1, 165}, {hdmi, 59.94}}, 60, {}, false, false, 0},
        {"keep-higher on", both, {0, 1}, {{dp1, 165}, {hdmi, 60}}, 120, {}, true, true, 0},
    };
    const auto load = [&](const Row& row) {
        fake.outputs = row.outputs;
        fake.wallpapers = row.wallpapers;
        fake.refreshRates = row.refresh;
        fake.frameRate = row.global;
        fake.screenFrameRate = row.own;
        fake.keepHigherFrameRate = row.keepHigher;
        return reread(rig) && rig.client->status().frameRates && rig.client->status().outputs.size() == row.outputs.size() &&
               rig.client->status().frameRates->keepHigher == row.keepHigher;
    };
    qsizetype graded = 0;
    for (const Row& row : rows) {
        if (!load(row)) {
            harnessFailure(std::string(goals) + ": the client never read the status for " + row.what);
            continue;
        }
        const QString want = row.n > 0 ? approvedNotice(row.n) : QString();
        expect(rig.applier->frameRateAboveScreens() == row.above && rig.applier->frameRateNotice() == want,
               QStringLiteral("S15.911: %1: the row shows %2 and the notice reads \"%3\", not %4 and \"%5\"")
                   .arg(QLatin1StringView(row.what))
                   .arg(rig.applier->frameRateAboveScreens())
                   .arg(rig.applier->frameRateNotice())
                   .arg(row.above)
                   .arg(want));
        ++graded;
    }
    expect(graded == qsizetype(std::size(rows)), QStringLiteral("S15.911: %1 of %2 notice rows were graded").arg(graded).arg(std::size(rows)));

    // GOAL: S15.911 -- with no daemon, neither the notice nor the row shows, though the last status
    //       read had a screen over its refresh rate.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^frameRateNotice$':
    //       only both layers broken together go red -- screensAboveRefresh()'s daemon-state test deleted
    //       (app/daemon/apply_controller.cpp) and the client keeping the last status when the daemon
    //       leaves (app/daemon/daemon_client.cpp) -> "FAIL: S15.911: with no daemon (gone 1) the row
    //       shows 1 ...". The state test deleted alone stays green: the client empties its status then.
    if (!load(rows[0]) || rig.applier->frameRateNotice() != approvedNotice(60)) {
        harnessFailure(std::string(goals) + ": the notice was not showing before the daemon left, so its going cannot be graded");
        return;
    }
    fake.stop();
    const bool gone = waitUntil([&] { return rig.client->state() == DaemonClient::State::Unavailable; }, kWaitMs);
    expect(gone && !rig.applier->frameRateAboveScreens() && rig.applier->frameRateNotice().isEmpty(),
           QStringLiteral("S15.911: with no daemon (gone %1) the row shows %2 and the notice reads \"%3\"")
               .arg(gone).arg(rig.applier->frameRateAboveScreens()).arg(rig.applier->frameRateNotice()));
    if (const QString why = fake.start();
        !why.isEmpty() || !waitUntil([&] { return rig.client->state() == DaemonClient::State::Ready; }, kWaitMs)) {
        harnessFailure(std::string(goals) + ": the daemon stand-in did not come back: " + why.toStdString());
        return;
    }

    // GOAL: S15.911 -- unticking keep-higher shows the notice as soon as the daemon accepts it, not one
    //       status read later, and the notice does not disappear while that read lands.
    // WHY-THIS-SHAPE: GetStatus is held from before the change, so the notice at Done can only come from
    //       what the reply recorded; every value the notice signal carries from then on is kept, so a
    //       blank between the reply and the read is caught even if the end state is right.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^frameRateNotice$',
    //       DaemonClient::setKeepHigherFrameRate recording nothing on the reply (app/daemon/daemon_client.cpp)
    //       -> "FAIL: S15.911: after keep-higher was unticked (done 1, status read held 1) the notice read ""
    //       with the read held ...".
    Row keep = rows[0];
    keep.keepHigher = true;
    if (!load(keep) || !rig.applier->frameRateAboveScreens() || !rig.applier->frameRateNotice().isEmpty()) {
        harnessFailure(std::string(goals) + ": keep-higher on did not leave the row shown and the notice hidden to start from");
        return;
    }
    QStringList seen;
    const auto watch = QObject::connect(rig.applier.get(), &ApplyController::frameRateNoticeChanged,
                                        [&] { seen.append(rig.applier->frameRateNotice()); });
    fake.hold = {QStringLiteral("GetStatus")};
    rig.applier->setKeepHigherFrameRate(false);
    const bool done = waitUntil([&] { return rig.applier->keepHigherState() == Progress::Done; }, kWaitMs);
    const QString atDone = rig.applier->frameRateNotice();
    const bool statusHeld = waitUntil([&] {
        return std::ranges::any_of(fake.held, [](const QDBusMessage& m) { return m.member() == u"GetStatus"; });
    }, kWaitMs);
    fake.hold.clear();
    fake.release();
    const bool settled = quiet(rig);
    QObject::disconnect(watch);
    const auto shown = std::ranges::find_if(seen, [](const QString& s) { return !s.isEmpty(); });
    const bool blanked = std::any_of(shown, seen.end(), [](const QString& s) { return s.isEmpty(); });
    expect(done && statusHeld && settled && atDone == approvedNotice(60) && !blanked && rig.applier->frameRateNotice() == approvedNotice(60),
           QStringLiteral("S15.911: after keep-higher was unticked (done %1, status read held %2) the notice read \"%3\" with the read "
                          "held, blanked %4 before it landed, and reads \"%5\"")
               .arg(done).arg(statusHeld).arg(atDone).arg(blanked).arg(rig.applier->frameRateNotice()));
}

void processorNotice() {
    constexpr const char* goals = "S15.951";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("processor"))) {
        return;
    }
    const bool applied = rig.apply() && rig.applier->applyState() == Progress::Done;
    const QList<FakeCall> set = rig.sent("SetWallpaperWithProperties");
    if (!applied || set.isEmpty() || !quiet(rig) || !rig.applier->processorNotice().isEmpty()) {
        harnessFailure(std::string(goals) + ": ocean was not applied to DP-1 with no notice shown to start from");
        return;
    }
    const int monitorId = set.last().args.value(2).toInt();
    // The approved sentences, written out here rather than taken from the app.
    const QString parts = QStringLiteral(
        "Parts of this video can't be played by your graphics card, so WallpaperX will use the processor for them, which "
        "costs more power. Re-saving the video with a video editor usually fixes this.");
    const QString all = QStringLiteral(
        "Your graphics card can't decode this video, so WallpaperX will use the processor for all of it, which costs "
        "more power.");

    // What the view's label is given: the notice read at each processorNoticeChanged, which is the only time the QML
    // binding reads it again.
    QStringList told;
    const auto watch = QObject::connect(rig.applier.get(), &ApplyController::processorNoticeChanged,
                                        [&] { told.append(rig.applier->processorNotice()); });
    const auto label = [&] { return told.isEmpty() ? QStringLiteral("(never told)") : told.last(); };
    // Reports `state` for the applied video and waits until the label is told something other than `from`.
    const auto report = [&](const QString& state, const QString& from) {
        rig.daemonFake->processorDecoding.insert(monitorId, state);
        const bool sent = rig.daemonFake->announceProcessorDecodingChanged();
        return sent && waitUntil([&] { return !told.isEmpty() && told.last() != from; }, kWaitMs);
    };

    // GOAL: S15.951 -- after ProcessorDecodingChanged with the applied video reported "parts", the label is told the
    //       approved "parts" sentence; after "all", the approved whole-video sentence.
    // WHY-THIS-SHAPE: graded on what processorNoticeChanged hands the label, not on the getter: the getter is computed
    //       fresh on every call, so it is right whether or not the view is ever told. The app is idle, so only the
    //       signal can make it read GetStatus again.
    // CONTROL: control_batch.sh 2026-10-24 (T18a), --phase '^processorNotice$', each alone, reverted (bytes restored,
    //       green again): in app/daemon/apply_controller.cpp, the statusChanged -> processorNoticeChanged connect deleted
    //       (re-run 2026-10-28, same result) -> "FAIL: S15.951: after "parts" the label is told "(never told)"" while the getter was right; the Share::All
    //       branch disabled -> "FAIL: S15.951: after "all" the label is told "Parts of this video ...". In
    //       app/daemon/daemon_client.cpp: "all" mapped to Share::Parts -> the same "all" line; the ProcessorDecodingChanged
    //       subscription deleted, and separately the onProcessor insert deleted -> the "(never told)" line.
    //       Re-run 2026-10-24 (T19a) for the new "all" sentence: Share::All branch disabled -> the same "all" line;
    //       the "all" sentence put back to the old "...can't play this video's format..." text -> "FAIL: S15.951:
    //       after "all" the label is told "Your graphics card can't play this video's format, ...".
    const bool partsShown = report(QStringLiteral("parts"), QString());
    expect(partsShown && label() == parts && rig.applier->processorNotice() == parts,
           QStringLiteral("S15.951: after \"parts\" the label is told \"%1\"").arg(label()));
    const bool allShown = report(QStringLiteral("all"), parts);
    expect(allShown && label() == all && rig.applier->processorNotice() == all,
           QStringLiteral("S15.951: after \"all\" the label is told \"%1\"").arg(label()));

    // GOAL: S15.951 -- every other state shows nothing: the daemon's checking, none and unknown, and a state this
    //       build does not know.
    // WHY-THIS-SHAPE: each state follows "parts", so the wait ends only when the status carrying it has landed and
    //       changed the label; waiting for an empty label from an empty one would end before the read.
    // CONTROL: control_batch.sh 2026-10-24 (T18a), --phase '^processorNotice$', reverted: daemon_client.cpp's
    //       `decoding == u"parts"` made `decoding != u"all"` (app/daemon/daemon_client.cpp) -> "FAIL: S15.951: after
    //       "checking" the label is told "Parts of this video ..."", and the same for none, unknown and everything_later.
    for (const QString& state : {QStringLiteral("checking"), QStringLiteral("none"), QStringLiteral("unknown"),
                                 QStringLiteral("everything_later")}) {
        const bool back = report(QStringLiteral("parts"), label()) && label() == parts;
        const bool left = back && report(state, parts);
        expect(left && label().isEmpty(), QStringLiteral("S15.951: after \"%1\" the label is told \"%2\" (parts shown first %3)")
                                              .arg(state, label()).arg(back));
    }

    // GOAL: S15.951 -- the notice is the selected screen's and the open wallpaper's: opening another wallpaper or
    //       selecting another screen tells the label it is gone, and going back tells it again.
    // WHY-THIS-SHAPE: the selection signals are emitted on this thread, so each step's emissions are all in `told`
    //       when select() returns; a step counts only if it told the label something.
    // CONTROL: control_batch.sh 2026-10-24 (T18a), --phase '^processorNotice$', each alone, reverted, all in
    //       app/daemon/apply_controller.cpp: the library lambda's `emit processorNoticeChanged();` deleted -> "FAIL:
    //       S15.951: with river open the label is told "(not told)""; the screen lambda's -> "... with HDMI-A-1 selected
    //       the label is told "(not told)""; `use->path != assetPath` dropped (re-run 2026-10-28) -> "... with river open the label is told
    //       "Parts ..."; onProcessor looked up at cbegin() instead of the monitor -> "... with HDMI-A-1 selected the label
    //       is told "Parts ..."" (green until HDMI-A-1 was given a monitor above).
    // HDMI-A-1 is given a monitor of its own, so the step to it reaches the per-monitor lookup rather than ending at a
    // screen the daemon does not know.
    rig.daemonFake->outputs.insert(QStringLiteral("HDMI-A-1"), monitorId + 1);
    if (!report(QStringLiteral("parts"), label()) || label() != parts ||
        !rig.client->status().outputs.contains(QStringLiteral("HDMI-A-1"))) {
        harnessFailure(std::string(goals) + ": the \"parts\" sentence, with HDMI-A-1 known to the daemon, did not come "
                                            "back before the selection steps");
        QObject::disconnect(watch);
        return;
    }
    const auto step = [&](const std::function<void()>& select) {
        const qsizetype before = told.size();
        select();
        return told.size() > before ? label() : QStringLiteral("(not told)");
    };
    const QString riverOpen = step([&] { rig.library->select(rig.river); });
    expect(riverOpen.isEmpty(), QStringLiteral("S15.951: with river open the label is told \"%1\"").arg(riverOpen));
    const QString oceanAgain = step([&] { rig.library->select(rig.ocean); });
    expect(oceanAgain == parts, QStringLiteral("S15.951: with ocean open again the label is told \"%1\"").arg(oceanAgain));
    const QString otherScreen = step([&] { rig.kscreen->select(QStringLiteral("HDMI-A-1")); });
    expect(otherScreen.isEmpty(), QStringLiteral("S15.951: with HDMI-A-1 selected the label is told \"%1\"").arg(otherScreen));
    const QString screenAgain = step([&] { rig.kscreen->select(QStringLiteral("DP-1")); });
    expect(screenAgain == parts, QStringLiteral("S15.951: with DP-1 selected again the label is told \"%1\"").arg(screenAgain));
    QObject::disconnect(watch);
}

void refreshReporter() {
    constexpr const char* goals = "S15.912";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("reporter"))) {
        return;
    }
    FakeDaemon& fake = *rig.daemonFake;
    const QString dp1 = QStringLiteral("DP-1");
    const QString hdmi = QStringLiteral("HDMI-A-1");
    if (!showScreens(rig, screensAt(165, 60))) {
        harnessFailure(std::string(goals) + ": the fake KScreen's screens never reached the client");
        return;
    }
    fake.outputs = {{dp1, 0}};
    RefreshRateReporter reporter(*rig.kscreen, *rig.client);
    const auto reports = [&] { return rig.sent("SetOutputRefreshRate"); };
    const auto lastIs = [&](const QString& name, double hz) {
        const auto r = reports();
        return !r.isEmpty() && r.back().args == QVariantList{name, hz} && r.back().signature == u"sd";
    };
    const QString refused = QStringLiteral("io.wallpaperx.Daemon.Error.InvalidArgs");

    // GOAL: S15.912 -- the reporter sends KScreen's rate only for names the daemon links, once: an
    //       accepted rate is not sent again on later status reads, and HDMI-A-1, which the daemon does
    //       not name, is never sent.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^refreshReporter$',
    //       in app/daemon/refresh_rate_reporter.cpp: sync() also sending every KScreen screen the daemon does
    //       not name -> "FAIL: S15.912: with only DP-1 linked, over three status reads the daemon was sent
    //       [HDMI-A-1 60, DP-1 165]"; the "daemon already holds it" skip deleted -> "... was sent [DP-1 165,
    //       DP-1 165, DP-1 165, ...".
    bool ok = reread(rig);
    for (const bool sound : {false, true}) {
        fake.sound = sound;
        ok = ok && reread(rig) && rig.client->sound() == sound;
    }
    expect(ok && describeReports(reports()) == u"[DP-1 165]" && reports().value(0).signature == u"sd",
           QStringLiteral("S15.912: with only DP-1 linked, over three status reads the daemon was sent %1 (settled %2)")
               .arg(describeReports(reports())).arg(ok));

    // GOAL: S15.912 -- a linked screen that KScreen shows mirrored, with no area, or not at all is
    //       reported as 0, and as its rate again once shown.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^refreshReporter$',
    //       in app/daemon/refresh_rate_reporter.cpp: wanted() without `selectable()` -> "FAIL: S15.912:
    //       HDMI-A-1 mirrored (staged 1): sent 0 0 ..." and the same for "disabled with no area"; wanted()
    //       giving 30 for a screen KScreen does not list -> those two and "HDMI-A-1 gone (staged 1): sent 0 0".
    // COVERAGE-ASSERT: each of the three cases was staged and graded.
    fake.outputs.insert(hdmi, 1);
    if (!reread(rig) || !lastIs(hdmi, 60)) {
        harnessFailure(std::string(goals) + ": linking HDMI-A-1 did not report its 60 Hz, so its going cannot be graded: " +
                       describeReports(reports()).toStdString());
        return;
    }
    int cases = 0;
    for (const auto& [kind, what] : {std::pair{Hdmi::Mirrored, "mirrored"}, std::pair{Hdmi::ZeroArea, "disabled with no area"},
                                     std::pair{Hdmi::Gone, "gone"}}) {
        const bool staged = showScreens(rig, screensAt(165, 60, kind)) && quiet(rig);
        const bool zero = staged && lastIs(hdmi, 0) && !fake.refreshRates.contains(hdmi);
        const bool back = showScreens(rig, screensAt(165, 60)) && quiet(rig) && lastIs(hdmi, 60);
        expect(staged && zero && back, QStringLiteral("S15.912: HDMI-A-1 %1 (staged %2): sent 0 %3, its 60 again %4; all sent %5")
                                           .arg(QLatin1StringView(what)).arg(staged).arg(zero).arg(back).arg(describeReports(reports())));
        ++cases;
    }
    expect(cases == 3, QStringLiteral("S15.912: %1 of 3 hidden-screen cases were graded").arg(cases));

    // GOAL: S15.912 -- a refused rate is not sent again when unrelated status changes follow, and is
    //       sent again once KScreen reports another rate for that screen.
    // WHY-THIS-SHAPE: the calls are counted across three unrelated reads that each provably reached the
    //       client, so "no resend" is not the absence of reads.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^refreshReporter$',
    //       in app/daemon/refresh_rate_reporter.cpp: the Failed entry never blocking a resend -> "FAIL:
    //       S15.912: after DP-1's 144 was refused (sent 1), 3 of 3 unrelated status reads sent 18 more";
    //       the block ignoring KScreen's rate -> "... after the refusal KScreen's new 120 for DP-1 was not
    //       given to the daemon".
    fake.refuse.insert(QStringLiteral("SetOutputRefreshRate"), {refused, QStringLiteral("no")});
    const bool refusedSent = showScreens(rig, screensAt(144, 60)) && quiet(rig) && lastIs(dp1, 144);
    const qsizetype afterRefusal = reports().size();
    int unrelated = 0;
    for (const bool sound : {false, true, false}) {
        fake.sound = sound;
        unrelated += reread(rig) && rig.client->sound() == sound ? 1 : 0;
    }
    expect(refusedSent && unrelated == 3 && reports().size() == afterRefusal,
           QStringLiteral("S15.912: after DP-1's 144 was refused (sent %1), %2 of 3 unrelated status reads sent %3 more: %4")
               .arg(refusedSent).arg(unrelated).arg(reports().size() - afterRefusal).arg(describeReports(reports())));
    fake.refuse.clear();
    const bool resent = showScreens(rig, screensAt(120, 60)) && quiet(rig) && lastIs(dp1, 120) && fake.refreshRates.value(dp1) == 120;
    expect(resent, QStringLiteral("S15.912: after the refusal KScreen's new 120 for DP-1 was not given to the daemon: %1")
                       .arg(describeReports(reports())));

    // GOAL: S15.912 -- a name the daemon refused, then unlinked and linked again, is sent again.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^refreshReporter$',
    //       the removeIf forgetting unlisted failures deleted (app/daemon/refresh_rate_reporter.cpp) -> "FAIL:
    //       S15.912: HDMI-A-1 refused (staged 1), unlinked and linked again was sent 0 more".
    fake.outputs.remove(hdmi);
    fake.refreshRates.remove(hdmi);
    bool staged = reread(rig);
    fake.refuse.insert(QStringLiteral("SetOutputRefreshRate"), {refused, QStringLiteral("no")});
    fake.outputs.insert(hdmi, 1);
    staged = staged && reread(rig) && lastIs(hdmi, 60);
    fake.refuse.clear();
    const qsizetype beforeRelink = reports().size();
    fake.outputs.remove(hdmi);
    staged = staged && reread(rig);
    fake.outputs.insert(hdmi, 1);
    const bool relinked = staged && reread(rig) && reports().size() == beforeRelink + 1 && lastIs(hdmi, 60) && fake.refreshRates.value(hdmi) == 60;
    expect(relinked, QStringLiteral("S15.912: HDMI-A-1 refused (staged %1), unlinked and linked again was sent %2 more: %3")
                         .arg(staged).arg(reports().size() - beforeRelink).arg(describeReports(reports())));

    // `forget`: the new daemon knows no refresh rates, as the real one does not save them.
    const auto restart = [&](bool forget) {
        fake.stop();
        const bool gone = waitUntil([&] { return rig.client->state() == DaemonClient::State::Unavailable; }, kWaitMs);
        if (forget) {
            fake.refreshRates.clear();
        }
        return gone && fake.start().isEmpty() && waitUntil([&] { return rig.client->state() == DaemonClient::State::Ready; }, kWaitMs);
    };

    // GOAL: S15.912 -- a rate the old daemon refused is sent to a daemon that starts, even when the new
    //       daemon's status reads the same as the old one's.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^refreshReporter$',
    //       `++m_daemonStarts;` deleted (app/daemon/refresh_rate_reporter.cpp) -> "FAIL: S15.912: DP-1's
    //       refused 100 (staged 1) after a daemon restart was sent 0 more".
    fake.refuse.insert(QStringLiteral("SetOutputRefreshRate"), {refused, QStringLiteral("no")});
    const bool refused100 = showScreens(rig, screensAt(100, 60)) && quiet(rig) && lastIs(dp1, 100);
    fake.refuse.clear();
    const qsizetype beforeRestart = reports().size();
    const bool restarted = refused100 && restart(false) && quiet(rig);
    expect(restarted && reports().size() > beforeRestart && lastIs(dp1, 100) && fake.refreshRates.value(dp1) == 100,
           QStringLiteral("S15.912: DP-1's refused 100 (staged %1) after a daemon restart was sent %2 more: %3")
               .arg(restarted).arg(reports().size() - beforeRestart).arg(describeReports(reports())));

    // GOAL: S15.912 -- a call the old daemon fails after a new one has started is sent again to the new
    //       one.
    // WHY-THIS-SHAPE: the call is held across the restart and its failure released only after the new
    //       daemon is Ready, so the failure arrives when the reporter already counts the new daemon.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^refreshReporter$',
    //       in app/daemon/refresh_rate_reporter.cpp: the block no longer comparing which daemon the call
    //       went to -> "FAIL: S15.912: DP-1's 90 failed by the old daemon after the new one started (held 1,
    //       back 1) was sent 0 more"; the callback's closing `sync();` deleted -> the same line.
    fake.hold = {QStringLiteral("SetOutputRefreshRate")};
    const bool held = showScreens(rig, screensAt(90, 60)) && waitUntil([&] { return fake.held.size() == 1; }, kWaitMs);
    const bool backAgain = held && restart(true) && quiet(rig);
    const qsizetype beforeFailure = reports().size();
    fake.hold.clear();
    fake.refuseAt(0, refused, QStringLiteral("gone"));
    const bool midCall = backAgain && quiet(rig) && reports().size() == beforeFailure + 1 && lastIs(dp1, 90) && fake.refreshRates.value(dp1) == 90;
    expect(midCall, QStringLiteral("S15.912: DP-1's 90 failed by the old daemon after the new one started (held %1, back %2) was sent "
                                   "%3 more: %4")
                        .arg(held).arg(backAgain).arg(reports().size() - beforeFailure).arg(describeReports(reports())));

    // GOAL: S15.912 -- the reporter does not poll: with nothing changing, nothing is sent or read.
    // WHY-THIS-SHAPE: an absence over 1.5 s; it catches a poll at any interval under that, not a slower
    //       one. The window is a fixed wait by nature, and an empty call list is the only pass.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^refreshReporter$',
    //       a 200 ms QTimer calling DaemonClient::refresh added to the reporter's constructor
    //       (app/daemon/refresh_rate_reporter.cpp) -> "FAIL: S15.912: with nothing changing for 1.5 s the
    //       daemon received [GetStatus,GetStatus,...]".
    const qsizetype calm = fake.calls.size();
    const bool calmStarted = quiet(rig);
    const qsizetype base = fake.calls.size();
    waitUntil([] { return false; }, 1500);
    QStringList extra;
    for (qsizetype i = base; i < fake.calls.size(); ++i) {
        extra.append(fake.calls[i].member);
    }
    expect(calmStarted && calm <= base && extra.isEmpty(),
           QStringLiteral("S15.912: with nothing changing for 1.5 s the daemon received [%1]").arg(extra.join(u',')));
}

void frameRateSetters() {
    constexpr const char* goals = "S15.913";
    Rig rig;
    if (!build(rig, goals, QStringLiteral("framerates"))) {
        return;
    }
    FakeDaemon& fake = *rig.daemonFake;
    // GOAL: S15.913 -- setFrameRate, setScreenFrameRate (0 = "Same as all screens") and
    //       setKeepHigherFrameRate reach the daemon typed as it takes them, end Done, and the client
    //       then reads what was set; a rate outside 30/60/120 is not sent.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^frameRateSetters$',
    //       in app/daemon/daemon_client.cpp unless named: SetFrameRate sent `rate / 2` -> "FAIL: S15.913:
    //       setFrameRate(120) ended state 2 and sent i ..."; SetMonitorFrameRate sent 60 -> "...
    //       setScreenFrameRate(30) ..."; SetKeepHigherFrameRate sent `!keep` -> "... setKeepHigherFrameRate
    //       (true) ..."; setScreenFrameRate refusing 0 (app/daemon/apply_controller.cpp) -> "...
    //       setScreenFrameRate(0) ended state 2 and sent (none or several) (0 calls)"; setFrameRate's
    //       allowed-rate test deleted (app/daemon/apply_controller.cpp) -> "... a rate of 45 sent [SetFrameRate]".
    struct Step {
        const char* what;
        std::function<void()> act;
        std::function<Progress()> state;
        const char* member;
        const char* signature;
        std::function<QVariantList()> args;
        std::function<bool()> read;
    };
    const auto dp1Id = [&] { return fake.outputs.value(QStringLiteral("DP-1"), -1); };
    const auto screenRate = [&] { return rig.client->screenFrameRates().value(QStringLiteral("DP-1")); };
    const Step steps[] = {
        {"setFrameRate(120)", [&] { rig.applier->setFrameRate(120); }, [&] { return rig.applier->frameRateState(); }, "SetFrameRate", "i",
         [] { return QVariantList{120}; }, [&] { return rig.client->frameRate() == 120 && fake.frameRate == QJsonValue(120); }},
        {"setScreenFrameRate(30)", [&] { rig.applier->setScreenFrameRate(30); }, [&] { return rig.applier->screenFrameRateState(); },
         "SetMonitorFrameRate", "ii", [&] { return QVariantList{dp1Id(), 30}; },
         [&] { return screenRate() == QVariant(30) && fake.screenFrameRate.value(dp1Id()) == QJsonValue(30); }},
        {"setScreenFrameRate(0)", [&] { rig.applier->setScreenFrameRate(0); }, [&] { return rig.applier->screenFrameRateState(); },
         "SetMonitorFrameRate", "ii", [&] { return QVariantList{dp1Id(), 0}; },
         [&] { return screenRate() == QVariant(0) && !fake.screenFrameRate.contains(dp1Id()); }},
        {"setKeepHigherFrameRate(true)", [&] { rig.applier->setKeepHigherFrameRate(true); }, [&] { return rig.applier->keepHigherState(); },
         "SetKeepHigherFrameRate", "b", [] { return QVariantList{true}; },
         [&] { return rig.client->keepHigherFrameRate() && fake.keepHigherFrameRate == QJsonValue(true); }},
    };
    for (const Step& step : steps) {
        const qsizetype before = fake.named(QString::fromLatin1(step.member)).size();
        step.act();
        const bool done = waitUntil([&] { return step.state() != Progress::Working; }, kWaitMs) && quiet(rig);
        const auto sent = fake.named(QString::fromLatin1(step.member));
        const bool one = sent.size() == before + 1;
        QStringList args;
        for (const QVariant& a : one ? sent.back().args : QVariantList{}) {
            args.append(a.toString());
        }
        const QString got = one ? sent.back().signature + u' ' + args.join(u',') : QStringLiteral("(none or several)");
        expect(done && step.state() == Progress::Done && one && sent.back().signature == QLatin1StringView(step.signature) &&
                   sent.back().args == step.args() && step.read(),
               QStringLiteral("S15.913: %1 ended state %2 and sent %3 (%4 calls); the client and the daemon read it %5")
                   .arg(QLatin1StringView(step.what)).arg(int(step.state())).arg(got).arg(sent.size() - before).arg(step.read()));
    }
    const qsizetype allBefore = fake.calls.size();
    rig.applier->setFrameRate(45);
    rig.applier->setScreenFrameRate(45);
    const bool drained = rig.daemonDrained();
    QStringList stray;
    for (qsizetype i = allBefore; i < fake.calls.size(); ++i) {
        if (fake.calls[i].member != u"Marker") {
            stray.append(fake.calls[i].member);
        }
    }
    expect(drained && stray.isEmpty(), QStringLiteral("S15.913: a rate of 45 sent [%1]").arg(stray.join(u',')));

    // GOAL: S15.913 -- a refused frame-rate change says which setting and the daemon's reason, worded as
    //       the sound and pause refusals are.
    // CONTROL: control_batch.sh, 2026-10-20, each alone, reverted (bytes restored, green again), --phase '^frameRateSetters$',
    //       each of the three sentence starts reworded in app/daemon/apply_controller.cpp -> "FAIL: S15.913:
    //       the refusals (failed 1) read "The frame rate was not set. ...", "... The screen rate for DP-1
    //       ..." and "... Keep-higher was not changed. ...".
    fake.refuse.insert(QStringLiteral("SetFrameRate"), {QStringLiteral("io.wallpaperx.Daemon.Error.RenderFailed"), QStringLiteral("disk full")});
    fake.refuse.insert(QStringLiteral("SetMonitorFrameRate"), {QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), QStringLiteral("no")});
    fake.refuse.insert(QStringLiteral("SetKeepHigherFrameRate"), {QStringLiteral("io.wallpaperx.Daemon.Error.RenderFailed"), QStringLiteral("disk full")});
    rig.applier->setFrameRate(30);
    rig.applier->setScreenFrameRate(60);
    rig.applier->setKeepHigherFrameRate(false);
    const bool failed = waitUntil([&] {
        return rig.applier->frameRateState() == Progress::Failed && rig.applier->screenFrameRateState() == Progress::Failed &&
               rig.applier->keepHigherState() == Progress::Failed;
    }, kWaitMs);
    expect(failed && rig.applier->frameRateMessage() == u"The frame rate was not changed for every screen. The WallpaperX service could not do it: disk full" &&
               rig.applier->screenFrameRateMessage() == u"The frame rate for DP-1 was not changed. The WallpaperX service refused it: no" &&
               rig.applier->keepHigherMessage() == u"Keeping a higher frame rate was not changed. The WallpaperX service could not do it: disk full",
           QStringLiteral("S15.913: the refusals (failed %1) read \"%2\", \"%3\" and \"%4\"")
               .arg(failed).arg(rig.applier->frameRateMessage(), rig.applier->screenFrameRateMessage(), rig.applier->keepHigherMessage()));
    fake.refuse.clear();
}

} // namespace

int main(int argc, char* argv[]) {
    wallpaperx::testing::g_testName = "apply";
    if (const auto refused = refuseUnlessUnderDbusRunSession()) {
        return *refused;
    }
    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        notVerified("S15.226-S15.253, S15.256-S15.264", "no temporary folder could be made");
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }
    isolateXdg(scratch.path());
    QCoreApplication app(argc, argv);
    if (const auto refused = refuseUnlessPrivateBus()) {
        return *refused;
    }
    runPhase("activationScriptAlone", activationScriptAlone);
    runPhase("plasmaShell", plasmaShell);
    runPhase("applyOrder", applyOrder);
    runPhase("freeIds", freeIds);
    runPhase("bindQueue", bindQueue);
    runPhase("liveEdits", liveEdits);
    runPhase("positionOnTheWire", positionOnTheWire);
    runPhase("failureSentences", failureSentences);
    runPhase("pauseModes", pauseModes);
    runPhase("daemonPresence", daemonPresence);
    runPhase("daemonReturn", daemonReturn);
    runPhase("persistenceSignal", persistenceSignal);
    runPhase("lateReplies", lateReplies);
    runPhase("pauseScope", pauseScope);
    runPhase("applyScope", applyScope);
    runPhase("bindAndRefresh", bindAndRefresh);
    runPhase("longConnectors", longConnectors);
    runPhase("liveAcrossApplies", liveAcrossApplies);
    runPhase("selectedScreenEdits", selectedScreenEdits);
    runPhase("liveFollowsStatus", liveFollowsStatus);
    runPhase("linkHandOver", linkHandOver);
    runPhase("applyReturn", applyReturn);
    runPhase("applyReopen", applyReopen);
    runPhase("bindReadFails", bindReadFails);
    runPhase("errorText", errorText);
    runPhase("slowStatusRefresh", slowStatusRefresh);
    runPhase("slowStatusBind", slowStatusBind);
    runPhase("statusNoAnswer", statusNoAnswer);
    runPhase("soundStatus", soundStatus);
    runPhase("soundSetters", soundSetters);
    runPhase("frameRateNotice", frameRateNotice);
    runPhase("processorNotice", processorNotice);
    runPhase("refreshReporter", refreshReporter);
    runPhase("frameRateSetters", frameRateSetters);

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its stand-ins, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
