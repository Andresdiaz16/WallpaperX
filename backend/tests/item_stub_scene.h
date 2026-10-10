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

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
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
        quint64 handover = 0;
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

    // Builds the next GetFrame reply as usual but does not send it until sendHeldReply().
    //
    // For the one state a synchronous stub cannot produce: a reply IN FLIGHT while the item
    // changes which monitor it shows. The daemon records the hold when it answers, so that reply
    // grants a claim to an item that no longer wants it, and only a stub that can pause between
    // the two halves puts the plugin in that position at all.
    // Refuses every further GetFrame with this error name, whatever the sequence has left; an
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
    // payload, which the driver refuses at import. Late creates it and holds the request until
    // answerHeldTimeline(), for an answer that arrives as late as a test chooses.
    enum class Timeline { Absent, Grant, Silent, BadDescriptor, Late };
    void serveTimelines(Timeline mode) { m_timelineMode = mode; }
    size_t heldTimelineAsks() const { return m_heldTimelineAsks.size(); }
    // Answers the oldest held ask with the caller's timeline, or with a duplicate of `instead` when it
    // is given (the caller keeps `instead`). False when none is held or the descriptor cannot be made.
    bool answerHeldTimeline(int instead = -1) {
        if (m_heldTimelineAsks.empty()) return false;
        auto [connection, message] = std::move(m_heldTimelineAsks.front());
        m_heldTimelineAsks.erase(m_heldTimelineAsks.begin());
        int fd = -1;
        if (instead >= 0) {
            fd = ::fcntl(instead, F_DUPFD_CLOEXEC, 0);
        } else if (const auto it = m_timelines.find(message.service()); it != m_timelines.end() && it->second) {
            fd = it->second->exportFd();
        }
        if (fd < 0) return false;
        QDBusUnixFileDescriptor handle(fd);
        ::close(fd);  // QDBusUnixFileDescriptor took its own duplicate.
        if (instead < 0) ++m_timelinesGranted;
        return connection.send(message.createReply(QList<QVariant>{QVariant::fromValue(handle)}));
    }
    // The next `count` asks are answered with `errorName`, whatever the mode.
    void refuseTimelineAsks(int count, QString errorName) {
        m_timelineRefusals = count;
        m_timelineRefusal = std::move(errorName);
    }
    // Each timeline is signalled `count` times as it is created, so its payload starts far ahead of
    // what another stub's has reached: a value from the other waited on this one then completes
    // rather than stalling the GPU, and only an import count can tell the pairing was wrong.
    void advanceTimelinesOnGrant(int count) { m_advanceOnGrant = count; }
    // Replies to requests that did NOT ask on the timeline carry `value` as acquire_value and no
    // sync_fds: a daemon answering a request nobody asked for a value on as if they had.
    void sendValueUnasked(quint64 value) { m_valueUnasked = value; }

    // Whether a reply carries the memory. `Daemon` is the daemon's rule: left out exactly when the
    // request declared the image served and the reply carries an acquire. `Always` is legal too --
    // memory the caller did not need, which it closes. The rest break the protocol on purpose:
    // `OmitUndeclared` leaves it out of every reply whatever was declared, `Twice` sends two
    // descriptors for one image.
    enum class Memory { Daemon, Always, OmitUndeclared, Twice };
    void sendMemory(Memory rule) { m_memoryRule = rule; }

    // What each GetFrame said, in arrival order.
    struct Ask {
        int monitorId = -1;
        std::vector<quint64> declared;
        bool onTimeline = false;
        // The image the reply served, whether it carried the memory, and its hand-over number; 0,
        // false and 0 for a request refused.
        quint64 served = 0;
        bool memorySent = false;
        quint64 handover = 0;
        // When it arrived, on this stub's clock (nowMs()). It is answered at once unless held, so this is
        // also when its answer left.
        qint64 arrivedMs = 0;
    };
    const std::vector<Ask>& asks() const { return m_asks; }
    qint64 nowMs() const { return m_clock.elapsed(); }
    // Replies that left the memory out.
    int memoryOmitted() const { return m_memoryOmitted; }
    // Numbers this stub's frames from `offset` + 1 instead of 1. Set before the first serve.
    void numberGenerationsFrom(quint64 offset) { m_generationOffset = offset; }
    // Numbers this stub's hand-overs from `offset` + 1 instead of 1. Set before the first serve.
    void numberHandoversFrom(quint64 offset) { m_lastHandover = offset; }
    // The highest hand-over number this stub has issued.
    quint64 lastHandover() const { return m_lastHandover; }
    // When this stub's clock started, on QElapsedTimer's monotonic reference, so times two stubs
    // stamped can be compared.
    qint64 clockReferenceMs() const { return m_clock.msecsSinceReference(); }

    // Keeps a daemon's ledger of what the caller holds, as the daemon does: every reply is a
    // numbered hand-over of its image, a release names a number and ends the claim only if it
    // covers the newest, and an image held is never served again -- the next one in the sequence
    // is, or NoFreeImage when every image is held. A claim ending is announced as a FrameReady for
    // the newest generation served, which is how a refused caller learns to ask again.
    //
    // Off by default: the plain stub serves its sequence whatever was released, which hides a
    // leaked hold -- a later serve and release of the same id clears it. A serveNext() image the
    // caller still holds is refused, not replaced, and counted in forcedHeld().
    void respectHolds(QDBusConnection announceOn) { m_announceOn = std::move(announceOn); }
    int forcedHeld() const { return m_forcedHeld; }

    // Refuses a reply as TooManyDescriptors, as the daemon does, once the descriptors it would carry
    // and those earlier replies carried that no release has covered would pass `cap`. A release
    // covers the replies of its image numbered up to the one it names. One caller only: every
    // charge is the item's. 0, the default, applies no cap.
    void enforceDescriptorCap(int cap) { m_descriptorCap = cap; }
    int descriptorRefusals() const { return m_descriptorRefusals; }
    int descriptorsCharged() const {
        int total = 0;
        for (const Charge& charge : m_charges) total += charge.descriptors;
        return total;
    }
    // The images whose claim still stands, ascending.
    std::vector<quint64> outstanding() const {
        std::vector<quint64> held;
        for (const auto& [id, claim] : m_claims) {
            if (claim.released < claim.newest) held.push_back(id);
        }
        return held;
    }
    // Every serve (S), refusal for want of a free image (X) and claim ended (R), in order, with the
    // image and hand-over: the trace a stall is read from.
    const std::string& ledgerLog() const { return m_ledgerLog; }
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
            const ReleaseArgs release = readReleaseArgs(message.arguments());
            if (!release.ok) {
                ++m_malformedReleases;
                return connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                                                QStringLiteral("this stub could not read the release")));
            }
            m_releases.push_back(Release{release.monitorId, release.bufferId, release.handover});
            std::erase_if(m_charges, [&release](const Charge& charge) {
                return charge.bufferId == release.bufferId && charge.handover <= release.handover;
            });
            if (m_announceOn) releaseClaim(release.bufferId, release.handover);
            return connection.send(message.createReply());
        }
        if (message.member() == QLatin1String("SetImportModifiers")) {
            ModifierReportArgs args = readModifierReport(message.arguments());
            if (!args.ok) {
                ++m_malformedModifierReports;
                return connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                                                QStringLiteral("this stub could not read the report")));
            }
            m_modifierReports.push_back(ModifierReport{args.drmFormat, std::move(args.modifiers)});
            return connection.send(message.createReply());
        }
        if (message.member() == QLatin1String("GetAcquireTimeline")) return answerTimeline(message, connection);
        if (message.member() != QLatin1String("GetFrame")) return false;
        // Counted before anything here can go wrong, so "the item asked" and "the stub answered"
        // stay separate verdicts.
        ++m_requests;
        const FrameRequestArgs request = readFrameRequest(message.arguments());
        if (!request.ok) {
            ++m_malformedRequests;
            return connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                                            QStringLiteral("this stub could not read the request")));
        }
        const bool onTimeline = request.onTimeline;
        if (onTimeline) ++m_onTimelineRequests;
        m_asks.push_back(Ask{request.monitorId, request.declared, onTimeline});
        m_asks.back().arrivedMs = m_clock.elapsed();
        if (!m_refuseWith.isEmpty()) {
            return connection.send(message.createErrorReply(
                m_refuseWith, QStringLiteral("this stub was told to refuse every further hand-over")));
        }
        if (!m_rotate && m_served >= static_cast<int>(m_sequence.size())) {
            return connection.send(
                message.createErrorReply(QStringLiteral("io.wallpaperx.Error.NoFrame"),
                                         QStringLiteral("this stub's frame sequence is exhausted")));
        }
        const wallpaperx::OffscreenTarget* forced = std::exchange(m_serveNext, nullptr);
        const wallpaperx::OffscreenTarget* pick =
            forced != nullptr ? forced : m_sequence[static_cast<size_t>(m_served) % m_sequence.size()];
        if (m_announceOn && forced != nullptr &&
            isHeld(m_bufferIdOverride != 0 ? m_bufferIdOverride : forced->bufferId)) {
            // A staged image the caller still holds: serving another in its place would grade a reply
            // the test never staged, so the conflict is refused and counted for the test to report.
            ++m_forcedHeld;
            return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Daemon.Error.NoFreeImage"),
                                                            QStringLiteral("this stub's staged image is held")));
        }
        if (m_announceOn && forced == nullptr) {
            // The next image of the sequence nobody holds, as the daemon draws into the next free one.
            pick = nullptr;
            for (size_t step = 0; step < m_sequence.size() && pick == nullptr; ++step) {
                const wallpaperx::OffscreenTarget* candidate =
                    m_sequence[(static_cast<size_t>(m_served) + step) % m_sequence.size()];
                if (!isHeld(m_bufferIdOverride != 0 ? m_bufferIdOverride : candidate->bufferId)) pick = candidate;
            }
            if (pick == nullptr) {
                m_ledgerLog += "X0 ";
                m_refusedForHolds = true;
                m_refusedMonitor = request.monitorId;
                m_asks.back().served = 0;
                return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Daemon.Error.NoFreeImage"),
                                                                QStringLiteral("this stub holds no free image")));
            }
        }
        const wallpaperx::OffscreenTarget& served = *pick;

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

        // QDBusUnixFileDescriptor takes its own duplicate.
        QDBusUnixFileDescriptor handle(frame.fd);
        ::close(frame.fd);

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
        // What this reply's acquire was made as; counted as sent only once the reply is past the cap.
        int valuesMade = 0;
        bool unusableMade = false;
        m_lateInThisReply = false;
        const auto timeline = m_timelines.find(message.service());
        const bool valued = onTimeline && timeline != m_timelines.end() && timeline->second;
        const bool ownValued = onTimeline && m_ownTimeline != VK_NULL_HANDLE && message.service() == m_ownCaller;
        if (ownValued) {
            try {
                value = signalOwnValue();
                ++valuesMade;
            } catch (const std::exception& e) {
                m_acquireError = e.what();
            }
        } else if (valued) {
            try {
                value = timeline->second->signal(served);
                ++valuesMade;
            } catch (const std::exception& e) {
                m_acquireError = e.what();
            }
        } else if (!onTimeline && m_valueUnasked != 0) {
            value = m_valueUnasked;
        } else if (acquireMode == Acquire::Real && m_lateBinaryNext) {
            m_lateBinaryNext = false;
            try {
                const int lateFd = exportLateBinary();
                syncFds.append(QDBusUnixFileDescriptor(lateFd));
                ::close(lateFd);
            } catch (const std::exception& e) {
                m_acquireError = e.what();
            }
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
                unusableMade = true;
            }
        }

        QList<FramePlane> planes;
        for (const wallpaperx::ExportedFramePlane& plane : frame.planes) {
            planes.append(FramePlane{plane.offset, plane.rowPitch});
        }

        m_handleType = frame.type == wallpaperx::ExportedFrame::HandleType::DmaBuf ? 1 : 0;
        m_lastModifier = frame.modifier;
        const quint64 bufferId = m_bufferIdOverride != 0 ? m_bufferIdOverride : frame.bufferId;
        // A value counts only for a request that asked for one, as on the daemon, which sends none
        // otherwise: one sent unasked is an acquire the caller does not read.
        const bool carriesAcquire = (onTimeline && value != 0) || !syncFds.isEmpty();
        const bool declared = std::find(request.declared.begin(), request.declared.end(), bufferId) !=
                              request.declared.end();
        QList<QDBusUnixFileDescriptor> memory;
        switch (m_memoryRule) {
            case Memory::Daemon:
                if (!(declared && carriesAcquire)) memory.append(handle);
                break;
            case Memory::Always:
                memory.append(handle);
                break;
            case Memory::OmitUndeclared:
                break;
            case Memory::Twice:
                memory.append(handle);
                memory.append(handle);
                break;
        }
        // The daemon's cap on descriptors sent and not given back, applied as the daemon applies it:
        // what this reply would carry, admitted against what earlier replies carried and no release
        // has covered yet.
        const int carried = static_cast<int>(memory.size() + syncFds.size());
        if (m_descriptorCap > 0 && descriptorsCharged() + carried > m_descriptorCap) {
            ++m_descriptorRefusals;
            return connection.send(
                message.createErrorReply(QStringLiteral("io.wallpaperx.Daemon.Error.TooManyDescriptors"),
                                         QStringLiteral("this stub's caller holds too many descriptors")));
        }
        // Past the cap, so the reply goes out (or is held to go out): only now is its acquire sent.
        m_valuesSent += valuesMade;
        if (unusableMade) ++m_unusableAcquiresSent;
        if (acquireMode == Acquire::Real && value == 0) m_acquireFdsSent += static_cast<int>(syncFds.size());
        if (m_lateInThisReply) m_lateDelivered = true;
        ++m_served;
        // One generation per frame served, starting at 1. The item ignores a FrameReady naming a
        // generation it already holds, so a stub that answered 1 for every frame could never be
        // made to fetch a second one.
        const quint64 generation = m_generationOffset + static_cast<quint64>(m_served);
        if (memory.isEmpty()) ++m_memoryOmitted;
        // One number per reply, from a sequence that only rises, as the daemon's does.
        const quint64 handover = ++m_lastHandover;
        if (m_descriptorCap > 0 && carried > 0) m_charges.push_back(Charge{bufferId, handover, carried});
        m_asks.back().served = bufferId;
        m_asks.back().memorySent = !memory.isEmpty();
        m_asks.back().handover = handover;
        if (m_announceOn) {
            m_claims[bufferId].newest = handover;
            m_claims[bufferId].monitorId = request.monitorId;
            m_newestByMonitor[request.monitorId] = generation;
            m_refusedForHolds = false;
            m_ledgerLog += "S" + std::to_string(bufferId) + "#" + std::to_string(handover) + " ";
        }
        // Both taken from the export rather than invented: the importer refuses a frame whose
        // device is not the one this context runs on, and these tests import for real.
        const QList<QVariant> fields =
            frameReplyFields(memory, frame.width, frame.height, frame.memorySize, m_handleType, generation,
                             frame.drmFormat, frame.modifier, planes,
                             QByteArray(reinterpret_cast<const char*>(frame.deviceUuid.data()),
                                        static_cast<qsizetype>(frame.deviceUuid.size())),
                             bufferId, handover, syncFds, value);
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
    // Requests whose arguments this stub could not read, answered InvalidArgs.
    int malformedRequests() const { return m_malformedRequests; }
    // The same for SetImportModifiers.
    int malformedModifierReports() const { return m_malformedModifierReports; }
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

    // GetAcquireTimeline hands out this stub's own exportable timeline semaphore instead of an
    // AcquireTimeline, one caller only, so a value's signal can be submitted and yet held back on
    // the GPU (lateValueNext()). Set before the item asks.
    void ownTimelines() { m_ownTimelines = true; }
    // The next value handed out on the stub's own timeline is signalled by a batch submitted at
    // once, behind a gate the host opens in signalLateValue() and behind `drawWhileHeld`, which is
    // called between the two submissions: a producer whose drawing and signal are on their way but
    // not done. The signal is SUBMITTED before the item can wait on it, since nothing may wait on a
    // signal not yet submitted -- measured on this driver, such a wait does not hold the reader.
    // `drawWhileHeld` must not wait on the device: the gate holds the queue.
    void lateValueNext(std::function<void()> drawWhileHeld) {
        m_lateValueNext = true;
        m_drawWhileHeld = std::move(drawWhileHeld);
    }
    // The same for the next reply sent as sync_fds: its binary semaphore's signal is submitted
    // behind the gate and `drawWhileHeld`, and exported while pending. Opened by signalLateValue().
    void lateBinaryNext(std::function<void()> drawWhileHeld) {
        m_lateBinaryNext = true;
        m_drawWhileHeld = std::move(drawWhileHeld);
    }
    // Whether the late acquire went into a reply past the descriptor cap: sent, or held by this stub to
    // be sent. A reply the cap refused does not count. A failure making it is in acquireError().
    bool lateValueSent() const { return m_lateDelivered; }
    // Submitted and held behind the gate: signalLateValue() not yet called.
    bool lateValuePending() const { return m_latePending; }
    bool signalLateValue(std::string& why) {
        if (!m_latePending) {
            why = "no late value was handed out";
            return false;
        }
        VkSemaphoreSignalInfo open{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        open.semaphore = m_gate;
        open.value = m_gateValue;
        if (const VkResult r = vkSignalSemaphore(m_device.device(), &open); r != VK_SUCCESS) {
            why = "vkSignalSemaphore on the gate returned " + std::to_string(static_cast<int>(r));
            return false;
        }
        try {
            m_device.wait(m_lateTicket);
        } catch (const std::exception& e) {
            why = e.what();
            return false;
        }
        m_latePending = false;
        return true;
    }
    ~ExportingStub() override {
        if (m_latePending) {
            std::string ignored;
            signalLateValue(ignored);
        }
        if (m_gate != VK_NULL_HANDLE) vkDestroySemaphore(m_device.device(), m_gate, nullptr);
        if (m_ownTimeline != VK_NULL_HANDLE) vkDestroySemaphore(m_device.device(), m_ownTimeline, nullptr);
        if (m_lateBinary != VK_NULL_HANDLE) vkDestroySemaphore(m_device.device(), m_lateBinary, nullptr);
    }

private:
    // A timeline semaphore at value 0, exportable as an opaque fd when `exportable`.
    VkSemaphore makeTimeline(bool exportable) {
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.pNext = exportable ? &exportInfo : nullptr;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &type;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (vkCreateSemaphore(m_device.device(), &info, nullptr, &semaphore) != VK_SUCCESS) return VK_NULL_HANDLE;
        return semaphore;
    }
    // An opaque-fd export of `semaphore`. Throws when the driver gives none.
    int exportSemaphoreFd(VkSemaphore semaphore) {
        const auto getFd =
            reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(m_device.device(), "vkGetSemaphoreFdKHR"));
        VkSemaphoreGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        fdInfo.semaphore = semaphore;
        fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (getFd == nullptr || getFd(m_device.device(), &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
            throw std::runtime_error("the stub could not export a semaphore descriptor");
        }
        return fd;
    }
    // Submits `scope` behind the gate signalLateValue() opens, with `drawWhileHeld` run between the
    // two submissions. m_latePending is set once the work is queued, whatever happens after: the
    // destructor must open the gate before it destroys anything that work waits on.
    void submitBehindGate(const wallpaperx::Device::SubmitScope& scope) {
        wallpaperx::Device::SubmitScope gate;
        gate.timelineWait.emplace_back(m_gate, ++m_gateValue, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        m_device.submit([](VkCommandBuffer) {}, gate);
        if (m_drawWhileHeld) std::exchange(m_drawWhileHeld, nullptr)();
        m_lateTicket = m_device.submit([](VkCommandBuffer) {}, scope);
        m_latePending = true;
    }
    int exportOwnTimeline() {
        if (m_ownTimeline == VK_NULL_HANDLE) m_ownTimeline = makeTimeline(true);
        if (m_gate == VK_NULL_HANDLE) m_gate = makeTimeline(false);
        if (m_ownTimeline == VK_NULL_HANDLE || m_gate == VK_NULL_HANDLE) {
            throw std::runtime_error("the stub could not make its own timeline");
        }
        return exportSemaphoreFd(m_ownTimeline);
    }
    // The next value on the stub's own timeline, signalled after everything already submitted --
    // the render that published the picture among it -- and, for a late value, only once the gate
    // opens. An ordinary value is waited for, as the daemon waits before it answers.
    quint64 signalOwnValue() {
        const quint64 value = ++m_ownValue;
        wallpaperx::Device::SubmitScope scope;
        scope.timelineSignal.emplace_back(m_ownTimeline, value);
        if (m_lateValueNext) {
            m_lateValueNext = false;
            submitBehindGate(scope);
            m_lateInThisReply = true;
        } else {
            m_device.wait(m_device.submit([](VkCommandBuffer) {}, scope));
        }
        return value;
    }

    // A binary semaphore whose signal is submitted behind the gate, exported while pending. Throws
    // when the driver cannot make or export one.
    int exportLateBinary() {
        if (m_gate == VK_NULL_HANDLE) m_gate = makeTimeline(false);
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &exportInfo;
        if (m_gate == VK_NULL_HANDLE || m_lateBinary != VK_NULL_HANDLE ||
            vkCreateSemaphore(m_device.device(), &info, nullptr, &m_lateBinary) != VK_SUCCESS) {
            throw std::runtime_error("the stub could not make the late acquire semaphore");
        }
        wallpaperx::Device::SubmitScope scope;
        scope.signal = {m_lateBinary};
        submitBehindGate(scope);
        const int fd = exportSemaphoreFd(m_lateBinary);
        m_lateInThisReply = true;
        return fd;
    }

    bool m_lateDelivered = false;
    bool m_lateInThisReply = false;  // the reply being built carries the late acquire
    bool m_lateBinaryNext = false;
    VkSemaphore m_lateBinary = VK_NULL_HANDLE;
    bool m_ownTimelines = false;
    QString m_ownCaller;
    VkSemaphore m_ownTimeline = VK_NULL_HANDLE;
    VkSemaphore m_gate = VK_NULL_HANDLE;
    quint64 m_ownValue = 0;
    quint64 m_gateValue = 0;
    bool m_lateValueNext = false;
    std::function<void()> m_drawWhileHeld;
    bool m_latePending = false;
    wallpaperx::Device::SubmitTicket m_lateTicket;

    struct Claim {
        quint64 newest = 0;
        quint64 released = 0;
        int monitorId = kFirstMonitor;  // the monitor its newest reply was served for
    };

    // One reply's descriptors, charged until a release covers it.
    struct Charge {
        quint64 bufferId = 0;
        quint64 handover = 0;
        int descriptors = 0;
    };
    int m_descriptorCap = 0;
    std::vector<Charge> m_charges;
    int m_descriptorRefusals = 0;
    int m_forcedHeld = 0;
    int m_malformedModifierReports = 0;
    int m_refusedMonitor = kFirstMonitor;

    bool isHeld(quint64 bufferId) const {
        const auto it = m_claims.find(bufferId);
        return it != m_claims.end() && it->second.released < it->second.newest;
    }

    void releaseClaim(quint64 bufferId, quint64 handover) {
        const auto it = m_claims.find(bufferId);
        if (it == m_claims.end() || it->second.released >= it->second.newest) return;
        it->second.released = std::max(it->second.released, handover);
        if (it->second.released < it->second.newest) return;
        m_ledgerLog += "R" + std::to_string(bufferId) + "#" + std::to_string(handover) + " ";
        // The frame a caller refused for want of a free image was asking for, on the monitor it asked
        // for, if one was; otherwise the newest served on the monitor this image was, which a caller
        // that has it ignores.
        const int monitorId = m_refusedForHolds ? m_refusedMonitor : it->second.monitorId;
        const quint64 generation = m_refusedForHolds ? m_generationOffset + static_cast<quint64>(m_served) + 1
                                                     : m_newestByMonitor[monitorId];
        QDBusMessage ready = QDBusMessage::createSignal(QString::fromUtf8(kPath), QString::fromUtf8(kInterface),
                                                        QStringLiteral("FrameReady"));
        ready.setArguments({monitorId, static_cast<qulonglong>(generation)});
        m_announceOn->send(ready);
    }

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
        if (m_ownTimelines && m_timelineMode == Timeline::Grant) {
            int own = -1;
            try {
                own = exportOwnTimeline();
            } catch (const std::exception& e) {
                m_acquireError = e.what();
                return connection.send(message.createErrorReply(QStringLiteral("io.wallpaperx.Daemon.Error.Unsupported"),
                                                                QString::fromStdString(m_acquireError)));
            }
            m_ownCaller = message.service();
            QDBusUnixFileDescriptor handle(own);
            ::close(own);
            ++m_timelinesGranted;
            return connection.send(message.createReply(QList<QVariant>{QVariant::fromValue(handle)}));
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
        if (m_timelineMode == Timeline::Late) {
            m_heldTimelineAsks.emplace_back(connection, message);
            return true;
        }
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
    quint64 m_lastHandover = 0;
    std::optional<QDBusConnection> m_announceOn;
    std::map<quint64, Claim> m_claims;
    std::map<int, quint64> m_newestByMonitor;  // the newest generation served on each monitor
    bool m_refusedForHolds = false;
    std::string m_ledgerLog;
    std::vector<std::pair<QDBusConnection, QDBusMessage>> m_heldQueue;
    std::vector<std::pair<QDBusConnection, QDBusMessage>> m_heldTimelineAsks;
    Timeline m_timelineMode = Timeline::Absent;
    int m_timelineRefusals = 0;
    QString m_timelineRefusal;
    int m_advanceOnGrant = 0;
    quint64 m_valueUnasked = 0;
    Memory m_memoryRule = Memory::Daemon;
    std::vector<Ask> m_asks;
    int m_memoryOmitted = 0;
    int m_malformedRequests = 0;
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
    // Every reason the item published, in order, for a phase whose reason changes again within one
    // turn of the event loop.
    property string reasonsSeen: ""
    onPublishedReasonChanged: reasonsSeen += "[" + publishedReason + "]"

    WallpaperXRenderer {
        id: renderer
        anchors.fill: parent
        monitorId: 0
    }
}
)");
}

}  // namespace wallpaperx::testing
