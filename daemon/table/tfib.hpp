#pragma once

#include "table/cs.hpp"
#include "table/pit.hpp"
#include "table/fib.hpp"
#include "table/strategy-choice.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/time.hpp>

#include <cstdint>
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
 * forwarding strategy. Once ordinary forwarding prefers the same face as the
 * TFIB entry, the entry moves to Standby: strategy control returns to the normal
 * pipeline, but the entry is kept as a fallback. The entry is released only after
 * NLSR reports a post-new-path calculation proof while forwarding still agrees.
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
  hasNewPathProof() const { return m_newPathProofAccepted; }

  uint64_t
  getMinAcceptProofSerial() const { return m_minAcceptProofSerial; }

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
   * Set when a valid new-path-calculated proof is accepted for this entry.
   * Cleared on every insert (new mobility generation).
   */
  bool m_newPathProofAccepted;
  /**
   * Proof serials at or below this value are rejected as possibly stale relative
   * to proofs observed before this entry was created.
   */
  uint64_t m_minAcceptProofSerial;
};

/**
 * @brief Decision for TFIB usage on an Interest.
 */
enum class TfibUseDecision
{
  NotFound,  ///< no entry covers the name
  Use,       ///< entry is Active: forward on the TFIB face
  Standby,   ///< entry is retained but idle: use the FIB and the strategy
  Released   ///< entry was removed after new-path proof + forwarding agreement
};

/**
 * @brief Result of attempting to accept a new-path-calculated proof.
 */
enum class TfibProofDecision
{
  NotFound,   ///< no TFIB entry covers the name
  Rejected,   ///< serial too old for this entry generation
  Accepted    ///< proof latched on the matching entry
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
   * newFaceSeq is greater than the existing one. A new generation clears any
   * latched new-path proof and raises the minimum acceptable proof serial above
   * proofs already observed on this forwarder.
   */
  void
  insert(const Name& prefix, face::Face& face, uint32_t seq, uint64_t floodId);

  /**
   * @brief Advances the lifecycle of a TFIB entry on Interest arrival.
   *
   * @param prefix             key of the entry, as returned by findLongestPrefixMatch
   * @param fibAgrees          ordinary forwarding currently prefers the TFIB face
   * @param fibUsable          the FIB has any usable nexthop for this prefix
   * @param idleTtl            expiry refresh applied while the entry is Active
   * @param fibStableWindow    how long @p fibAgrees must hold before Standby
   * @param standbyMaxLifetime upper bound armed when the entry first enters Standby
   *
   * Active stays Active until @p fibAgrees has held for @p fibStableWindow, then
   * moves to Standby. Standby returns to Active when @p fibUsable is false or
   * @p fibAgrees is false, and is released once a new-path proof is latched while
   * forwarding still agrees.
   */
  TfibUseDecision
  onUse(const Name& prefix, bool fibAgrees, bool fibUsable,
        time::milliseconds idleTtl, time::milliseconds fibStableWindow,
        time::milliseconds standbyMaxLifetime);

  /**
   * @brief Records a new-path-calculated proof from the local routing protocol.
   *
   * @param name   mobile prefix carried in the localhost Interest
   * @param serial monotonic calculation serial from NLSR
   */
  TfibProofDecision
  acceptNewPathProof(const Name& name, uint64_t serial);

  /**
   * @brief Highest new-path-calculated serial observed by this forwarder.
   */
  uint64_t
  getMaxObservedProofSerial() const { return m_maxObservedProofSerial; }

  /**
   * @brief Erases all entries whose nexthop is the specified face.
   */
  void
  erase(const face::Face& face);

  /**
   * @brief Removes all expired entries from the TFIB.
   *
   * @return prefixes of the removed entries. An entry reclaimed here was never
   *         confirmed by a new-path proof, so reporting it separates release on
   *         proof from release on the standby bound.
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
      >,
      boost::multi_index::ordered_non_unique<
        boost::multi_index::tag<Expiry_>,
        boost::multi_index::member<TfibEntry, time::steady_clock::time_point, &TfibEntry::m_expiry>
      >
    >
  >;

  Container m_table;
  uint64_t m_maxObservedProofSerial = 0;
};

} // namespace table
} // namespace nfd
