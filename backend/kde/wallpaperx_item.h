// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "buffer_claims.h"
#include "frame_import.h"

#include <QQuickItem>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QtGlobal>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class QDBusMessage;

namespace wallpaperx {

// Says when what the daemon's /config answers may have changed, so main.qml asks only then: the
// daemon announced a change, the first lookup of its bus name found an owner, or the name changed
// owner -- a daemon arriving, leaving or replaced. main.qml asks once at startup on its own; this
// reports what comes after its creation. GUI thread.
class ConfigWatch : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(WallpaperXConfigWatch)

public:
    explicit ConfigWatch(QObject* parent = nullptr);

signals:
    void configChanged();
};

// The item that puts a daemon-rendered frame on the desktop.
//
// Three threads meet here and the split is the whole design:
//
//   * the GUI thread constructs this, owns the DBus proxy, and receives FrameReady;
//   * GetFrame is asked ASYNCHRONOUSLY and its reply arrives back on the GUI thread. Not a
//     preference: a blocking call parks the thread running the compositor's UI, and the first
//     fetch happens while QML is still building this item, where the nested event loop such a
//     call needs never runs -- measured, it never returned and the item drew nothing;
//   * the render thread imports that fd into GL inside updatePaintNode(), because that is the
//     only place a GL context is current. Freeing is the scene graph's: the node owns the
//     imports, and destroying it releases them on that same thread.
//
// The daemon cycles a ring of images and hands the same few back over and over, so an import is
// kept and looked up by which image it is, and a picture drawn into an image already imported
// costs only the daemon's acquire and a change of which texture the node samples. Only for a
// picture that brings that acquire: waiting on it is the one thing that tells the driver the
// memory holds something else now, so a picture arriving without one is imported afresh.
//
// Each fetch tells the daemon which images the node can draw that way, and the daemon then leaves
// their memory out of the reply. Told per fetch and never left for the daemon to remember: the
// node drops imports on its own -- a trim, a teardown, a failed hand-over -- and a reply can cross
// one. A reply naming an import the node no longer has is drawn by no one; the next fetch claims
// nothing, so it brings the memory.
//
// The acquire comes one of two ways, chosen by each fetch: a value on the daemon's acquire
// timeline, imported once per node, when this item can wait on it; a new binary semaphore per
// picture otherwise, which always works and is where any doubt sends it back.
//
// What is NOT cached is the claim: the daemon must not draw into the image being sampled, so every
// picture's hold is given back as soon as the node stops sampling it.
//
// m_pendingMutex guards the handover between the first two and the third. Every GL object lives
// and dies on the render thread.
class WallpaperXItem : public QQuickItem {
    Q_OBJECT
    // Declared here rather than registered by hand in a plugin's registerTypes(), so the build
    // generates the module's qmldir AND its type information. Without generated types qmllint
    // cannot see WallpaperXRenderer at all -- measured: it reports "was not found" even with the
    // module on its import path -- which would leave the one QML file that draws the wallpaper
    // permanently unlintable.
    QML_NAMED_ELEMENT(WallpaperXRenderer)
    Q_PROPERTY(int monitorId READ monitorId WRITE setMonitorId NOTIFY monitorIdChanged)
    // The connector name main.qml resolved monitorId from. This item's pixel size is reported to
    // the daemon under it; empty reports nothing.
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
    // Whether a window has the focus on this screen, and whether one on it is maximized or
    // fullscreen. Set from QML, which can see the windows; reported to the daemon under `output`.
    Q_PROPERTY(bool windowFocused READ windowFocused WRITE setWindowFocused NOTIFY windowFocusedChanged)
    Q_PROPERTY(bool windowMaximized READ windowMaximized WRITE setWindowMaximized NOTIFY windowMaximizedChanged)
    // Why nothing is being drawn, when nothing is. Empty while the wallpaper is showing, so QML
    // can fall back to the message states main.qml already derives rather than inventing its own.
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY unavailableReasonChanged)
    // Whether a frame has actually been imported and handed to the scene graph. An empty
    // unavailableReason does not mean this: it is also empty before the first frame arrives,
    // which is a window where the desktop shows neither a wallpaper nor a message.
    Q_PROPERTY(bool frameShown READ frameShown NOTIFY frameShownChanged)
    // Properties rather than plain accessors because the only readers outside this process load
    // the item through its QML module and have no other way to ask. One change signal for all
    // three, emitted after every picture drawn or failed, on the hop to this thread that each
    // already makes -- a signal of its own would be another queued event per video frame. Without
    // a NOTIFY a QML binding on them would be read once and then frozen.
    Q_PROPERTY(quint64 importCount READ importCount NOTIFY importCountsChanged)
    Q_PROPERTY(quint64 reusedImportCount READ reusedImportCount NOTIFY importCountsChanged)
    Q_PROPERTY(quint64 semaphoreImportCount READ semaphoreImportCount NOTIFY importCountsChanged)
    Q_PROPERTY(quint64 lostImportCount READ lostImportCount NOTIFY importCountsChanged)

