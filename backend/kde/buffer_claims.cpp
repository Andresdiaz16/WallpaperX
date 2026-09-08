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

BufferClaims& BufferClaims::instance() {
    static BufferClaims* claims = new BufferClaims();
    return *claims;
}

}  // namespace wallpaperx
