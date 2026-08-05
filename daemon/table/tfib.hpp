#pragma once

#include "table/cs.hpp"
#include "table/pit.hpp"
#include "table/fib.hpp"
#include "table/strategy-choice.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/time.hpp>

#include <optional>
#include <vector>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

namespace nfd {

namespace face {
class Face; // Correct forward declaration in the correct namespace
}

namespace table {

/**
 * @brief Lifecycle state of a TFIB entry.
 *
 * An Active entry is on the forwarding fast path and therefore bypasses the
 * forwarding strategy. Once the FIB can forward equivalently, the entry moves to
 * Standby: strategy control returns to the normal pipeline, but the entry is kept
 * as a fallback in case the routing plane later loses the prefix again.
 */
enum class TfibEntryState
{
  Active,
  Standby
};

/**
 * @brief An entry in the Temporary Forwarding Information Base (TFIB).
 */
class TfibEntry
{
public:
  /**
   * @brief Constructs a TFIB entry.
   */
  TfibEntry(const Name& prefix, face::Face& face,
            uint32_t newFaceSeq, uint64_t floodId);

  const Name&
  getPrefix() const { return m_prefix; }

  face::Face&
  getFace() const { return m_face; }

  const time::steady_clock::time_point&
  getExpiry() const { return m_expiry; }

  uint32_t
  getNewFaceSeq() const { return m_newFaceSeq; }

  uint64_t
  getFloodId() const { return m_floodId; }

  TfibEntryState
  getState() const { return m_state; }

  bool
  isRouteReady() const { return m_routeReady; }

public:
  // These members must be public for boost::multi_index::member
  Name m_prefix;
  face::Face& m_face;
  time::steady_clock::time_point m_expiry;
  uint32_t m_newFaceSeq;
  uint64_t m_floodId;
  time::steady_clock::time_point m_lastUsed;
  std::optional<time::steady_clock::time_point> m_fibAvailableSince;
  TfibEntryState m_state;
  /**
   * Absolute upper bound on the entry's lifetime, armed when the entry first
   * enters Standby. It survives a later fallback to Active so that a persistently
   * broken routing plane cannot keep the entry alive indefinitely.
   */
  std::optional<time::steady_clock::time_point> m_hardDeadline;
  /**
   * Set when the local routing protocol reports that it has absorbed the
   * topology change for this prefix. Cleared implicitly on every insert, because
   * a new mobility event constructs a new entry.
   */
  bool m_routeReady;
};

/**
 * @brief Decision for TFIB usage on an Interest.
 */
enum class TfibUseDecision
{
  NotFound,  ///< no entry covers the name
  Use,       ///< entry is Active: forward on the TFIB face
  Standby,   ///< entry is retained but idle: use the FIB and the strategy
  Released   ///< entry was removed after routing confirmed the new location
};

/**
 * @brief The Temporary Forwarding Information Base (TFIB).
 *
 * This table stores transient forwarding entries created by the OptoFlood mechanism.
 */
class Tfib
{
public:
  /**
   * @brief Finds the longest-prefix-match entry for a name.
   */
  TfibEntry*
  findLongestPrefixMatch(const Name& name);
  
  /**
   * @brief Inserts or updates a TFIB entry.
   *
   * If an entry for the same prefix exists, it is updated only if the
   * newFaceSeq is greater than the existing one.
   */
  void
  insert(const Name& prefix, face::Face& face, uint32_t seq, uint64_t floodId);

  /**
   * @brief Advances the lifecycle of a TFIB entry on Interest arrival.
   *
   * @param prefix             key of the entry, as returned by findLongestPrefixMatch
   * @param fibAgrees          the FIB already forwards this prefix on the TFIB face
   * @param fibUsable          the FIB has any usable nexthop for this prefix
   * @param idleTtl            expiry refresh applied while the entry is Active
   * @param fibStableWindow    how long @p fibAgrees must hold before Standby
   * @param standbyMaxLifetime upper bound armed when the entry first enters Standby
   *
   * Active stays Active until @p fibAgrees has held for @p fibStableWindow, then
   * moves to Standby. Standby returns to Active whenever @p fibUsable is false, and
   * is released once the routing protocol has reported readiness while the FIB is
   * usable.
   */
  TfibUseDecision
  onUse(const Name& prefix, bool fibAgrees, bool fibUsable,
        time::milliseconds idleTtl, time::milliseconds fibStableWindow,
        time::milliseconds standbyMaxLifetime);

  /**
   * @brief Records that the routing protocol has absorbed the topology change.
   *
   * @return true if an entry covering @p name was marked.
   */
  bool
  markRouteReady(const Name& name);

  /**
   * @brief Erases all entries whose nexthop is the specified face.
   */
  void
  erase(const face::Face& face);
  
  /**
   * @brief Removes all expired entries from the TFIB.
   *
   * @return prefixes of the removed entries. An entry reclaimed here was never
   *         confirmed by the routing protocol, so reporting it separates release on
   *         readiness from release on the standby bound.
   */
  std::vector<Name>
  cleanup();

private:
  struct Prefix_ {};
  struct Expiry_ {};

  using Container = boost::multi_index::multi_index_container<
    std::shared_ptr<TfibEntry>,
    boost::multi_index::indexed_by<
      boost::multi_index::ordered_unique<
        boost::multi_index::tag<Prefix_>,
        boost::multi_index::member<TfibEntry, const Name, &TfibEntry::m_prefix>
        // Default std::less<Name> is sufficient as ndn::Name is comparable
      >,
      boost::multi_index::ordered_non_unique<
        boost::multi_index::tag<Expiry_>,
        boost::multi_index::member<TfibEntry, time::steady_clock::time_point, &TfibEntry::m_expiry>
      >
    >
  >;

  Container m_table;
};

} // namespace table
} // namespace nfd
