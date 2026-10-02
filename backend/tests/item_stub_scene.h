// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The scene several tests stand WallpaperXItem up in: a stub daemon that serves real exported
// frames and answers ReleaseBuffer, the QML that instantiates the plugin's own type, and the
// readers that ask what the item ended up with.
//
// Shared because they all need the same precondition -- a frame genuinely imported and attached
// to a scene-graph node -- and reach it identically; only what they then do to it differs. Kept
// as one definition rather than copies: a copy that drifts still compiles, still runs, and
// still reads as coverage, and two copies had already lost two CONTROL annotations between
// them.

#include "kde/frame_import.h"
#include "renderers/static_renderer.h"
#include "support/test_image.h"
#include "tests/daemon_wire.h"
#include "tests/pump.h"
#include "tests/qt_frame_reply.h"
#include "vulkan/device.h"
#include "vulkan/frame_export.h"
#include "vulkan/offscreen.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QQuickItem>
#include <QSGSimpleTextureNode>
#include <QString>
#include <QtQuick/private/qquickitem_p.h>

#include <EGL/egl.h>
#include <GL/gl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wallpaperx::testing {

// Not square and not a round number of pixels wide, for frame_export_probe's reason: a square
// target imports fine with width and height transposed.
inline constexpr uint32_t kWidth = 68;
inline constexpr uint32_t kHeight = 36;

// The monitor id the QML below asks for, and the one a test may move it to. Different, and
// neither is the -1 the item refuses to fetch for.
inline constexpr int kFirstMonitor = 0;
inline constexpr int kSecondMonitor = 1;

// Renders once into `target`, so it carries a PUBLISHED frame.
//
// Not decoration: the daemon only ever exports a frame a render published, and the acquire
// descriptor is a hand-over of that publication -- exportFrameAcquireFd refuses a target with
// nothing published, by design. A stub exporting an unrendered target would be serving the item a
// shape the daemon cannot produce, and the acquire half of its reply could not exist at all.
//
// The renderer is a local: targets.render() waits for the submission, so nothing of it is needed
// afterwards. `fixtureName` keeps each caller's PNG on a path of its own -- these binaries share
// WALLPAPERX_TEST_IMAGE_DIR.
inline void publishOneFrame(wallpaperx::Device& device, wallpaperx::OffscreenTargetManager& targets,
                            const wallpaperx::OffscreenTarget& target, const char* fixtureName) {
    const std::string imagePath =
        wallpaperx::testing::writeGradientPng(std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/" + fixtureName, 16, 16);
    wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);
    targets.render(renderer, target);
}

// The same, drawing one flat colour: a picture a readback can name at a glance, so a check can
// tell which of several pictures an import is showing.
inline void publishSolidFrame(wallpaperx::Device& device, wallpaperx::OffscreenTargetManager& targets,
                              const wallpaperx::OffscreenTarget& target, const std::string& fixtureName,
                              std::array<unsigned char, 4> rgba) {
    const std::string imagePath =
        wallpaperx::testing::writeSolidPng(std::string(WALLPAPERX_TEST_IMAGE_DIR) + "/" + fixtureName, 16, 16, rgba);
    wallpaperx::StaticRenderer renderer(device, targets.renderPass(), imagePath, WALLPAPERX_SHADER_DIR);
    targets.render(renderer, target);
}

// Serves a caller-chosen sequence of real exported frames -- memory and acquire descriptor both
// -- one per request, and refuses once the sequence runs out. Also answers ReleaseBuffer and
// records what it was told.
//
// The refusal is a barrier tests rest on. Making the item fetch again -- by moving it to another
// monitor id -- must not produce an unplanned importable frame, or the next render pass would
// import it and set the "a node is drawing" fact back to true, masking the mutation a check
// exists to catch. A refusal reaches the item as a fetch reason and touches neither the node nor
// the texture.
//
// ReleaseBuffer is ANSWERED rather than left to Qt's unknown-method error, and recorded, because
// the plugin's release is otherwise invisible from outside it: it sends a message and watches no
// reply, so a release that was never sent and one that was sent look identical to the item. The
// log is what makes "released once per image, not once per import" a fact this side can read.
//
// Both counters are touched only from the thread whose event loop dispatches this connection,
// which is the thread the tests pump on, and the plugin's release hops to that same thread
// before it sends. No locking, and nothing here may start being called from the render thread.
class ExportingStub : public QDBusVirtualObject {
public:
    // What ReleaseBuffer was told, in order.
    struct Release {
        int monitorId = -1;
        quint64 bufferId = 0;
    };

