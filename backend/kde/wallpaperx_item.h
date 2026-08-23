// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "frame_import.h"

#include <QQuickItem>
#include <QString>
#include <QtGlobal>
#include <QtQml/qqmlregistration.h>

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
//     only place a GL context is current. Freeing is the scene graph's: the node owns its
//     texture, and destroying it releases the import on that same thread.
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
    // Why nothing is being drawn, when nothing is. Empty while the wallpaper is showing, so QML
    // can fall back to the message states main.qml already derives rather than inventing its own.
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY unavailableReasonChanged)
    // Whether a frame has actually been imported and handed to the scene graph. An empty
    // unavailableReason does not mean this: it is also empty before the first frame arrives,
    // which is a window where the desktop shows neither a wallpaper nor a message.
    Q_PROPERTY(bool frameShown READ frameShown NOTIFY frameShownChanged)

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
    static constexpr int kFrameReplyFields = 10;

    // Whether a reply carries enough fields to be read. A newer daemon may send MORE, which is
    // fine; fewer is not.
    static bool replyIsReadable(int fieldCount);

    static bool worthFetching(Arriving generation, Held newestFetched);

    // Whether a frame that has arrived should replace what is on screen. Frames for one monitor
    // can complete out of order, so the newest wins by number rather than by arrival.
    static bool supersedesShown(Arriving generation, Held shown);

    int monitorId() const { return m_monitorId; }
    void setMonitorId(int monitorId);

    QString unavailableReason() const { return m_unavailableReason; }
    bool frameShown() const { return m_frameShown; }

signals:
    void monitorIdChanged();
    void unavailableReasonChanged();
    void frameShownChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void releaseResources() override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private slots:
    // FrameReady(monitor_id, frame_generation) from the daemon, on the GUI thread.
    void onFrameReady(int monitorId, qulonglong generation);

private:
    // A frame fetched but not yet imported. Owns its fd until the render thread takes it: if
    // this is destroyed with one still in it, the fd is closed here rather than leaked, since a
    // leak of one fd per frame ends a login session.
    struct PendingFrame {
        FrameImport::Request request;
        quint64 generation = 0;

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
    // Drops every generation mark together. See the definition for why they go as a set.
    void forgetGenerations();
    void fetchFrame(quint64 generation);
    // The reply half of fetchFrame, on the GUI thread once the bus answers.
    void takeFrameReply(const QDBusMessage& call);
    // The reason has two independent producers -- see the definitions for why neither writes the
    // published string directly.
    void setFetchReason(const QString& reason);   // GUI thread
    void setRenderReason(const QString& reason);  // render thread
    void publishReason();                         // GUI thread

    int m_monitorId = -1;
    // The newest generation this item has fetched, so a signal for a frame already in hand
    // costs nothing. GUI thread only, like everything else the fetch path touches.
    quint64 m_fetchedGeneration = 0;
    // The generation a failed import already asked for again, so an unimportable frame is
    // retried once rather than forever.
    quint64 m_retriedGeneration = 0;
    // A GetFrameFd is out, and the highest generation announced while it was. GUI thread only.
    bool m_fetchInFlight = false;
    std::optional<quint64> m_queuedFetch;
    // Whether FrameReady is subscribed. Without it no wallpaper change is ever heard, which no
    // single frame's success can retire.
    bool m_subscribed = false;
    QString m_fetchReason;         // GUI thread only
    QString m_unavailableReason;   // GUI thread only, what QML reads
    bool m_frameShown = false;     // GUI thread only, derived from m_shownGeneration in publishReason()

    std::mutex m_pendingMutex;
    std::optional<PendingFrame> m_pending;  // guarded by m_pendingMutex

    // Written by the render thread in updatePaintNode and by the GUI thread in releaseResources
    // and setMonitorId, which is why it is guarded.
    std::mutex m_importedMutex;
    quint64 m_shownGeneration = 0;  // guarded by m_importedMutex
    // Bumped every time the marks are invalidated, so the render thread can tell whether the
    // shown generation it read is still the one it is about to write back to.
    quint64 m_generationEpoch = 0;  // guarded by m_importedMutex
    QString m_renderReason;         // guarded by m_importedMutex
};

}  // namespace wallpaperx
