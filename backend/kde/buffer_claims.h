// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace wallpaperx {

// Which of the daemon's images this process still has a live import of, and so what it may tell
// the daemon it has stopped reading.
//
// Process-wide, because the daemon's ledger is: it keys a hold on the caller's bus connection, so
// two items on one monitor -- which happens while screens are being reshuffled -- share one claim
// on any image they both hold. Counted, because one image is imported more than once at a time:
// the plugin re-fetches the frame it is already showing whenever the scene changes, and the older
// texture outlives the newer one's import.
//
// Free of Qt, like the importer beside it, so the decision can be graded without a bus, a window
// or a GPU. Its mutex is a leaf: nothing here calls out.
//
// It is also the process's one record of which daemon it is speaking to, kept with the claims
// because a claim means something only against that daemon, and two records could disagree.
class BufferClaims {
public:
    // A daemon lifetime: the stretch between two owner changes of the bus name, numbered, and the
    // unique name that owned it -- empty when nobody did. A unique name is never reused, so a
    // release addressed to it cannot reach a successor.
    //
    // Lifetime 0 is the stretch before the bus has said who owns the name. Its owner is empty
    // because it is not known, so nothing may be asked of the daemon in it.
    struct Daemon {
        uint64_t lifetime = 0;
        std::string owner;
    };

    // One hand-over, from the reply that granted it to the import whose end gives it back.
    //
    // `daemon` is the lifetime the granting request was asked in. Buffer ids restart when the
    // daemon does, so without it a release could name an image the CURRENT daemon has handed to
    // somebody -- this process included -- and take it out from under its reader.
    struct Claim {
        uint64_t daemon = 0;
        int monitorId = -1;
        uint64_t bufferId = 0;

        // A daemon never issues buffer id 0, so it is the "nothing claimed" state and no second
        // field is needed to say so.
        bool held() const { return bufferId != 0; }
        bool operator==(const Claim&) const = default;
    };

    // What to tell the daemon when an image is given back: who, and the newest hand-over of it this
    // process has taken -- which gives back every one up to it, and none the daemon made since.
    struct Release {
        std::string owner;
        uint64_t handover = 0;
    };

    // A reply taken: the claim it granted, and the earlier hand-over of the same image it supersedes,
    // when this process still holds an import of that image.
    //
    // `superseded` must be sent at once. The daemon charges every reply's descriptors until a release
    // names it, and this ledger otherwise names a hand-over only when the last import of its image
    // ends -- which for an image re-fetched while it stays on screen is never, until the daemon
    // refuses this process every picture. It names a number below the reply just taken, so it only
    // ever gives back replies already read and never ends the claim that reply holds.
    struct Taken {
        Claim claim;
        std::optional<Release> superseded;
    };

    // Records that the daemon of lifetime `daemon` has handed `bufferId` over as hand-over number
    // `handover`. A `bufferId` of 0 records nothing and returns an unheld claim, so a caller reading
    // a reply need not branch before asking. A claim from a lifetime that has ended, or one with no
    // owner to give it back to, is returned unrecorded: it still names the image, which is what an
    // import of it is found by, but no daemon's ledger holds it. Replies must be taken in the order
    // they arrived, or a release could name a hand-over this process has not read.
    [[nodiscard]] Taken take(uint64_t daemon, int monitorId, uint64_t bufferId, uint64_t handover);

    // Gives one back. The release to send only when this was the LAST live import of that image,
    // which is the one moment the daemon may be told. Nothing for a claim already given back, and
    // for one granted by a daemon since replaced.
    std::optional<Release> giveBack(const Claim& claim);

    // The bus reports that `owner` -- empty for nobody -- owns the daemon's name now. A new
    // lifetime, and every claim the previous one granted is dropped without being released: there
    // is nobody left to tell, and those ids are about to be handed out again by the new daemon.
    // Every report is a change: the caller must report each one once.
    void daemonReplaced(std::string owner);

    // The bus's answer to who owned the name when this process started watching it -- empty for
    // nobody -- which begins the first known lifetime. False, changing nothing, once a change has
    // been reported: those reports are newer.
    bool daemonNamed(std::string owner);

    Daemon currentDaemon();

    // The process's ledger. Never destroyed: the scene graph can tear a texture down at any point
    // during exit, and that teardown reads this.
    static BufferClaims& instance();

private:
    struct Entry {
        int monitorId = -1;
        uint64_t bufferId = 0;
        int imports = 0;
        uint64_t newest = 0;
    };

    std::mutex m_mutex;
    // Every entry was granted by m_daemon, whose owner is not empty, since take() records no other
    // and daemonReplaced() drops the rest. That invariant is what stops a reused buffer id from
    // matching a claim that outlived its daemon, and a release from going out unaddressed.
    Daemon m_daemon;
    std::vector<Entry> m_entries;
};

// The claims one reader of the daemon's images is holding right now, so each is given back
// exactly once -- by whichever comes first: the import that took it ending, or the reader
// stopping showing the image at all.
//
// Both are needed, and neither covers the other. An import ends when the scene graph destroys
// the texture, which is the only moment the image is provably finished with -- but a desktop
// that hides a wallpaper rather than destroying it may never destroy that texture, and the
// daemon keys a hold on the process, not on the item, so those images stay out of the ring for
// the session and every later hand-over is refused. Held in one place rather than in the
// destructors that would each do it, because two of them releasing the same claim would give
// back an image a second reader still holds.
//
// Free of Qt, like the ledger above: it decides only what may be released, never how.
class ClaimSet {
public:
    void add(const BufferClaims::Claim& claim);

    // Removes one and reports whether it was still here. False means something else has already
    // given it back, and the caller must not.
    bool remove(const BufferClaims::Claim& claim);

    // Every claim still here, emptied. Duplicates are kept: two live imports of one image are two
    // claims, and the ledger counts them.
    std::vector<BufferClaims::Claim> removeAll();

private:
    std::mutex m_mutex;
    std::vector<BufferClaims::Claim> m_claims;
};

}  // namespace wallpaperx