    // One target, served once: what the frames-on-screen phases need.
    ExportingStub(wallpaperx::Device& device, const wallpaperx::OffscreenTarget& target)
        : m_device(device), m_sequence{&target} {}

    // A chosen sequence, one frame per request. The same target may appear twice -- that is what
    // the plugin does when the scene changes and it re-imports the image it is already showing.
    ExportingStub(wallpaperx::Device& device, std::vector<const wallpaperx::OffscreenTarget*> sequence)
        : m_device(device), m_sequence(std::move(sequence)) {}

    // Reports this buffer id on every reply, instead of each image's own. For the one check that
    // needs two DIFFERENT images to collide on one id, which is what a daemon restart produces:
    // ids restart at 1, so the successor's first image carries an id the previous daemon had
    // already handed out.
    void reportBufferIdAs(quint64 bufferId) { m_bufferIdOverride = bufferId; }

    // Builds the next GetFrameFd reply as usual but does not send it until sendHeldReply().
    //
    // For the one state a synchronous stub cannot produce: a reply IN FLIGHT while the item
    // changes which monitor it shows. The daemon records the hold when it answers, so that reply
    // grants a claim to an item that no longer wants it, and only a stub that can pause between
    // the two halves puts the plugin in that position at all.
    // Refuses every further GetFrameFd with this error name, whatever the sequence has left; an
    // empty name puts the sequence back in charge. For the checks that are about ONE named
    // refusal, which the exhausted-sequence answer cannot express -- the plugin branches on the
    // name, and NoFreeImage is the only one it treats as temporary.
    void refuseFurtherWith(QString errorName) { m_refuseWith = std::move(errorName); }

    void holdNextReply() { m_holdNext = true; }
    bool replyIsHeld() const { return m_heldConnection.has_value(); }

    // Holds every frame reply, oldest first, until sendOldestHeld(). For a drive that must deliver
    // exactly one reply between two render passes while the item already has the next request out:
    // released as it arrived, the next reply could replace one the render thread had not yet seen.
    void holdEveryReply(bool hold) { m_holdEvery = hold; }
    size_t heldReplies() const { return m_heldQueue.size(); }
    bool sendOldestHeld() {
        if (m_heldQueue.empty()) return false;
        auto [connection, reply] = std::move(m_heldQueue.front());
        m_heldQueue.erase(m_heldQueue.begin());
        return connection.send(reply);
    }

    // Serves the sequence round and round instead of refusing once it runs out: a daemon's ring,
    // handing the same few images back in turn for as long as it is asked.
    void rotate() { m_rotate = true; }

    // Serves `target` for the next request only, whatever the sequence says, for a check that needs
    // one particular image to come round next.
    void serveNext(const wallpaperx::OffscreenTarget* target) { m_serveNext = target; }

    // Holds the reply to the request that would be served as number `number` (counting from one),
    // as holdNextReply() does. Decided here, as the request arrives, rather than armed by the test
    // after it has seen the previous one served: the item asks again on its own after a failed
    // picture, and a hold armed from outside races that request.
    void holdServeNumber(int number) { m_holdAtServe = number; }

    // What goes beside the memory descriptor on every further reply, in place of the daemon's
    // acquire. `None` is what a driver that cannot export one sends; `Unusable` is a descriptor the
    // importer cannot wait on -- a memfd, which is no semaphore -- so the picture's memory imports
    // and its hand-over fails, the one way a hand-over failure can be staged from outside the item.
    enum class Acquire { Real, None, Unusable };
    void sendAcquire(Acquire mode) { m_acquireFrom = {{1, mode}}; }
    // The same, from the reply served as number `serveNumber` on (counting from one), leaving the
    // replies before it as they were scheduled. Decided per reply as it is built, for
    // holdServeNumber()'s reason: the item's own re-asks do not wait for the test.
    void sendAcquireFrom(int serveNumber, Acquire mode) { m_acquireFrom[serveNumber] = mode; }

