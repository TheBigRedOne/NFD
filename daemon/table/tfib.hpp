#pragma once

#include "table/cs.hpp"
#include "table/pit.hpp"
#include "table/fib.hpp"
#include "table/strategy-choice.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/time.hpp>

#include <cstdint>
#include <optional>
#include <ostream>
#include <vector>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

namespace nfd {

namespace face {
class Face; // Correct forward declaration in the correct namespace
}

namespace table {

/** Initial idle expiry for a newly created Active generation. */
inline constexpr time::milliseconds TFIB_ENTRY_LIFETIME = time::milliseconds(5000);

/**
 * Absolute generation lifetime from insert. Not configurable.
 * Armed on every new TfibEntry; same-sequence insert does not refresh it.
 */
inline constexpr time::milliseconds TFIB_STANDBY_MAX_LIFETIME = time::milliseconds(120000);

/**
 * @brief Lifecycle state of a TFIB entry.
 *
 * An Active entry is on the forwarding fast path and therefore bypasses the
 * forwarding strategy. Once the exact ordinary FIB entry for the TFIB prefix
 * prefers the same face continuously for the forwarding-plane stability window,
 * the entry moves to Standby: strategy control returns to the normal pipeline,
 * but the entry is kept as a fallback until this generation's hardDeadline.
 * Deletion is table erase, not a third runtime state.
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
   *
   * Arms hardDeadline immediately: now + TFIB_STANDBY_MAX_LIFETIME.
   * Initial expiry is min(now + TFIB_ENTRY_LIFETIME, hardDeadline).
   */
  TfibEntry(const Name& prefix, face::Face& face,
            uint32_t newFaceSeq, uint64_t floodId);

  const Name&
  getPrefix() const { return m_prefix; }

  face::Face&
  getFace() const { return m_face; }

  const time::steady_clock::time_point&
  getExpiry() const { return m_expiry; }

  const time::steady_clock::time_point&
  getHardDeadline() const { return m_hardDeadline; }

  uint32_t
  getNewFaceSeq() const { return m_newFaceSeq; }

  uint64_t
  getFloodId() const { return m_floodId; }

  TfibEntryState
  getState() const { return m_state; }

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
   * Absolute upper bound on this generation's lifetime, armed at construction.
   * Survives Active <-> Standby so local FIB agreement cannot destroy fallback
   * state before this deadline.
   */
  time::steady_clock::time_point m_hardDeadline;
};

/**
 * @brief Decision for TFIB usage on an Interest.
 */
enum class TfibUseDecision
{
  NotFound,  ///< no entry covers the name
  Use,       ///< entry is Active: forward on the TFIB face
  Standby    ///< entry is retained but idle: use the FIB and the strategy
};

/**
 * @brief Result of observing a localhost new-path-calculated Interest.
 *
 * Telemetry only. Proofs have no TFIB lifecycle authority.
 */
enum class TfibProofDecision
{
  NotFound,   ///< no TFIB entry covers the name
  Rejected,   ///< unused; proofs do not affect TFIB generations
  Accepted    ///< a covering TFIB entry exists; no state is mutated
};

inline std::ostream&
operator<<(std::ostream& os, TfibEntryState state)
{
  switch (state) {
    case TfibEntryState::Active: return os << "Active";
    case TfibEntryState::Standby: return os << "Standby";
  }
  return os << "TfibEntryState(" << static_cast<int>(state) << ")";
}

inline std::ostream&
operator<<(std::ostream& os, TfibUseDecision decision)
{
  switch (decision) {
    case TfibUseDecision::NotFound: return os << "NotFound";
    case TfibUseDecision::Use: return os << "Use";
    case TfibUseDecision::Standby: return os << "Standby";
  }
  return os << "TfibUseDecision(" << static_cast<int>(decision) << ")";
}

inline std::ostream&
operator<<(std::ostream& os, TfibProofDecision decision)
{
  switch (decision) {
    case TfibProofDecision::NotFound: return os << "NotFound";
    case TfibProofDecision::Rejected: return os << "Rejected";
    case TfibProofDecision::Accepted: return os << "Accepted";
  }
  return os << "TfibProofDecision(" << static_cast<int>(decision) << ")";
}

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
   * @brief Inserts a TFIB generation.
   *
   * A new prefix or a strictly higher NewFaceSeq replaces the current generation
   * with a new Active entry and a new hardDeadline. The same sequence is a
   * complete no-op (face, seq, FloodId, state, expiry, and hardDeadline unchanged).
   */
  void
  insert(const Name& prefix, face::Face& face, uint32_t seq, uint64_t floodId);

  /**
   * @brief Advances the lifecycle of a TFIB entry on Interest arrival.
   *
   * @param prefix          key of the entry, as returned by findLongestPrefixMatch
   * @param fibAgrees       exact ordinary FIB currently prefers the TFIB face
   * @param fibUsable       an exact ordinary FIB entry exists and has nexthops
   * @param idleTtl         expiry refresh applied while the entry is Active
   * @param fibStableWindow how long @p fibAgrees must hold before Standby
   *
   * Active stays Active until @p fibAgrees has held for @p fibStableWindow, then
   * moves to Standby and pins expiry to hardDeadline. Standby returns to Active
   * when @p fibUsable is false or @p fibAgrees is false, retaining the original
   * hardDeadline. Proof state is not consulted.
   */
  TfibUseDecision
  onUse(const Name& prefix, bool fibAgrees, bool fibUsable,
        time::milliseconds idleTtl, time::milliseconds fibStableWindow);

  /**
   * @brief Observes a new-path-calculated Interest from the local routing protocol.
   *
   * Telemetry only. Does not latch proofs, change Active/Standby, erase entries,
   * or modify expiry, hardDeadline, or generation authority.
   *
   * @param name   mobile prefix carried in the localhost Interest
   * @param serial monotonic calculation serial from NLSR (ignored)
   */
  TfibProofDecision
  acceptNewPathProof(const Name& name, uint64_t serial);

  /**
   * @brief Erases all entries whose nexthop is the specified face.
   */
  void
  erase(const face::Face& face);

  /**
   * @brief Removes all expired entries from the TFIB.
   *
   * @return prefixes of the removed entries.
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
};

} // namespace table
} // namespace nfd
