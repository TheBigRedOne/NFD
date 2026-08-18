#include "tfib.hpp"
#include "daemon/face/face.hpp"

#include <ndn-cxx/lp/tags.hpp>

#include <algorithm>

namespace nfd {
namespace table {

TfibEntry::TfibEntry(const Name& prefix, Face& face,
                     uint32_t newFaceSeq, uint64_t floodId)
  : m_prefix(prefix)
  , m_face(face)
  , m_expiry()
  , m_newFaceSeq(newFaceSeq)
  , m_floodId(floodId)
  , m_lastUsed()
  , m_fibAvailableSince(std::nullopt)
  , m_state(TfibEntryState::Active)
  , m_hardDeadline()
{
  const auto now = time::steady_clock::now();
  m_lastUsed = now;
  m_hardDeadline = now + TFIB_STANDBY_MAX_LIFETIME;
  m_expiry = std::min(now + TFIB_ENTRY_LIFETIME, m_hardDeadline);
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
Tfib::insert(const Name& prefix, face::Face& face, uint32_t seq, uint64_t floodId)
{
  auto& prefix_idx = m_table.get<Prefix_>();
  auto it = prefix_idx.find(prefix);

  if (it != prefix_idx.end()) {
    if (seq > (*it)->getNewFaceSeq()) {
      prefix_idx.replace(it, std::make_shared<TfibEntry>(prefix, face, seq, floodId));
    }
    return;
  }

  m_table.insert(std::make_shared<TfibEntry>(prefix, face, seq, floodId));
}

TfibUseDecision
Tfib::onUse(const Name& prefix, bool fibAgrees, bool fibUsable,
            time::milliseconds idleTtl, time::milliseconds fibStableWindow)
{
  auto& prefix_idx = m_table.get<Prefix_>();
  auto it = prefix_idx.find(prefix);
  if (it == prefix_idx.end()) {
    return TfibUseDecision::NotFound;
  }

  auto now = time::steady_clock::now();

  auto boundedExpiry = [&] (const TfibEntry& entry, time::steady_clock::time_point t) {
    return std::min(t, entry.m_hardDeadline);
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
      entry->m_expiry = entry->m_hardDeadline;
    });
    return TfibUseDecision::Standby;
  }

  // Standby: ordinary FIB is primary. Fall back when it cannot forward equivalently.
  // Local FIB agreement never shortens hardDeadline and never erases this generation.
  if (!fibUsable || !fibAgrees) {
    prefix_idx.modify(it, [&] (std::shared_ptr<TfibEntry>& entry) {
      entry->m_state = TfibEntryState::Active;
      entry->m_fibAvailableSince.reset();
      entry->m_lastUsed = now;
      entry->m_expiry = boundedExpiry(*entry, now + idleTtl);
    });
    return TfibUseDecision::Use;
  }

  return TfibUseDecision::Standby;
}

TfibProofDecision
Tfib::acceptNewPathProof(const Name& name, uint64_t)
{
  return findLongestPrefixMatch(name) == nullptr
         ? TfibProofDecision::NotFound
         : TfibProofDecision::Accepted;
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