    // The acquire timeline, one per caller. Absent -- the default -- answers GetAcquireTimeline as a
    // daemon from before it did, UnknownMethod, so every test not about the timeline stays on
    // sync_fds. Grant creates and hands over the caller's timeline; Silent creates it and never
    // answers, which is a lost reply; BadDescriptor hands over a descriptor that is no semaphore
    // payload, which the driver refuses at import.
    enum class Timeline { Absent, Grant, Silent, BadDescriptor };
    void serveTimelines(Timeline mode) { m_timelineMode = mode; }
    // The next `count` asks are answered with `errorName`, whatever the mode.
    void refuseTimelineAsks(int count, QString errorName) {
        m_timelineRefusals = count;
        m_timelineRefusal = std::move(errorName);
    }
    // Each timeline is signalled `count` times as it is created, so its payload starts far ahead of
    // what another stub's has reached: a value from the other waited on this one then completes
    // rather than stalling the GPU, and only an import count can tell the pairing was wrong.
    void advanceTimelinesOnGrant(int count) { m_advanceOnGrant = count; }
    // GetFrameFd replies carry `value` as a thirteenth field and no sync_fds: a daemon answering
    // the method nobody asked for a value on as if they had.
    void sendValueOnGetFrameFd(quint64 value) { m_valueOnGetFrameFd = value; }
    // Numbers this stub's frames from `offset` + 1 instead of 1. Set before the first serve.
    void numberGenerationsFrom(quint64 offset) { m_generationOffset = offset; }
    // The generation the next frame served will carry: what a FrameReady announcing it names.
    quint64 nextGeneration() const { return m_generationOffset + static_cast<quint64>(m_served) + 1; }

    int timelineAsks() const { return static_cast<int>(m_askTimesMs.size()); }
    // When each ask arrived, in milliseconds since this stub was made.
    const std::vector<qint64>& timelineAskTimesMs() const { return m_askTimesMs; }
    int timelinesGranted() const { return m_timelinesGranted; }
    int onTimelineRequests() const { return m_onTimelineRequests; }
    // Pictures that went out as a value on a timeline this stub granted.
    int valuesSent() const { return m_valuesSent; }

    // Every SetImportModifiers the item sent, answered with success as the daemon answers one it
    // accepts. The only view there is of a withdrawal: the plugin withdraws a layout by reporting
    // its list again without it.
    struct ModifierReport {
        quint32 drmFormat = 0;
        std::vector<quint64> modifiers;
    };
    const std::vector<ModifierReport>& modifierReports() const { return m_modifierReports; }

    // Sends it. False when there is none, which a caller must branch on: a phase that thinks it
    // released a reply and did not would grade the item against a request still outstanding.
    bool sendHeldReply() {
        if (!m_heldConnection) return false;
        const bool sent = m_heldConnection->send(m_heldReply);
        m_heldConnection.reset();
        m_heldReply = QDBusMessage();
        return sent;
    }

    QString introspect(const QString&) const override { return QString(); }