public:
    explicit WallpaperXItem(QQuickItem* parent = nullptr);
    ~WallpaperXItem() override;

    // The two generation decisions, as pure functions so they can be graded without a daemon,
    // a window or a GPU. Inline in their call sites they were reachable only by staging two
    // fetches that complete out of order, which nothing here can force.
    //
    // Zero means "not known": the callers that are not FrameReady signals pass it, having no
    // idea what the daemon is on, and it must never be treated as older than everything.

    // Whether a FrameReady announcing `generation` is worth fetching, given the newest already
    // fetched. Fetching is not free -- a bus round trip, an export by the daemon, a dup here.
    // Distinct types rather than two quint64s, because the two arguments mean opposite things
    // and swapping them compiled -- a real gap: every check in frame_generation_test passes
    // against a build with the arguments the wrong way round, since the predicates themselves
    // are still correct. The type is what grades the call site; a test cannot.
    struct Arriving {
        quint64 value = 0;
    };
    struct Held {
        quint64 value = 0;
    };

    // How many fields GetFrame declares. A reply with fewer is not ours, and reading past the
    // end of one is a crash inside the compositor -- which is why this is a named constant and
    // a predicate rather than a literal buried in the parse.
    static constexpr int kFrameReplyFields = 14;

    static constexpr int kBufferIdField = 11;
    static constexpr int kAcquireValueField = 12;
    // handover, the last of them: what a release of this reply's image names.
    static constexpr int kHandoverField = 13;

    // Whether a reply carries enough fields to be read. A newer daemon may send MORE, which is
    // fine; fewer is not.
    static bool replyIsReadable(int fieldCount);

    static bool worthFetching(Arriving generation, Held newestFetched);

    // Whether a frame that has arrived should replace what is on screen. Frames for one monitor
    // can complete out of order, so the newest wins by number rather than by arrival.
    static bool supersedesShown(Arriving generation, Held shown);

    int monitorId() const { return m_monitorId; }
    void setMonitorId(int monitorId);

    QString output() const { return m_output; }
    void setOutput(const QString& output);

    bool windowFocused() const { return m_windows.focused; }
    void setWindowFocused(bool focused);
    bool windowMaximized() const { return m_windows.maximized; }
    void setWindowMaximized(bool maximized);

    QString unavailableReason() const { return m_unavailableReason; }
    bool frameShown() const { return m_frameShown; }

    // How many pictures this item drew through an import it had to make, and how many through one
    // it already held. Both count drawn pictures and nothing else, so their sum is what reached
    // the screen: a picture that failed counts in neither. Written by the render thread and read
    // from any; nothing in this item reads them. They are the only way to tell an import per
    // picture from an import per image -- both draw the same wallpaper, and one does it at the
    // video's frame rate.
    quint64 importCount() const { return m_importCount.load(std::memory_order_relaxed); }
    quint64 reusedImportCount() const { return m_reusedImportCount.load(std::memory_order_relaxed); }

    // How many semaphores this item has tried to import into its context, successful or not: one
    // per picture on the binary path, one per node on the timeline path. Attempts, because each is
    // an exposure -- on NVIDIA one failed import leaves every later one in the context failing.
    quint64 semaphoreImportCount() const { return m_semaphoreImportCount.load(std::memory_order_relaxed); }

    // Pictures that arrived without memory for an import this item had dropped after naming it in
    // the request. Each is asked for again once, naming nothing, unless the screen or the daemon
    // changed meanwhile.
    quint64 lostImportCount() const { return m_lostImportCount.load(std::memory_order_relaxed); }

