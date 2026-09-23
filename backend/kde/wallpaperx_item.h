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

class QDBusMessage;

namespace wallpaperx {

// The item that puts a daemon-rendered frame on the desktop.
//
// Three threads meet here and the split is the whole design:
//
//   * the GUI thread constructs this, owns the DBus proxy, and receives FrameReady;
//   * GetFrameFd is asked ASYNCHRONOUSLY and its reply arrives back on the GUI thread. Not a
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
    // Why nothing is being drawn, when nothing is. Empty while the wallpaper is showing, so QML
    // can fall back to the message states main.qml already derives rather than inventing its own.
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY unavailableReasonChanged)
    // Whether a frame has actually been imported and handed to the scene graph. An empty
    // unavailableReason does not mean this: it is also empty before the first frame arrives,
    // which is a window where the desktop shows neither a wallpaper nor a message.
    Q_PROPERTY(bool frameShown READ frameShown NOTIFY frameShownChanged)
    // Properties rather than plain accessors because the only readers outside this process load
    // the item through its QML module and have no other way to ask. One change signal for both:
    // a picture moves exactly one of them, and the announcement rides the hop to this thread that
    // every drawn picture already makes -- a second signal would be a second queued event per
    // video frame. Without a NOTIFY a QML binding on either would be read once and then frozen,
    // which is the one reading this exists for.
    Q_PROPERTY(quint64 importCount READ importCount NOTIFY importCountsChanged)
    Q_PROPERTY(quint64 reusedImportCount READ reusedImportCount NOTIFY importCountsChanged)

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

    // How many fields GetFrameFd declares. A reply with fewer is not ours, and reading past the
    // end of one is a crash inside the compositor -- which is why this is a named constant and
    // a predicate rather than a literal buried in the parse.
    static constexpr int kFrameReplyFields = 12;

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

signals:
    void monitorIdChanged();
    void outputChanged();
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

private:
    // A frame fetched but not yet imported. Owns its fd until the render thread takes it: if
    // this is destroyed with one still in it, the fd is closed here rather than leaked, since a
    // leak of one fd per frame ends a login session.
    //
    // It owns the daemon's claim on the image the same way, and for the same reason: the daemon
    // records the hold when it answers GetFrameFd, so every path that drops the frame without
    // importing it owes a release. A picture that reaches the screen moves the claim to the node.
    struct PendingFrame {
        FrameImport::Request request;
        quint64 generation = 0;
        BufferClaims::Claim claim;
        // Where the claim is registered, so dropping this frame gives the image back only if the
        // item has not already done so on its way off the screen.
        std::shared_ptr<ClaimSet> claims;

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
    // Drops every generation mark and the frame they describe. See the definition for why they go
    // as a set. discardHeldFrame() is the same without publishing, for the paths that must not.
    void discardHeldFrame();
    void forgetGenerations();
    void fetchFrame(quint64 generation);
    // Asks only when this item neither holds a reply nor has a request out. The scene-change
    // caller's question, which fetchFrame(0) cannot express because 0 forces.
    void ensureFrameRequested();
    // The reply half of fetchFrame, on the GUI thread once the bus answers. `monitorId` is the
    // monitor the request named, which is what the claim must be recorded against.
    void takeFrameReply(int monitorId, const QDBusMessage& call);
    // The same reply when this item has moved to another monitor since asking: the hold the
    // daemon recorded is given straight back, because nothing else ends it.
    static void releaseFrameReply(int monitorId, const QDBusMessage& call);
    // Which image a reply granted, 0 for one that granted none.
    static quint64 grantedBufferId(const QDBusMessage& call);
    // Asks again after a refusal, while this item has nothing on screen. GUI thread.
    void retryWhileNothingIsShown();
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

    int m_monitorId = -1;
    QString m_output;  // GUI thread only
    // Settles the size before it is reported: a login lays the item out, exposes the window and
    // applies its scale in quick succession, and every intermediate size costs the daemon a redraw.
    QTimer m_sizeReportTimer;
    // The last report sent and not refused, so an unchanged size is not sent again. Forgotten
    // wherever the daemon may have lost it: a restart, or a monitor change, which can mean the
    // name was unbound and bound again in between. GUI thread only.
    std::optional<SizeReport> m_sizeReported;

    // Whether this item is in a window and visible, which is Qt's answer, and whether the scene
    // graph is actually painting it, which is the compositor's -- Qt goes on calling an occluded
    // window's item visible. What the daemon is told is the two together: a wallpaper nobody can
    // see is one it need not keep decoding. GUI thread only.
    bool m_onScreen = false;
    bool m_painting = true;
    // The last answer the daemon accepted, so an unchanged one is not sent again. Empty wherever
    // the daemon may have lost it, as the size report is.
    std::optional<bool> m_visibleReported;
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
    // A GetFrameFd is out, and the highest generation announced while it was. GUI thread only.
    bool m_fetchInFlight = false;
    std::optional<quint64> m_queuedFetch;
    // A retry after a refusal is armed. One at a time, so a refusal answered by another refusal
    // cannot compound into a growing number of timers. GUI thread only.
    bool m_retryQueued = false;
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
    //                       -- a daemon replaced, a monitor changed -- so none of them can be
    //                       handed over again. The picture on the screen stays: it is still a
    //                       picture, and blanking the desktop until a replacement daemon produces
    //                       one costs more than the memory does.
    //   Destroy             the image being shown has been given back, so it must stop being
    //                       sampled at all. Outranks the other, whichever was asked for first.
    enum class NodeRequest { None, ForgetStaleImports, Destroy };
    NodeRequest m_nodeRequest = NodeRequest::None;  // guarded by m_importedMutex
    // Bumped every time the marks are invalidated, so the render thread can tell whether the
    // shown generation it read is still the one it is about to write back to.
    quint64 m_generationEpoch = 0;  // guarded by m_importedMutex
    QString m_renderReason;         // guarded by m_importedMutex
};

}  // namespace wallpaperx