    bool handleMessage(const QDBusMessage& message, const QDBusConnection& connection) override {
        if (message.interface() != QLatin1String(kInterface)) return false;
        if (message.member() == QLatin1String("ReleaseBuffer")) {
            const QList<QVariant> args = message.arguments();
            if (args.size() >= 2) {
                m_releases.push_back(Release{args.at(0).toInt(), args.at(1).toULongLong()});
            } else {
                ++m_malformedReleases;
            }
            return connection.send(message.createReply());
        }
        if (message.member() == QLatin1String("SetImportModifiers")) {
            const QList<QVariant> args = message.arguments();
            ModifierReport report;
            if (args.size() >= 2) {
                report.drmFormat = args.at(0).toUInt();
                const QDBusArgument list = args.at(1).value<QDBusArgument>();
                list.beginArray();
                while (!list.atEnd()) {
                    quint64 modifier = 0;
                    list >> modifier;
                    report.modifiers.push_back(modifier);
                }
                list.endArray();
            }
            m_modifierReports.push_back(std::move(report));
            return connection.send(message.createReply());
        }
        if (message.member() == QLatin1String("GetAcquireTimeline")) return answerTimeline(message, connection);
        const bool onTimeline = message.member() == QLatin1String("GetFrameFdOnTimeline");
        if (!onTimeline && message.member() != QLatin1String("GetFrameFd")) return false;
        // Counted before anything here can go wrong, so "the item asked" and "the stub answered"
        // stay separate verdicts.
        ++m_requests;
        if (onTimeline) ++m_onTimelineRequests;
        if (!m_refuseWith.isEmpty()) {
            return connection.send(message.createErrorReply(
                m_refuseWith, QStringLiteral("this stub was told to refuse every further hand-over")));
        }
        if (!m_rotate && m_served >= static_cast<int>(m_sequence.size())) {
            return connection.send(
                message.createErrorReply(QStringLiteral("io.wallpaperx.Error.NoFrame"),
                                         QStringLiteral("this stub's frame sequence is exhausted")));
        }
        const wallpaperx::OffscreenTarget& served =
            m_serveNext != nullptr ? *std::exchange(m_serveNext, nullptr)
                                   : *m_sequence[static_cast<size_t>(m_served) % m_sequence.size()];

        wallpaperx::ExportedFrame frame;
        try {
            frame = wallpaperx::exportFrame(m_device, served);
        } catch (const std::exception& e) {
            m_exportError = e.what();
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QString::fromStdString(m_exportError)));
        }
        if (frame.fd < 0) {
            m_exportError = "exportFrame produced no descriptor";
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Error.ExportFailed"),
                                                            QString::fromStdString(m_exportError)));
        }

        QDBusUnixFileDescriptor handle(frame.fd);
        ::close(frame.fd);  // QDBusUnixFileDescriptor took its own duplicate.

        // The acquire descriptor, exported the way the daemon exports it: a fresh already-signalled
        // binary semaphore per hand-over. Unconditional rather than opt-in, because it is what the
        // daemon sends whenever it can, and a stub that served an easier reply than the real
        // producer does would leave the item's whole receive path graded against a shape nothing
        // produces.
        //
        // A driver that cannot export one sends none and says so, rather than failing the reply:
        // that is exactly what the daemon does, and a caller can then tell "this host has no
        // acquire" from "the plugin lost it".
        QList<QDBusUnixFileDescriptor> syncFds;
        // Numbered as this reply will be once it is counted below.
        Acquire acquireMode = Acquire::Real;
        if (auto it = m_acquireFrom.upper_bound(m_served + 1); it != m_acquireFrom.begin()) {
            acquireMode = std::prev(it)->second;
        }
        // A value only for a request that asked on the timeline this stub granted the caller, as
        // the daemon does; everyone else gets sync_fds and, on the timeline method, a value of 0.
        quint64 value = 0;
        const auto timeline = m_timelines.find(message.service());
        const bool valued = onTimeline && timeline != m_timelines.end() && timeline->second;
        if (valued) {
            try {
                value = timeline->second->signal(served);
                ++m_valuesSent;
            } catch (const std::exception& e) {
                m_acquireError = e.what();
            }
        } else if (!onTimeline && m_valueOnGetFrameFd != 0) {
            value = m_valueOnGetFrameFd;
        } else if (acquireMode == Acquire::Real) {
            try {
                const int acquireFd = wallpaperx::exportFrameAcquireFd(m_device, served);
                if (acquireFd >= 0) {
                    syncFds.append(QDBusUnixFileDescriptor(acquireFd));
                    ::close(acquireFd);
                }
            } catch (const std::exception& e) {
                m_acquireError = e.what();
            }
        } else if (acquireMode == Acquire::Unusable) {
            const int notASemaphore = ::memfd_create("wallpaperx-stub-not-a-semaphore", MFD_CLOEXEC);
            if (notASemaphore >= 0) {
                syncFds.append(QDBusUnixFileDescriptor(notASemaphore));
                ::close(notASemaphore);
                ++m_unusableAcquiresSent;
            }
        }
        if (acquireMode == Acquire::Real && value == 0) m_acquireFdsSent += static_cast<int>(syncFds.size());

        QList<FramePlane> planes;
        for (const wallpaperx::ExportedFramePlane& plane : frame.planes) {
            planes.append(FramePlane{plane.offset, plane.rowPitch});
        }

        m_handleType = frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf ? 1 : 0;
        m_lastModifier = frame.modifier;
        ++m_served;
        // One generation per frame served, starting at 1. The item ignores a FrameReady naming a
        // generation it already holds, so a stub that answered 1 for every frame could never be
        // made to fetch a second one.
        const quint64 generation = m_generationOffset + static_cast<quint64>(m_served);
        // Both taken from the export rather than invented: the importer refuses a frame whose
        // device is not the one this context runs on, and these tests import for real.
        QList<QVariant> fields =
            frameReplyFields(handle, frame.width, frame.height, frame.memorySize, m_handleType, generation,
                             frame.drmFormat, frame.modifier, planes,
                             QByteArray(reinterpret_cast<const char*>(frame.deviceUuid.data()),
                                        static_cast<qsizetype>(frame.deviceUuid.size())),
                             m_bufferIdOverride != 0 ? m_bufferIdOverride : frame.bufferId, syncFds);
        if (onTimeline || value != 0) fields.append(QVariant::fromValue(static_cast<qulonglong>(value)));
        const QDBusMessage reply = message.createReply(fields);
        if (m_holdEvery) {
            m_heldQueue.emplace_back(connection, reply);
            return true;
        }
        if (m_holdNext || m_served == m_holdAtServe) {
            m_holdNext = false;
            if (m_served == m_holdAtServe) m_holdAtServe = -1;
            m_heldReply = reply;
            m_heldConnection = connection;
            return true;  // handled: the caller must not be given Qt's unknown-method error
        }
        return connection.send(reply);
    }

    int requests() const { return m_requests; }
    int served() const { return m_served; }
    int handleType() const { return m_handleType; }
    // The DRM format modifier of the last frame served: the layout a withdrawal would name.
    quint64 lastModifier() const { return m_lastModifier; }
    const std::string& exportError() const { return m_exportError; }

    // What the plugin released, in the order it said so. Empty is a real answer, and the checks
    // that read it say which of empty and non-empty they expect.
    const std::vector<Release>& releases() const { return m_releases; }
    // A ReleaseBuffer whose arguments this stub could not read. Its own counter so a wire shape
    // this harness cannot parse is not silently counted as "no release was sent".
    int malformedReleases() const { return m_malformedReleases; }

    // How many acquire descriptors went out with the frames served. Read by the checks that are
    // ABOUT the acquire, so a stub that quietly stopped sending one turns them from a claim about
    // the plugin into a claim about nothing.
    int acquireFdsSent() const { return m_acquireFdsSent; }
    const std::string& acquireError() const { return m_acquireError; }
    // How many replies went out with an acquire the importer cannot wait on.
    int unusableAcquiresSent() const { return m_unusableAcquiresSent; }

