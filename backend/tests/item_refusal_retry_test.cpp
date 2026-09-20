// SPDX-License-Identifier: GPL-2.0-or-later

// What the KDE plugin does when the daemon REFUSES a hand-over rather than failing one.
//
// The daemon keeps one image per monitor free to draw into and bounds what one consumer may
// hold, and it takes nothing back: every bound is a refusal, answered as
// io.wallpaperx.Daemon.Error.NoFreeImage. A refusal is temporary, and the daemon announces a
// frame whenever an image comes back -- but that announcement is produced by some consumer
// RELEASING, and a consumer that has stopped releasing sends none. So an item that has nothing on
// screen and is refused cannot simply wait: it would show the plain backdrop with no message and
// never ask again.
//
// Two answers, graded against each other in one process, because the difference between them is
// the whole property:
//
//   * NoFreeImage with nothing on screen -- say so, and ask again on a timer.
//   * NoFrame, the ordinary state before the user has ever set a wallpaper -- say so, and do NOT
//     ask again. Nothing is coming until the user acts, and a plugin that polled here would ask
//     the daemon forever on a fresh install.
//
// One process, two scenes, because "it re-asks" and "it does not re-ask" are the same measurement
// with opposite expectations: read in separate runs, a wait that was silently too short would
// make the second pass and only the first fail, and the pair would still look like coverage.
//
// A WINDOW WITH NO SCENE GRAPH RUNNING, which is an exact requirement of this file and not a
// convenience. The item fetches only while it is on the screen -- in a window and visible -- so an
// item parented into nothing never reaches the bus at all and every check here would grade a
// silence the plugin is right to keep. The window is a QQuickRenderControl one that is never
// initialised and never rendered into, so joining it costs exactly ONE fetch (the in-flight latch
// coalesces the scene-change re-ask into the visibility one) and nothing afterwards can produce a
// request but the retry timer this file is about. No OpenGL is needed and the run skips on nothing
// but a missing bus.

#include "tests/test_report.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace {

using wallpaperx::testing::check;
using wallpaperx::testing::notVerified;

constexpr const char* kService = "io.wallpaperx.Daemon";
constexpr const char* kPath = "/io/wallpaperx/Daemon";
constexpr const char* kInterface = "io.wallpaperx.Daemon";

// The plugin's own constant for how long a refused item with nothing on screen waits. Written out
// rather than included from the plugin: comparing the plugin against its own number would make
// "it asks again within a couple of seconds" unfalsifiable, since a change to it would move both
// sides of every wait below at once.
constexpr int kExpectedRetryMs = 2000;

// Long enough for at least one retry to have happened and been observed, with margin for a busy
// machine. Used for BOTH the "it re-asked" wait and the "it did not re-ask" span, so the two
// answers are read over the same window and the negative one cannot pass by being asked sooner.
constexpr int kRetryWindowMs = 3 * kExpectedRetryMs;

// Answers every GetFrameFd with one chosen error, and counts the asking. No frame is ever served:
// what is under test is entirely what the item does with a refusal.
class RefusingStub : public QDBusVirtualObject {
public:
    explicit RefusingStub(QString errorName) : m_errorName(std::move(errorName)) {}

    void answerWith(QString errorName) { m_errorName = std::move(errorName); }

    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface)) return false;
        if (message.member() != QLatin1String("GetFrameFd")) return false;
        ++m_requests;
        return connection.send(message.createErrorReply(
            m_errorName, QStringLiteral("this stub answers every hand-over with the same refusal")));
    }

    int requests() const { return m_requests; }

private:
    QString m_errorName;
    int m_requests = 0;
};

// Pumps the event loop until `done`, or gives up. Returns whether it happened -- every caller
// branches on it, so an expiry can never be read as the thing having occurred.
bool pumpUntil(const std::function<bool()>& done, int limitMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > limitMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

// Pumps for a fixed span, waiting for nothing. Separate from pumpUntil so no call site can
// discard a "it never happened" answer by accident.
void pumpFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// publishedReason is a BINDING, not a read: it updates only if the item emits
// unavailableReasonChanged, so a property that changed silently would leave it stale.
QString qmlSource() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 64
    height: 64
    property string publishedReason: renderer.unavailableReason
    property bool rendererDrawing: renderer.frameShown

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
    }
}
)");
}

// One item, parented into `window` so that it counts as on the screen and therefore fetches at
// all. Returned rather than built inline so each phase gets a fresh one: the retry timer belongs
// to the item, and a phase reusing the previous phase's item would be reading a timer armed under
// the other answer.
std::unique_ptr<QObject> makeItem(QQmlEngine& engine, QQuickWindow& window, const char* fileName) {
    QQmlComponent component(&engine);
    component.setData(qmlSource().toUtf8(),
                      QUrl::fromLocalFile(QCoreApplication::applicationDirPath() + QLatin1Char('/') +
                                          QString::fromUtf8(fileName)));
    std::unique_ptr<QObject> root(component.create());
    if (root == nullptr) {
        for (const QQmlError& error : component.errors()) {
            std::fprintf(stderr, "  qml: %s\n", qPrintable(error.toString()));
        }
        return root;
    }
    auto* rootItem = qobject_cast<QQuickItem*>(root.get());
    if (rootItem == nullptr) {
        wallpaperx::testing::harnessFailure("the QML root is not a QQuickItem, so it could not be put in a window");
        return {};
    }
    rootItem->setParentItem(window.contentItem());
    return root;
}

}  // namespace

