#pragma once

#include "table/cs.hpp"
#include "table/pit.hpp"
#include "table/fib.hpp"
#include "table/strategy-choice.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/time.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

namespace nfd {

namespace face {
class Face; // Correct forward declaration in the correct namespace
}

namespace table {

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

public:
  // These members must be public for boost::multi_index::member
  Name m_prefix;
  face::Face& m_face;
  time::steady_clock::time_point m_expiry;
  uint32_t m_newFaceSeq;
  uint64_t m_floodId;
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
   * @brief Erases all entries whose nexthop is the specified face.
   */
  void
  erase(const face::Face& face);
  
  /**
   * @brief Removes all expired entries from the TFIB.
   */
  void
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