private:
    bool answerTimeline(const QDBusMessage& message, const QDBusConnection& connection) {
        m_askTimesMs.push_back(m_clock.elapsed());
        if (m_timelineRefusals > 0) {
            --m_timelineRefusals;
            return connection.send(
                message.createErrorReply(m_timelineRefusal, QStringLiteral("this stub was told to refuse this ask")));
        }
        if (m_timelineMode == Timeline::Absent) {
            return connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"),
                                                            QStringLiteral("no method GetAcquireTimeline here")));
        }
        std::unique_ptr<wallpaperx::AcquireTimeline>& timeline = m_timelines[message.service()];
        try {
            if (!timeline) {
                timeline = std::make_unique<wallpaperx::AcquireTimeline>(m_device);
                for (int i = 0; i < m_advanceOnGrant; ++i) timeline->signal(*m_sequence.front());
            }
        } catch (const std::exception& e) {
            m_acquireError = e.what();
            timeline.reset();
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Daemon.Error.Unsupported"),
                                                            QString::fromStdString(m_acquireError)));
        }
        if (m_timelineMode == Timeline::Silent) return true;  // registered, and the answer never comes
        int fd = timeline->exportFd();
        if (m_timelineMode == Timeline::BadDescriptor) {
            ::close(fd);
            fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        }
        QDBusUnixFileDescriptor handle(fd);
        ::close(fd);  // QDBusUnixFileDescriptor took its own duplicate.
        ++m_timelinesGranted;
        return connection.send(message.createReply(QList<QVariant>{QVariant::fromValue(handle)}));
    }

    bool m_holdEvery = false;
    std::vector<std::pair<QDBusConnection, QDBusMessage>> m_heldQueue;
    Timeline m_timelineMode = Timeline::Absent;
    int m_timelineRefusals = 0;
    QString m_timelineRefusal;
    int m_advanceOnGrant = 0;
    quint64 m_valueOnGetFrameFd = 0;
    quint64 m_generationOffset = 0;
    std::vector<qint64> m_askTimesMs;
    int m_timelinesGranted = 0;
    int m_onTimelineRequests = 0;
    int m_valuesSent = 0;
    QElapsedTimer m_clock = [] {
        QElapsedTimer started;
        started.start();
        return started;
    }();
    std::map<QString, std::unique_ptr<wallpaperx::AcquireTimeline>> m_timelines;
    bool m_rotate = false;
    const wallpaperx::OffscreenTarget* m_serveNext = nullptr;
    quint64 m_lastModifier = 0;
    int m_holdAtServe = -1;
    std::map<int, Acquire> m_acquireFrom;
    int m_unusableAcquiresSent = 0;
    std::vector<ModifierReport> m_modifierReports;
    wallpaperx::Device& m_device;
    std::vector<const wallpaperx::OffscreenTarget*> m_sequence;
    quint64 m_bufferIdOverride = 0;
    int m_requests = 0;
    int m_served = 0;
    int m_handleType = -1;
    int m_acquireFdsSent = 0;
    int m_malformedReleases = 0;
    QString m_refuseWith;
    bool m_holdNext = false;
    QDBusMessage m_heldReply;
    std::optional<QDBusConnection> m_heldConnection;
    std::vector<Release> m_releases;
    std::string m_exportError;
    std::string m_acquireError;
};

