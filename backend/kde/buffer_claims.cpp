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

BufferClaims::Taken BufferClaims::take(uint64_t daemon, int monitorId, uint64_t bufferId, uint64_t handover) {
    if (bufferId == 0) return {};

    Taken taken{Claim{daemon, monitorId, bufferId}, std::nullopt};
    std::lock_guard<std::mutex> lock(m_mutex);
    if (daemon == m_daemon.lifetime && !m_daemon.owner.empty()) {
        auto entry = findEntry(m_entries, monitorId, bufferId);
        if (entry == m_entries.end()) {
            m_entries.push_back(Entry{monitorId, bufferId, 1, handover});
        } else {
            ++entry->imports;
            // 0 is a reply that recorded no hand-over, and names nothing a release could.
            if (entry->newest != 0 && entry->newest < handover) {
                taken.superseded = Release{m_daemon.owner, entry->newest};
            }
            entry->newest = std::max(entry->newest, handover);
        }
    }
    return taken;
}

std::optional<BufferClaims::Release> BufferClaims::giveBack(const Claim& claim) {
    if (!claim.held()) return std::nullopt;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (claim.daemon != m_daemon.lifetime) return std::nullopt;
    auto entry = findEntry(m_entries, claim.monitorId, claim.bufferId);
    if (entry == m_entries.end()) return std::nullopt;
    if (--entry->imports > 0) return std::nullopt;
    const uint64_t newest = entry->newest;
    m_entries.erase(entry);
    return Release{m_daemon.owner, newest};
}

void BufferClaims::daemonReplaced(std::string owner) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_daemon.lifetime;
    m_daemon.owner = std::move(owner);
    m_entries.clear();
}

bool BufferClaims::daemonNamed(std::string owner) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_daemon.lifetime != 0) return false;
    m_daemon.lifetime = 1;
    m_daemon.owner = std::move(owner);
    return true;
}

BufferClaims::Daemon BufferClaims::currentDaemon() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_daemon;
}

void ClaimSet::add(const BufferClaims::Claim& claim) {
    if (!claim.held()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_claims.push_back(claim);
}

bool ClaimSet::remove(const BufferClaims::Claim& claim) {
    if (!claim.held()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto held = std::find(m_claims.begin(), m_claims.end(), claim);
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
