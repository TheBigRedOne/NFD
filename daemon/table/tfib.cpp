#include "tfib.hpp"
#include "daemon/face/face.hpp"

#include <ndn-cxx/lp/tags.hpp>

#include <algorithm>

namespace nfd {
namespace table {

static constexpr time::milliseconds TFIB_ENTRY_LIFETIME = 5000_ms;

TfibEntry::TfibEntry(const Name& prefix, Face& face,
                     uint32_t newFaceSeq, uint64_t floodId)
  : m_prefix(prefix)
  , m_face(face)
  , m_expiry(time::steady_clock::now() + TFIB_ENTRY_LIFETIME)
  , m_newFaceSeq(newFaceSeq)
  , m_floodId(floodId)
  , m_lastUsed(time::steady_clock::now())
  , m_fibAvailableSince(std::nullopt)
  , m_state(TfibEntryState::Active)
  , m_hardDeadline(std::nullopt)
  , m_routeReady(false)
{
}

TfibEntry*
Tfib::findLongestPrefixMatch(const Name& name)
{
  if (m_table.empty()) {
    return nullptr;
  }

  Name prefix = name;
  while (true) {
    auto it = m_table.get<Prefix_>().find(prefix);
    if (it != m_table.get<Prefix_>().end()) {
      return it->get();
    }
    if (prefix.empty()) {
      break;
    }
    prefix = prefix.getPrefix(-1);
  }
  return nullptr;
}

void
Tfib::insert(const Name& prefix, Face& face, uint32_t seq, uint64_t floodId)
{
  auto& prefix_idx = m_table.get<Prefix_>();
  auto it = prefix_idx.find(prefix);

  if (it != prefix_idx.end()) {
    // Entry exists, check sequence number
    if (seq > (*it)->getNewFaceSeq()) {
      // Update existing entry if new sequence is greater
      prefix_idx.replace(it, std::make_shared<TfibEntry>(prefix, face, seq, floodId));
    }
  }
  else {
    // Insert new entry
    m_table.insert(std::make_shared<TfibEntry>(prefix, face, seq, floodId));
  }
}

TfibUseDecision
Tfib::onUse(const Name& prefix, bool fibAgrees, bool fibUsable,
            time::milliseconds idleTtl, time::milliseconds fibStableWindow,
            time::milliseconds standbyMaxLifetime)
{
  auto& prefix_idx = m_table.get<Prefix_>();
  auto it = prefix_idx.find(prefix);
  if (it == prefix_idx.end()) {
    return TfibUseDecision::NotFound;
  }

  auto now = time::steady_clock::now();

  // Expiry never exceeds the bound armed on the first Standby transition.
  auto boundedExpiry = [&] (const TfibEntry& entry, time::steady_clock::time_point t) {
    return entry.m_hardDeadline ? std::min(t, *entry.m_hardDeadline) : t;
  };

  if ((*it)->m_state == TfibEntryState::Active) {
    bool toStandby = false;
    auto fibSince = (*it)->m_fibAvailableSince;
    if (fibAgrees) {
      if (!fibSince) {
        fibSince = now;
      }
      else if (now - *fibSince >= fibStableWindow) {
        toStandby = true;
      }
    }
    else {
      fibSince.reset();
    }

    if (!toStandby) {
      prefix_idx.modify(it, [&] (std::shared_ptr<TfibEntry>& entry) {
        entry->m_lastUsed = now;
        entry->m_expiry = boundedExpiry(*entry, now + idleTtl);
        entry->m_fibAvailableSince = fibSince;
      });
      return TfibUseDecision::Use;
    }

    prefix_idx.modify(it, [&] (std::shared_ptr<TfibEntry>& entry) {
      entry->m_state = TfibEntryState::Standby;
      entry->m_fibAvailableSince = fibSince;
      if (!entry->m_hardDeadline) {
        entry->m_hardDeadline = now + standbyMaxLifetime;
      }
      entry->m_expiry = *entry->m_hardDeadline;
    });
    // Report the transition on its own, so that handing back strategy control is
    // observable even when the entry is releasable straight away. Release is then
    // decided on the next Interest.
    return TfibUseDecision::Standby;
  }

  // Standby: the FIB is expected to carry the traffic. Fall back only if it cannot,
  // and release the entry once routing has confirmed the new location.
  if (!fibUsable) {
    prefix_idx.modify(it, [&] (std::shared_ptr<TfibEntry>& entry) {
      entry->m_state = TfibEntryState::Active;
      entry->m_fibAvailableSince.reset();
      entry->m_lastUsed = now;
      entry->m_expiry = boundedExpiry(*entry, now + idleTtl);
    });
    return TfibUseDecision::Use;
  }

  if ((*it)->m_routeReady) {
    prefix_idx.erase(it);
    return TfibUseDecision::Released;
  }

  return TfibUseDecision::Standby;
}

bool
Tfib::markRouteReady(const Name& name)
{
  auto* entry = findLongestPrefixMatch(name);
  if (entry == nullptr) {
    return false;
  }
  entry->m_routeReady = true;
  return true;
}

void
Tfib::erase(const Face& face)
{
  for (auto it = m_table.begin(); it != m_table.end(); ) {
    if ((*it)->getFace().getId() == face.getId()) {
      it = m_table.erase(it);
    }
    else {
      ++it;
    }
  }
}

std::vector<Name>
Tfib::cleanup()
{
  auto& expiry_idx = m_table.get<Expiry_>();
  auto now = time::steady_clock::now();

  auto range_end = expiry_idx.upper_bound(now);
  std::vector<Name> erased;
  for (auto it = expiry_idx.begin(); it != range_end; ++it) {
    erased.push_back((*it)->m_prefix);
  }
  expiry_idx.erase(expiry_idx.begin(), range_end);
  return erased;
}

} // namespace table
} // namespace nfd