// Whether the GL context must be on the same GPU as the exporter. Only an opaque fd is device
// bound; a dma_buf imports across devices. Asked of a real export, because the stub does not know
// its handle type until it has served a frame, which is after every caller's gate.
inline bool exportNeedsSameDevice(wallpaperx::Device& device, const wallpaperx::OffscreenTarget& target) {
    wallpaperx::ExportedFrame probe = wallpaperx::exportFrame(device, target);
    const bool needs = probe.type != wallpaperx::ExportedFrame::HandleType::DmaBuf;
    if (probe.fd >= 0) ::close(probe.fd);
    return needs;
}

inline QSGNode* itemPaintNode(QQuickItem* item) {
    return item == nullptr ? nullptr : QQuickItemPrivate::get(item)->paintNode;
}

inline QSGTexture* itemTexture(QQuickItem* item) {
    auto* node = static_cast<QSGSimpleTextureNode*>(itemPaintNode(item));
    return node == nullptr ? nullptr : node->texture();
}

inline bool claimsFrameShown(QQuickItem* item) { return item->property("frameShown").toBool(); }

// GL_DEVICE_UUID_EXT, read from whatever context is current. False when the context has no
// GL_EXT_memory_object -- which is also a context that could not have imported anything.
inline bool glDeviceUuid(std::array<uint8_t, 16>& out) {
    using GetUnsignedBytevFn = void (*)(GLenum, GLubyte*);
    auto getBytes = reinterpret_cast<GetUnsignedBytevFn>(eglGetProcAddress("glGetUnsignedBytevEXT"));
    if (getBytes == nullptr) return false;
    while (glGetError() != GL_NO_ERROR) {
    }
    constexpr GLenum kDeviceUuidExt = 0x9597;  // GL_DEVICE_UUID_EXT
    getBytes(kDeviceUuidExt, out.data());
    return glGetError() == GL_NO_ERROR;
}

// Instantiates the plugin's own registered type, so a module that fails to load or a type that
// stops resolving is a failure of the thing under test rather than of this scene.
inline QString qmlSource() {
    return QStringLiteral(R"(
import QtQuick
import com.wallpaperx

Item {
    width: 68
    height: 36
    property Item rendererItem: renderer
    // Read only by the "never imported" skip, so that a run which could not set up says what the
    // item thought was wrong instead of naming nothing.
    property string publishedReason: renderer.unavailableReason

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
    }
}
)");
}

}  // namespace wallpaperx::testing