int main(int argc, char** argv) {
    wallpaperx::testing::g_testName = "item_refusal_retry_test";

    QGuiApplication app(argc, argv);

    const char* kGoals = "the refused-hand-over goals (a refusal with nothing on screen is reported and asked again, "
                         "and an ordinary no-frame answer is reported and not polled)";

    QDBusConnection bus = QDBusConnection::sessionBus();
    RefusingStub stub(QStringLiteral("io.wallpaperx.Daemon.Error.NoFreeImage"));
    if (!bus.isConnected() || !bus.registerService(QString::fromUtf8(kService)) ||
        !bus.registerVirtualObject(QString::fromUtf8(kPath), &stub)) {
        notVerified(kGoals, "no session bus this run could own io.wallpaperx.Daemon on: " +
                                bus.lastError().message().toStdString());
        return wallpaperx::testing::reportNotVerifiedOrPassed();
    }

    QQmlEngine engine;
    engine.addImportPath(QString::fromUtf8(WALLPAPERX_QML_MODULE_PATH));

    // A window the items live in, so that they count as being on the screen and fetch at all. Under
    // a render control that is never initialised, so no scene-graph pass ever runs: the item asks
    // for one on every hand-over and the answer must stay "nothing was drawn", or it would be
    // driving its paint watch instead of its retry timer.
    QQuickRenderControl control;
    QQuickWindow window(&control);
    window.setGeometry(0, 0, 64, 64);

    // How many times the item asked while it was being refused, carried out of the first phase so
    // the second phase's "it did not ask again" can be read against a number that is known to be
    // reachable. Without it the second phase's span could be too short and nobody would know.
    int refusedRetries = 0;

    // ---------------------------------------------------------------- //
    // NoFreeImage, with nothing on screen.
    // ---------------------------------------------------------------- //
    {
        std::unique_ptr<QObject> root = makeItem(engine, window, "item_refusal_retry_refused.qml");
        // The module and the type are the plugin's own, so a load failure here is production's,
        // not this harness's -- and every check below is gated on it.
        // CONTROL: the import path replaced with a nonexistent directory -> `qml: ... module
        //       "com.wallpaperx" is not installed` and "FAIL: the com.wallpaperx module loads and
        //       WallpaperXRenderer instantiates". Reverted.
        check(root != nullptr, "the com.wallpaperx module loads and WallpaperXRenderer instantiates");
        if (root == nullptr) {
            notVerified(kGoals, "the plugin's QML type could not be instantiated, so no item was ever refused");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }

        const auto publishedReason = [&root] { return root->property("publishedReason").toString(); };

        // GOAL: an item with nothing on screen that is refused SAYS SO. False for a plugin that
        //       reports nothing, which is the right answer only while a wallpaper is up: with the
        //       backdrop showing, silence is a blank desktop that explains nothing.
        // WHY-THIS-SHAPE: the reason is required to be non-empty and the item is required to have
        //       actually asked, in that order, so a run where the item never reached the bus
        //       reports the asking rather than the reason. The string itself is not compared:
        //       it is translated user-facing prose, and pinning it here would make every wording
        //       change a test failure while establishing nothing more.
        // CONTROL: the `else` arm of `if (m_frameShown)` in WallpaperXItem::takeFrameReply
        //       (backend/kde/wallpaperx_item.cpp) changed to `setFetchReason(QString());` ->
        //       "FAIL: an item with nothing on screen publishes a reason when it is refused",
        //       printing "the item asked 3 time(s) and published []". Reverted.
        const bool refusedAndReported =
            pumpUntil([&] { return stub.requests() > 0 && !publishedReason().isEmpty(); }, 5000);
        check(refusedAndReported, "an item with nothing on screen publishes a reason when it is refused");
        if (!refusedAndReported) {
            std::fprintf(stderr, "  the item asked %d time(s) and published [%s]\n", stub.requests(),
                         publishedReason().toStdString().c_str());
            notVerified(kGoals, "the item was never refused with a reason, so the retry below had nothing under it");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        check(!root->property("rendererDrawing").toBool(),
              "and it is not claiming a frame is on screen, which is the case the retry is for");

        // GOAL: and it asks again on its own. Nothing else will make it: the daemon's
        //       announcement of a freed image is sent when a consumer RELEASES one, and the
        //       consumer holding the ring is by definition one that has stopped releasing.
        //       Without this the screen stays on the backdrop until the user sets a wallpaper.
        // WHY-THIS-SHAPE: no FrameReady is ever sent on this bus, and the window the item lives in
        //       never runs a scene-graph pass, so nothing but the item's own timer can produce a
        //       second request. The obvious shape -- send a FrameReady and check it re-fetches --
        //       grades the announcement path, which already has coverage, and would pass for a
        //       plugin with no timer at all.
        // COVERAGE-ASSERT: the count at the moment the refusal was published is asserted to be
        //       exactly one, so a harness that made the item ask repeatedly on its own -- by
        //       rendering the scene, or by joining and leaving the window -- fails here rather
        //       than crediting those asks to the timer.
        // CONTROL: `retryWhileNothingIsShown();` deleted from the NoFreeImage branch of
        //       WallpaperXItem::takeFrameReply (backend/kde/wallpaperx_item.cpp) -> "FAIL: and it
        //       asks again on its own, with nothing having told it to", printing that it stayed
        //       at 1 request. Reverted.
        const int askedWhenReported = stub.requests();
        check(askedWhenReported == 1, "joining the window cost exactly one fetch, so a second is the timer's");
        if (askedWhenReported != 1) {
            std::fprintf(stderr, "  the item had asked %d time(s) before the retry window opened; something other "
                                 "than the timer is driving it and this phase cannot attribute the next ask\n",
                         askedWhenReported);
        }
        const bool askedAgain = pumpUntil([&] { return stub.requests() > askedWhenReported; }, kRetryWindowMs);
        check(askedAgain, "and it asks again on its own, with nothing having told it to");
        if (!askedAgain) {
            std::fprintf(stderr, "  it stayed at %d request(s) for %d ms after being refused, with no announcement "
                                 "coming\n",
                         stub.requests(), kRetryWindowMs);
        }

        // COVERAGE-ASSERT: how many retries actually landed in the window, carried into the
        // second phase. That phase's claim is that a DIFFERENT answer produces none over the same
        // span, and it is worth nothing unless this one produced some.
        refusedRetries = stub.requests() - askedWhenReported;
    }

    // ---------------------------------------------------------------- //
    // NoFrame, the ordinary state before a wallpaper has ever been set.
    // ---------------------------------------------------------------- //
    stub.answerWith(QStringLiteral("io.wallpaperx.Daemon.Error.NoFrame"));
    {
        // Read BEFORE the item exists, because the item's first fetch can complete during
        // construction: subscribing to FrameReady is a blocking call to the bus daemon, and Qt
        // dispatches incoming messages while one is outstanding. Measured -- taken afterwards,
        // this count already included the request and the check below read zero.
        const int askedBefore = stub.requests();
        std::unique_ptr<QObject> root = makeItem(engine, window, "item_refusal_retry_noframe.qml");
        if (root == nullptr) {
            wallpaperx::testing::harnessFailure("the plugin's QML type instantiated once and then would not again");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }
        const auto publishedReason = [&root] { return root->property("publishedReason").toString(); };

        const bool reported = pumpUntil(
            [&] { return stub.requests() > askedBefore && !publishedReason().isEmpty(); }, 5000);
        check(reported, "an item told there is no frame yet publishes a reason too");
        if (!reported) {
            std::fprintf(stderr, "  the item asked %d time(s) since the answer changed and published [%s]\n",
                         stub.requests() - askedBefore, publishedReason().toStdString().c_str());
            notVerified(kGoals, "the second item never reached the bus, so the no-polling check had nothing under it");
            return wallpaperx::testing::reportNotVerifiedOrPassed();
        }

        // GOAL: and it does NOT ask again. "No frame yet" is answered by the user setting a
        //       wallpaper, which arrives as a FrameReady; a plugin that treated it like a
        //       refusal would ask the daemon every couple of seconds for the life of a fresh
        //       install, on every screen.
        // WHY-THIS-SHAPE: measured over the same span as the retry above, in the same process,
        //       against a count carried out of it -- so "it did not ask again" is read beside a
        //       demonstration that the span is long enough for asking again to be seen. A span
        //       checked on its own could be too short and this check would pass for a plugin
        //       that polls everything.
        // CONTROL: `retryWhileNothingIsShown();` moved out of the NoFreeImage branch and into
        //       the general error path of WallpaperXItem::takeFrameReply (backend/kde/
        //       wallpaperx_item.cpp), just before its `return` -> "FAIL: and it does not ask
        //       again -- a frame that has never existed is not something to poll for", printing
        //       the extra requests. Reverted.
        // COVERAGE-ASSERT: the retry count from the first phase is asserted non-zero here, so a
        //       run in which nothing ever retried cannot report this silence as established.
        check(refusedRetries > 0, "the same span produced at least one retry when the answer was a refusal");
        if (refusedRetries <= 0) {
            std::fprintf(stderr, "  the refusal phase saw no retry, so %d ms of silence here says nothing\n",
                         kRetryWindowMs);
        } else {
            const int askedWhenReported = stub.requests();
            pumpFor(kRetryWindowMs);
            const int extra = stub.requests() - askedWhenReported;
            check(extra == 0, "and it does not ask again -- a frame that has never existed is not something to poll "
                              "for");
            if (extra != 0) {
                std::fprintf(stderr, "  it asked %d more time(s) in %d ms, where a refusal produced %d\n", extra,
                             kRetryWindowMs, refusedRetries);
            }
        }
    }

    return wallpaperx::testing::reportNotVerifiedOrPassed();
}
