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
        kscreen = std::make_unique<KScreenClient>(QDBusConnection::sessionBus(), kscreenFake->service());
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
    //       given only `speed` (re-run 2026-10-18) -> red, "S15.237: with Plasma refusing (live on "DP-1"),
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
    // CONTROL: app/daemon/apply_controller.cpp, sendLive()'s `if (m_sending != Sending::No)` made
    //       `if (false)` (control_batch, 2026-10-04) -> red, "S15.238: a burst of 10 edits made while a
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
    //       `m_liveError = QString();` (control_batch, re-run 2026-10-15) -> red, "S15.241: the refused edit
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
    // CONTROL: app/daemon/apply_controller.cpp, control_batch, re-run 2026-10-17: sendLive()'s
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
    //       land after the clear and read as an edit made after reopening.
    // CONTROL: app/daemon/apply_controller.cpp, `endLiveUnlessOpen();` in the selectionChanged
    //       handler deleted (control_batch, re-run 2026-10-15) -> red, "S15.239: closed and reopened, live on
    //       "DP-1", an edit sent 0 call(s)" (the edit queued behind the in-flight send of the cleared
    //       values) and "with another wallpaper opened, live on "DP-1", its edit sent 1 call(s)".
    rig.library->clearSelection();
    const bool closedEnded = rig.applier->liveOutput().isEmpty();
    rig.library->select(rig.ocean);
    const bool earlierArrived = rig.daemonDrained();
    rig.daemonFake->calls.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool reopenDrained = rig.daemonDrained();
    expect(earlierArrived && closedEnded && reopenDrained && rig.applier->liveOutput().isEmpty() && edits().isEmpty(),
           QStringLiteral("S15.239: closed and reopened, live on \"%1\", an edit sent %2 call(s)").arg(rig.applier->liveOutput()).arg(edits().size()));
    const bool reapplied = rig.apply();
    rig.library->select(rig.river);
    rig.daemonFake->calls.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 2.0);
    const bool otherDrained = rig.daemonDrained();
    expect(reapplied && otherDrained && rig.applier->liveOutput().isEmpty() && edits().isEmpty(),
           QStringLiteral("S15.239: with another wallpaper opened, live on \"%1\", its edit sent %2 call(s)").arg(rig.applier->liveOutput()).arg(edits().size()));

    // GOAL: S15.240 -- an edit made while Apply is running is sent once the daemon accepts it.
    // WHY-THIS-SHAPE: the daemon's reply to SetWallpaperWithProperties is held, so the edit is
    //       provably made while Apply is running.
    // CONTROL: app/daemon/apply_controller.cpp, startLive()'s `sendLive();` deleted (control_batch,
    //       re-run 2026-10-15) -> red, "S15.240: the edit made during Apply was sent as [] (held 1, landed 1)".
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
    // CONTROL: app/library/wpx_package.cpp Wallpaper::accept() keeping `value` for a number (control_batch
    //       2026-10-18, reverted) -> red, "S15.732: Apply sent {..., position.zoom=2(int), ...} (1 call(s)), not
    //       {..., position.zoom=2(double), ...}", and the S15.733 and S15.734 lines likewise.
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
    // CONTROL: the same break -> red, "S15.733: the live edit sent {..., position.y=1(int), position.zoom=2(int),
    //       ...} (1 call(s)), not {..., position.y=1(double), position.zoom=2(double), ...}".
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
    // CONTROL: app/daemon/daemon_client.cpp `emit serviceStarted();` deleted (control_batch 2026-10-18,
    //       reverted) -> red, "S15.734: after the daemon returned it received {} (0 call(s)), not {..., position=fit(QString), ...}".
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
    //       (control_batch, 2026-10-04) -> red, the four daemon rows, e.g. "S15.244: daemon not
    //       running gave "..." (state 3, live on "" before "DP-1")".
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
        row.arrange();
        const bool finished = rig.apply();
        ++ran;
        const QString liveAfter = rig.applier->liveOutput();
        const bool liveRight = row.daemonAccepted ? liveAfter == u"DP-1" : liveAfter == liveBefore;
        expect(finished && rig.applier->applyState() == Progress::Failed && rig.applier->applyMessage() == row.sentence && liveRight,
               QStringLiteral("S15.244: %1 gave \"%2\" (state %3, live on \"%4\" before \"%5\"), not \"%6\"")
                   .arg(QLatin1StringView(row.what), rig.applier->applyMessage())
                   .arg(int(rig.applier->applyState()))
                   .arg(liveAfter, liveBefore, row.sentence));
        d.release();
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
    //       serviceStarted();` deleted -> red, "S15.647: after the daemon returned it received [] naming the
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
    //       deleted (control_batch, re-run 2026-10-17) -> red, "S15.648: after a restart following an accepted
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
    // CONTROL: control_batch, 2026-10-17, each reverted: app/daemon/daemon_client.cpp, the constructor's
    //       PersistenceChanged connection deleted -> red, "S15.689: after PersistenceChanged announcing a
    //       failed write the app shows "" (0 status read(s))"; DaemonClient::refresh() returning at once while
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
    // Every call an edit made has reached the stand-in, and every reply the app was owed has been
    // handled, so a resend it made on one has reached the stand-in too.
    const auto settled = [&] { return rig.daemonDrained() && rig.repliesHandled() && rig.daemonDrained(); };
    const auto edits = [&] { return rig.sent("SetWallpaperProperties"); };
    if (!rig.apply() || rig.applier->applyState() != Progress::Done || rig.applier->liveOutput() != u"DP-1") {
        harnessFailure(std::string(goals) + ": Ocean Waves could not be applied to DP-1 to start from: " +
                       rig.applier->applyMessage().toStdString());
        return;
    }
    const int dp1 = d.outputs.value(QStringLiteral("DP-1"), -1);

    // GOAL: S15.256 -- an Apply to another screen that fails, at the bind or at the daemon, leaves
    //       the live link where it was: DP-1 stays live, and an edit then sends exactly one
    //       SetWallpaperProperties, to DP-1's id.
    // WHY-THIS-SHAPE: both failure points, because the link could be dropped before the bind or
    //       only once the daemon is reached. The count is read once every reply has been handled,
    //       so a second call the edit caused would have arrived.
    // CONTROL: app/daemon/apply_controller.cpp, `if (m_live) { m_live.reset(); m_liveError.clear();
    //       emit liveChanged(); }` put back before `m_applyWallpaper = ...` in apply() (control_batch,
    //       re-run 2026-10-15) -> red, "S15.256: after an Apply to HDMI-A-1 refused at SetMonitorOutput
    //       (failed 1), live on "" and an edit on DP-1 sent [], not [0]" and the same at
    //       SetWallpaperWithProperties.
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
        const bool quiet = settled();
        d.calls.clear();
        rig.library->setPropertyValue(QStringLiteral("speed"), r.speed);
        const bool after = settled();
        QStringList sent;
        for (const FakeCall& c : edits()) {
            sent.append(c.args.value(0).toString());
        }
        expect(failed && quiet && after && live == u"DP-1" && sent == QStringList{QString::number(dp1)},
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
    const bool quiet = settled();
    d.calls.clear();
    rig.library->setPropertyValue(QStringLiteral("speed"), 0.5);
    const bool after = settled();
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
    const bool landed = waitUntil([&] { return rig.applier->applyState() != Progress::Working; }, kWaitMs) && settled();
    const QStringList calls = wallpaperCalls(d);
    const auto applyCall = std::ranges::find_if(calls, [](const QString& c) { return c.startsWith(u"SetWallpaperWithProperties "); });
    const qsizetype applyAt = applyCall == calls.end() ? -1 : applyCall - calls.begin();
    expect(bindHeld && editFirst && landed && rig.applier->applyState() == Progress::Done && applyAt > 0 &&
               !calls.isEmpty() && calls.back().endsWith(QStringLiteral(" 2")),
           QStringLiteral("S15.258: the daemon received [%1]; the last should carry speed 2 (bind held %2, edit first %3, landed %4)")
               .arg(calls.join(QStringLiteral(", "))).arg(bindHeld).arg(editFirst).arg(landed));
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
    runPhase("bindReadFails", bindReadFails);
    runPhase("errorText", errorText);
    runPhase("slowStatusRefresh", slowStatusRefresh);
    runPhase("slowStatusBind", slowStatusBind);
    runPhase("statusNoAnswer", statusNoAnswer);
    runPhase("soundStatus", soundStatus);
    runPhase("soundSetters", soundSetters);
    runPhase("frameRateNotice", frameRateNotice);
    runPhase("refreshReporter", refreshReporter);
    runPhase("frameRateSetters", frameRateSetters);

    const bool failed = wallpaperx::testing::reportCheckFailures();
    const bool faulted = wallpaperx::testing::reportHarnessFaults("in this test file or its stand-ins, not in the app");
    if (failed || faulted) {
        return 1;
    }
    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