signals:
    void monitorIdChanged();
    void outputChanged();
    void windowFocusedChanged();
    void windowMaximizedChanged();
    void unavailableReasonChanged();
    void frameShownChanged();
    void importCountsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void releaseResources() override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private slots:
    // FrameReady(monitor_id, frame_generation) from the daemon, on the GUI thread.
    void onFrameReady(int monitorId, qulonglong generation);

    // ImportModifiersChanged() from the daemon, on the GUI thread. Only a report the daemon
    // refused is worth retrying, and only because the refusal may have been about a set this
    // process does not control.
    void onImportModifiersChanged();

    // ConfigChanged() from the daemon, on the GUI thread: a screen may have been named, which is
    // what a size refused as UnknownOutput waits for.
    void onConfigChanged();

private:
    // A frame fetched but not yet imported. Owns its fd until the render thread takes it: if
    // this is destroyed with one still in it, the fd is closed here rather than leaked, since a
    // leak of one fd per frame ends a login session. request.fd is -1 when the daemon left the
    // memory out because the fetch named this image as one the node can draw.
    //
    // It owns the daemon's claim on the image the same way, and for the same reason: the daemon
    // records the hold when it answers GetFrame, so every path that drops the frame without
    // importing it owes a release. A picture that reaches the screen moves the claim to the node.
    struct PendingFrame {
        FrameImport::Request request;
        quint64 generation = 0;
        BufferClaims::Claim claim;
        // Where the claim is registered, so dropping this frame gives the image back only if the
        // item has not already done so on its way off the screen.
        std::shared_ptr<ClaimSet> claims;
        // The unique name of the daemon that sent the reply, which the request was addressed to. A
        // value waited on another daemon's timeline never completes, so a value is only ever paired
        // with this.
        std::string sender;

        PendingFrame() = default;
        ~PendingFrame();
        PendingFrame(PendingFrame&&) noexcept;
        PendingFrame& operator=(PendingFrame&&) noexcept;
        PendingFrame(const PendingFrame&) = delete;
        PendingFrame& operator=(const PendingFrame&) = delete;
    };

    void connectToDaemon();
    // Installs the FrameReady match rule. False when the bus refused it.
    bool subscribeToFrameReady();
    // Installs the ImportModifiersChanged match rule. False when the bus refused it.
    bool subscribeToImportModifiersChanged();
    // What becomes of a picture that has arrived and not yet been drawn. Kept across a change of
    // daemon -- it is still this screen's, from the daemon it was asked of -- and dropped with a
    // change of monitor or on leaving the screen.
    enum class Undrawn { Keep, Drop };
    // Drops every generation mark and the frame they describe. See the definition for why they go
    // as a set. discardHeldFrame() is the same without publishing, for the paths that must not.
    void discardHeldFrame(Undrawn undrawn);
    void forgetGenerations(Undrawn undrawn);
    void fetchFrame(quint64 generation);
    // Owes a fetch of `generation` for when the latch opens: the highest announced, or 0, which
    // forces and stays forced.
    void queueFetch(quint64 generation);
    // Asks only when this item neither holds a reply nor has a request out. The scene-change
    // caller's question, which fetchFrame(0) cannot express because 0 forces.
    void ensureFrameRequested();
    // Whether the next fetch, to `daemon`, may ask for a value on the acquire timeline rather than
    // sync_fds: this process holds that daemon's timeline, and this item's context can wait on it
    // and has not refused it. Decided per fetch, so any doubt costs one picture's binary semaphore
    // rather than a value nothing can wait on. GUI thread.
    bool fetchOnTimeline(const BufferClaims::Daemon& daemon);
    // The images whose memory the next fetch, to `daemon`, need not be sent: the node's imports
    // from that daemon for this monitor, at most kMaxKnownBuffers. Empty when the fetch must bring
    // the memory. GUI thread.
    std::vector<quint64> importsToDeclare(const BufferClaims::Daemon& daemon);
    // The reply half of fetchFrame, on the GUI thread once the bus answers. `monitorId` is the
    // monitor the request named, which is what the claim must be recorded against; `from`, the
    // daemon the request was addressed to and the lifetime it was asked in; `askedForValue`, whether the
    // request asked for a value on the acquire timeline; `declared`, the images it said the node
    // can draw.
    void takeFrameReply(int monitorId, const QDBusMessage& call, const BufferClaims::Daemon& from,
                        bool askedForValue, const std::vector<quint64>& declared);
    // The same reply when this item has moved to another monitor since asking: the hold the
    // daemon recorded is given straight back, because nothing else ends it. `daemon` is the
    // lifetime the request was asked in.
    static void releaseFrameReply(int monitorId, const QDBusMessage& call, quint64 daemon);
    // Whether the generation marks were invalidated since m_generationEpoch read `epoch`. GUI thread.
    bool marksMovedSince(quint64 epoch);
    // Records the hand-over a reply made for `monitorId` in the lifetime `daemon`: an unheld claim
    // for a reply that granted none. Every reply goes through this, in arrival order.
    static BufferClaims::Claim takeGranted(int monitorId, const QDBusMessage& call, quint64 daemon);
    // Asks again after a refusal, while this item has nothing on screen. GUI thread.
    void retryWhileNothingIsShown();
    // Ends the wait after a TooManyDescriptors refusal. GUI thread.
    void reopenFetchAfterBackoff();
    // The next refusal of either kind waits the first, shortest time again. On its own for a frame
    // served, which leaves any wait running. GUI thread.
    void endRefusalRuns();
    // Ends the NoFreeImage run and stops its retry. On its own for a new monitor or the item leaving
    // the screen: a TooManyDescriptors back-off and its count are the whole process's, so they
    // outlive either. GUI thread.
    void abandonNoImageRun();
    // Ends both runs and stops the waits they started: the NoFreeImage retry and a
    // TooManyDescriptors back-off. For a new daemon; the caller fetches afresh when it can. GUI
    // thread.
    void abandonRefusals();
    // Tells the daemon which dma_buf layouts this driver can import, so it stops choosing ones
    // this process cannot take. Render thread; sends from the GUI thread. Once per process.
    void reportImportModifiersOnce(quint32 drmFormat);
    // Drops a modifier the daemon may choose but this driver could not actually import, and
    // re-reports. Render thread; sends from the GUI thread.
    void withdrawImportModifier(quint32 drmFormat, quint64 modifier);
    // The reason has two independent producers -- see the definitions for why neither writes the
    // published string directly.
    void setFetchReason(const QString& reason);   // GUI thread
    void setRenderReason(const QString& reason);  // render thread
    void publishReason();                         // GUI thread

    // What SetOutputSize was last told. GUI thread.
    struct SizeReport {
        QString output;
        QSize pixels;
        bool operator==(const SizeReport&) const = default;
    };
    // Restarts the settle timer; reportSize() runs when the triggers stop. GUI thread.
    void scheduleSizeReport();
    void reportSize();

    // Recomputes whether this item is on the screen at all -- in a window and visible -- and acts
    // on the change. See the definition for what leaving costs. GUI thread.
    void updateOnScreen();

    // Gives back every image this item still holds, and drops the frame it had not yet imported.
    // Does not wait for the reads to finish; see the definition. GUI thread, and the destructor.
    void releaseHeldImages();

    // Stops presenting the image releaseHeldImages() gave back, by asking the next scene graph pass
    // to destroy the node. GUI thread, and never from the destructor: it publishes to QML.
    void dropShownFrame();

    // Starts the watch that answers whether the desktop is drawing what this item hands it, if it
    // is not already running. Armed wherever the daemon answers a fetch, served or refused -- a
    // covered screen produces refusals, since a scene graph that has stopped running frees no
    // image to be handed the next one. Once it finds no passes it keeps itself going.
    void armPaintWatch();
    void checkPainting();

    // Tells the daemon whether this screen's wallpaper can be seen, when the answer changes.
    // GUI thread.
    void reportVisible();

    struct Windows {
        bool focused = false;
        bool maximized = false;
        bool operator==(const Windows&) const = default;
    };
    // What SetOutputFocus was last told. GUI thread.
    struct WindowReport {
        QString output;
        Windows windows;
        bool operator==(const WindowReport&) const = default;
    };
    // Tells the daemon what the windows on this screen are doing, when that changed or the daemon
    // may have lost it. GUI thread.
    void reportWindows();
    // Wherever the item starts speaking to a different daemon, name or monitor, or stops speaking:
    // the next refusal starts a new run, retried at the first delay and logged, and a retry left
    // from the old run is dropped. Each caller arranges its own resend.
    void restartWindowRetry();

    int m_monitorId = -1;
    QString m_output;  // GUI thread only
    // Settles the size before it is reported: a login lays the item out, exposes the window and
    // applies its scale in quick succession, and every intermediate size costs the daemon a redraw.
    QTimer m_sizeReportTimer;
    // The last report sent, so an unchanged size is not sent again -- one refused for its
    // arguments included. Forgotten when a resend could succeed: a refusal a later event or a
    // retry can cure, a restart, or a monitor change, which can mean the name was unbound and bound
    // again in between. Empty, it is sent at the next trigger, ConfigChanged among them. GUI thread
    // only.
    std::optional<SizeReport> m_sizeReported;
    // The report a run of retried refusals is about, and how many it has had, which doubles the
    // wait before the next resend. A different report starts a new run; a success ends it. GUI
    // thread only.
    struct SizeRefusals {
        SizeReport report;
        int times = 0;
    };
    std::optional<SizeRefusals> m_sizeRefusals;

    // Whether this item is in a window and visible, which is Qt's answer, and whether the scene
    // graph is actually painting it, which is the compositor's -- Qt goes on calling an occluded
    // window's item visible. What the daemon is told is the two together: a wallpaper nobody can
    // see is one it need not keep decoding. GUI thread only.
    bool m_onScreen = false;
    bool m_painting = true;
    // The last answer the daemon accepted, so an unchanged one is not sent again. Empty wherever
    // the daemon may have lost it, as the size report is.
    std::optional<bool> m_visibleReported;
    // The two window facts as QML last set them.
    Windows m_windows;
    // The last window report sent and not refused. Forgotten wherever the daemon may have lost it,
    // and on leaving the screen, after which another item for the name may have reported over it.
    std::optional<WindowReport> m_windowsReported;
    // Zero delay after a change, so both facts changing in one QML update are one report rather than
    // two. Also holds the retry of a refused report, which a change brings forward.
    QTimer m_windowReportTimer;
    // How long the next retry of a refused window report waits: doubles while the daemon goes on
    // refusing, back to the start on an accepted one or a new context (restartWindowRetry). Only the
    // first refusal of a run is logged.
    int m_windowRetryMs;
    QTimer m_paintWatch;
    quint64 m_paintMark = 0;   // the count when the watch was last armed
    // One window has ended with no pass and another has been asked for. A single empty window is
    // equally consistent with a pass that was merely late, and acting on it stalls a video the
    // user is watching.
    bool m_paintUnconfirmed = false;
    // How long the next probe waits, doubling while the screen stays dark. Reset whenever the watch
    // is armed afresh, so a short absence is still noticed within kPaintProbeMs of coming back.
    int m_probeMs = 250;
    // Passes the scene graph has run for this item. Written by the render thread and read by the
    // GUI thread, which is the whole point: it is the one fact that says whether what this item
    // hands over is being put on the screen.
    std::atomic<quint64> m_paintCount{0};

    // Render thread, read by whoever asks. See importCount().
    std::atomic<quint64> m_importCount{0};
    std::atomic<quint64> m_reusedImportCount{0};
    std::atomic<quint64> m_semaphoreImportCount{0};
    std::atomic<quint64> m_lostImportCount{0};
    // Hand-overs that failed since a picture last reached the screen. What separates a blip, which
    // must not take a wallpaper off the screen, from a producer hand-over that has stopped working,
    // which has to say so. On the item rather than on the node, so a node rebuilt for an unrelated
    // reason does not restart the run. Render thread only, like everything in updatePaintNode.
    int m_handOversFailed = 0;

    // The claims on the pictures this item has been handed. Shared with the node, which the scene
    // graph destroys on the render thread and after this item is gone. Replaced, never reused,
    // when the item leaves the screen: a holder left over from before then finds its own set empty
    // rather than a claim on the same ring slot taken since.
    std::shared_ptr<ClaimSet> m_claims = std::make_shared<ClaimSet>();
    // The newest generation this item has fetched, so a signal for a frame already in hand
    // costs nothing. GUI thread only, like everything else the fetch path touches.
    quint64 m_fetchedGeneration = 0;
    // The generation a failed import already asked for again, so an unimportable frame is
    // retried once rather than forever.
    quint64 m_retriedGeneration = 0;
    // Whether a GetFrame is out or its TooManyDescriptors refusal is being waited out, and the
    // fetch owed when that ends: the refused one and what was announced meanwhile. No fetch is
    // sent in either. Only Asking has a reply to come, which may name an import of a daemon or
    // monitor this item has since left. GUI thread only.
    enum class Fetch { Idle, Asking, BackingOff };
    Fetch m_fetch = Fetch::Idle;
    std::optional<quint64> m_queuedFetch;
    // Running exactly while m_fetch is BackingOff. A member, not a single shot, so
    // abandonRefusals() can end the backoff.
    QTimer m_backoffTimer;
    // The daemon lifetime of a GetFrame that outlived its patience and has not been answered, or 0.
    // Nothing more is asked of that lifetime until it answers or is replaced: a daemon that has
    // stopped answering would otherwise collect one more pending call per fetch.
    quint64 m_unansweredLifetime = 0;
    // The next GetFrame sent names no imports, because a reply to one that did found the import it
    // named gone. Cleared when that fetch is sent, so a daemon leaving the memory out of its reply
    // too is a protocol violation and not a reason to ask again. GUI thread only.
    bool m_declareNoImports = false;
    // The retry after a NoFreeImage refusal. One at a time, so a refusal answered by another refusal
    // cannot compound into a growing number of timers; stopped by abandonNoImageRun(), so one armed
    // before cannot set the first wait after. GUI thread only.
    QTimer m_noImageRetryTimer;
    // The refusals of each kind in the current run, which double the wait after the next one. Kept
    // apart because their causes are unrelated: a busy image ring, and this process holding too many
    // descriptors. A run ends in endRefusalRuns(), abandonNoImageRun() or abandonRefusals(). GUI
    // thread only.
    int m_noImageRefusals = 0;
    int m_descriptorRefusals = 0;
    // Whether FrameReady is subscribed. Without it no wallpaper change is ever heard, which no
    // single frame's success can retire.
    bool m_subscribed = false;
    QString m_fetchReason;         // GUI thread only
    QString m_unavailableReason;   // GUI thread only, what QML reads
    bool m_frameShown = false;     // GUI thread only, derived from m_nodeHasFrame in publishReason()

    std::mutex m_pendingMutex;
    std::optional<PendingFrame> m_pending;  // guarded by m_pendingMutex

    // Written by the render thread in updatePaintNode and by the GUI thread in releaseResources
    // and setMonitorId, which is why it is guarded.
    std::mutex m_importedMutex;
    quint64 m_shownGeneration = 0;  // guarded by m_importedMutex
    // Whether a node with a live texture is on screen -- NOT the same question as
    // m_shownGeneration, which is a staleness mark cleared whenever generations stop meaning
    // anything (a daemon restart, a monitor change) while the node keeps drawing what it has.
    // Cleared where the node dies, not where generations are invalidated.
    bool m_nodeHasFrame = false;  // guarded by m_importedMutex
    // What the next pass owes the node. Only the render thread may touch GL, so the GUI thread
    // asks and the pass does it; two things it can ask for, one value so they cannot both be
    // pending and disagree.
    //
    //   ForgetStaleImports  the marks these imports were fetched against stopped meaning anything
    //                       -- a daemon replaced, a monitor changed -- and no reply asked before
    //                       that is still to come, so none of them can be handed over again. Acted
    //                       on after the pass's picture, which may be that last reply's. The
    //                       picture on the screen stays: it is still a picture, and blanking the
    //                       desktop until a replacement daemon produces one costs more than the
    //                       memory does.
    //   Destroy             the image being shown has been given back, so it must stop being
    //                       sampled at all. Outranks the other, whichever was asked for first.
    enum class NodeRequest { None, ForgetStaleImports, Destroy };
    NodeRequest m_nodeRequest = NodeRequest::None;  // guarded by m_importedMutex
    // Bumped every time the marks are invalidated, so the render thread can tell whether the
    // shown generation it read is still the one it is about to write back to.
    quint64 m_generationEpoch = 0;  // guarded by m_importedMutex
    // What the render thread has found about waiting on the daemon's acquire timeline, for the GUI
    // thread choosing which method a fetch uses: whether this item's context can wait on one at all
    // -- false too once the context has refused a semaphore import -- and the daemon whose timeline
    // this item failed to import or wait on, which it asks for sync_fds from then on.
    bool m_contextWaitsOnTimeline = false;  // guarded by m_importedMutex
    std::string m_timelineRefusedBy;        // guarded by m_importedMutex
    // The images the node holds an import of and can draw again, as the render thread last found
    // them, for the GUI thread to declare in the next fetch. Empty whenever the node could not reuse
    // one -- no node, or a context that cannot wait on the daemon's acquire. Each names the daemon
    // lifetime and monitor it belongs to, which only the GUI thread can compare with the current.
    struct KnownImport {
        quint64 daemon = 0;
        int monitorId = -1;
        quint64 bufferId = 0;
    };
    std::vector<KnownImport> m_knownImports;  // guarded by m_importedMutex
    QString m_renderReason;         // guarded by m_importedMutex
};

}  // namespace wallpaperx
