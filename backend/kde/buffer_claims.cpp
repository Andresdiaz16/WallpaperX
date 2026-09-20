// SPDX-License-Identifier: GPL-2.0-or-later

#include "buffer_claims.h"

#include <algorithm>

namespace wallpaperx {

namespace {

// Linear because the list is as long as the imports this process has live at once -- a handful,
// two per monitor in the steady state.
template <typename Entries>
auto findEntry(Entries& entries, int monitorId, uint64_t bufferId) {
    return std::find_if(entries.begin(), entries.end(), [monitorId, bufferId](const auto& entry) {
        return entry.monitorId == monitorId && entry.bufferId == bufferId;
    });
}

}  // namespace

BufferClaims::Claim BufferClaims::take(int monitorId, uint64_t bufferId, std::string owner) {
    if (bufferId == 0) return {};

    std::lock_guard<std::mutex> lock(m_mutex);
    auto entry = findEntry(m_entries, monitorId, bufferId);
    if (entry == m_entries.end()) {
        m_entries.push_back(Entry{monitorId, bufferId, 1});
    } else {
        ++entry->imports;
    }
    return Claim{m_daemon, monitorId, bufferId, std::move(owner)};
}

bool BufferClaims::giveBack(const Claim& claim) {
    if (!claim.held()) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (claim.daemon != m_daemon) return false;
    auto entry = findEntry(m_entries, claim.monitorId, claim.bufferId);
    if (entry == m_entries.end()) return false;
    if (--entry->imports > 0) return false;
    m_entries.erase(entry);
    return true;
}

void BufferClaims::daemonReplaced() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_daemon;
    m_entries.clear();
}

void ClaimSet::add(const BufferClaims::Claim& claim) {
    if (!claim.held()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_claims.push_back(claim);
}

bool ClaimSet::remove(const BufferClaims::Claim& claim) {
    if (!claim.held()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    // The owner is not part of the identity: it is where a release is addressed, and the same
    // image can be claimed once before this process has learned the daemon's unique name and
    // again after.
    const auto held = std::find_if(m_claims.begin(), m_claims.end(), [&claim](const BufferClaims::Claim& mine) {
        return mine.daemon == claim.daemon && mine.monitorId == claim.monitorId && mine.bufferId == claim.bufferId;
    });
    if (held == m_claims.end()) return false;
    m_claims.erase(held);
    return true;
}

std::vector<BufferClaims::Claim> ClaimSet::removeAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<BufferClaims::Claim> held;
    held.swap(m_claims);
    return held;
}

BufferClaims& BufferClaims::instance() {
    static BufferClaims* claims = new BufferClaims();
    return *claims;
}

}  // namespace wallpaperx
