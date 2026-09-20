// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <mutex>
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
class BufferClaims {
public:
    // One hand-over, from the reply that granted it to the import whose end gives it back.
    //
    // `daemon` numbers the daemon lifetimes this process has watched go by. Buffer ids restart
    // when the daemon does, so without it a release could name an image the CURRENT daemon has
    // handed to somebody -- this process included -- and take it out from under its reader.
    struct Claim {
        uint64_t daemon = 0;
        int monitorId = -1;
        uint64_t bufferId = 0;

        // The unique bus name that granted it, which is where the release must be addressed:
        // the well-known name can have moved to a successor by the time one is sent. Empty when
        // the caller had no owner to name, and then the well-known name is all there is.
        std::string owner;

        // A daemon never issues buffer id 0, so it is the "nothing claimed" state and no second
        // field is needed to say so.
        bool held() const { return bufferId != 0; }
    };

    // Records that the daemon has handed `bufferId` over. A `bufferId` of 0 records nothing and
    // returns an unheld claim, so a caller reading a reply need not branch before asking.
    //
    // `owner` is carried on the claim and nowhere else: it is the release's destination, not part
    // of what identifies a claim, which is what `daemon` is for.
    Claim take(int monitorId, uint64_t bufferId, std::string owner = {});

    // Gives one back. True only when this was the LAST live import of that image, which is the
    // one moment the daemon may be told. False for a claim already given back, and for one
    // granted by a daemon that has since been replaced -- its ledger went with it.
    bool giveBack(const Claim& claim);

    // A different process owns the daemon's bus name now. Every claim the previous one granted is
    // dropped without being released: there is nobody left to tell, and those ids are about to be
    // handed out again by the new daemon.
    void daemonReplaced();

    // The process's ledger. Never destroyed: the scene graph can tear a texture down at any point
    // during exit, and that teardown reads this.
    static BufferClaims& instance();

private:
    struct Entry {
        int monitorId = -1;
        uint64_t bufferId = 0;
        int imports = 0;
    };

    std::mutex m_mutex;
    // Every entry was granted by m_daemon, since daemonReplaced() drops the rest. That invariant
    // is what stops a reused buffer id from matching a claim that outlived its daemon.
    uint64_t m_daemon = 0;
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
