#include "tfib.hpp"
#include "daemon/face/face.hpp"

#include <ndn-cxx/lp/tags.hpp>

namespace nfd {
namespace table {

static constexpr time::milliseconds TFIB_ENTRY_LIFETIME = 1000_ms;

TfibEntry::TfibEntry(const Name& prefix, Face& face,
                     uint32_t newFaceSeq, uint64_t floodId)
  : m_prefix(prefix)
  , m_face(face)
  , m_expiry(time::steady_clock::now() + TFIB_ENTRY_LIFETIME)
  , m_newFaceSeq(newFaceSeq)
  , m_floodId(floodId)
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

void
Tfib::cleanup()
{
  auto& expiry_idx = m_table.get<Expiry_>();
  auto now = time::steady_clock::now();
  
  auto range_end = expiry_idx.upper_bound(now);
  expiry_idx.erase(expiry_idx.begin(), range_end);
}

} // namespace table
} // namespace nfd
