// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "frame_import.h"

#include <QQuickItem>
#include <QString>
#include <QtGlobal>
#include <QtQml/qqmlregistration.h>

#include <mutex>
#include <optional>

class QDBusInterface;

namespace wallpaperx {

// The item that puts a daemon-rendered frame on the desktop.
//
// Three threads meet here and the split is the whole design:
//
//   * the GUI thread constructs this, owns the DBus proxy, and receives FrameReady;
//   * the DBus reply carries a file descriptor, which is fetched on the GUI thread because
//     GetFrameFd is a blocking call and blocking the render thread stalls the compositor;
//   * the render thread imports that fd into GL inside updatePaintNode(), because that is the
//     only place a GL context is current, and frees the previous texture there for the same
//     reason.
//
// m_pendingMutex guards the handover between the first two and the third. Nothing else here is
// shared: every GL object lives and dies on the render thread.
class WallpaperXItem : public QQuickItem {
    Q_OBJECT
    // Declared here rather than registered by hand in a plugin's registerTypes(), so the build
    // generates the module's qmldir AND its type information. Without generated types qmllint
    // cannot see WallpaperXRenderer at all -- measured: it reports "was not found" even with the
    // module on its import path -- which would leave the one QML file that draws the wallpaper
    // permanently unlintable.
    QML_NAMED_ELEMENT(WallpaperXRenderer)
    Q_PROPERTY(int monitorId READ monitorId WRITE setMonitorId NOTIFY monitorIdChanged)
    Q_PROPERTY(bool daemonConnected READ daemonConnected NOTIFY daemonConnectedChanged)
    // Why nothing is being drawn, when nothing is. Empty while the wallpaper is showing, so QML
    // can fall back to the message states main.qml already derives rather than inventing its own.
    Q_PROPERTY(QString unavailableReason READ unavailableReason NOTIFY unavailableReasonChanged)

public:
    explicit WallpaperXItem(QQuickItem* parent = nullptr);
    ~WallpaperXItem() override;

    int monitorId() const { return m_monitorId; }
    void setMonitorId(int monitorId);

    bool daemonConnected() const { return m_daemonConnected; }
    QString unavailableReason() const { return m_unavailableReason; }

signals:
    void monitorIdChanged();
    void daemonConnectedChanged();
    void unavailableReasonChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void releaseResources() override;

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
    void fetchFrame(quint64 generation);
    void setDaemonConnected(bool connected);
    void setUnavailableReason(const QString& reason);

    int m_monitorId = -1;
    // The newest generation this item has fetched, so a signal for a frame already in hand
    // costs nothing. GUI thread only, like everything else the fetch path touches.
    quint64 m_fetchedGeneration = 0;
    bool m_daemonConnected = false;
    QString m_unavailableReason;

    QDBusInterface* m_daemon = nullptr;

    std::mutex m_pendingMutex;
    std::optional<PendingFrame> m_pending;  // guarded by m_pendingMutex

    // Render thread only, past construction.
    FrameImport::Result m_imported;
    quint64 m_shownGeneration = 0;
};

}  // namespace wallpaperx
