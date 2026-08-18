/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2026,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "table/tfib.hpp"
#include "fw/face-table.hpp"

#include "tests/test-common.hpp"
#include "tests/daemon/global-io-fixture.hpp"
#include "tests/daemon/face/dummy-face.hpp"

namespace nfd::tests {

using nfd::table::Tfib;
using nfd::table::TfibEntry;
using nfd::table::TfibEntryState;
using nfd::table::TfibProofDecision;
using nfd::table::TfibUseDecision;
using nfd::table::TFIB_ENTRY_LIFETIME;
using nfd::table::TFIB_STANDBY_MAX_LIFETIME;

namespace {

constexpr time::milliseconds IDLE_TTL = 5000_ms;
constexpr time::milliseconds FIB_STABLE_WINDOW = 5000_ms;

TfibUseDecision
onUse(Tfib& tfib, const Name& prefix, bool fibAgrees, bool fibUsable)
{
  return tfib.onUse(prefix, fibAgrees, fibUsable, IDLE_TTL, FIB_STABLE_WINDOW);
}

} // namespace

BOOST_AUTO_TEST_SUITE(Table)
BOOST_FIXTURE_TEST_SUITE(TestTfib, GlobalIoTimeFixture)

BOOST_AUTO_TEST_CASE(InsertArmsHardDeadline)
{
  auto face = make_shared<DummyFace>();
  Tfib tfib;
  const auto now = time::steady_clock::now();
  tfib.insert("/A", *face, 1, 11);

  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Active);
  BOOST_CHECK_EQUAL(&entry->getFace(), face.get());
  BOOST_CHECK_EQUAL(entry->getNewFaceSeq(), 1);
  BOOST_CHECK_EQUAL(entry->getFloodId(), 11);
  BOOST_CHECK_EQUAL(entry->getHardDeadline(), now + TFIB_STANDBY_MAX_LIFETIME);
  BOOST_CHECK(entry->getExpiry() <= entry->getHardDeadline());
  BOOST_CHECK_EQUAL(entry->getExpiry(), now + TFIB_ENTRY_LIFETIME);
}

BOOST_AUTO_TEST_CASE(SameSeqIsCompleteNoOp)
{
  auto face1 = make_shared<DummyFace>();
  auto face2 = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face1, 1, 11);
  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);

  const auto expiry = entry->getExpiry();
  const auto deadline = entry->getHardDeadline();
  const auto state = entry->getState();

  this->advanceClocks(1_ms, 200_ms);
  tfib.insert("/A", *face2, 1, 99);

  BOOST_REQUIRE_EQUAL(tfib.findLongestPrefixMatch("/A"), entry);
  BOOST_CHECK_EQUAL(&entry->getFace(), face1.get());
  BOOST_CHECK_EQUAL(entry->getNewFaceSeq(), 1);
  BOOST_CHECK_EQUAL(entry->getFloodId(), 11);
  BOOST_CHECK_EQUAL(entry->getState(), state);
  BOOST_CHECK_EQUAL(entry->getExpiry(), expiry);
  BOOST_CHECK_EQUAL(entry->getHardDeadline(), deadline);
}

BOOST_AUTO_TEST_CASE(HigherSeqReplacesGeneration)
{
  auto face1 = make_shared<DummyFace>();
  auto face2 = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face1, 1, 11);
  auto* g1 = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(g1 != nullptr);
  const auto g1Deadline = g1->getHardDeadline();

  this->advanceClocks(1_s, 1_s);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", false, false), TfibUseDecision::Use);

  tfib.insert("/A", *face2, 2, 22);
  auto* g2 = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(g2 != nullptr);
  BOOST_CHECK_EQUAL(g2->getState(), TfibEntryState::Active);
  BOOST_CHECK_EQUAL(&g2->getFace(), face2.get());
  BOOST_CHECK_EQUAL(g2->getNewFaceSeq(), 2);
  BOOST_CHECK_EQUAL(g2->getFloodId(), 22);
  BOOST_CHECK(g2->getHardDeadline() > g1Deadline);

  const auto g2Deadline = g2->getHardDeadline();
  for (int i = 0; i < 29; ++i) {
    this->advanceClocks(1_s, 4_s);
    BOOST_CHECK_EQUAL(onUse(tfib, "/A", false, false), TfibUseDecision::Use);
  }

  this->advanceClocks(1_s, 3_s);
  BOOST_CHECK(tfib.cleanup().empty());
  BOOST_REQUIRE(tfib.findLongestPrefixMatch("/A") != nullptr);

  this->advanceClocks(1_s, 2_s);
  const auto erased = tfib.cleanup();
  BOOST_REQUIRE_EQUAL(erased.size(), 1);
  BOOST_CHECK_EQUAL(erased.front(), Name("/A"));
  BOOST_CHECK(tfib.findLongestPrefixMatch("/A") == nullptr);
  BOOST_CHECK(time::steady_clock::now() >= g2Deadline);
}

BOOST_AUTO_TEST_CASE(ActiveIdleRefreshCappedByHardDeadline)
{
  auto face = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face, 1, 11);
  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  const auto deadline = entry->getHardDeadline();

  for (int i = 0; i < 29; ++i) {
    this->advanceClocks(1_s, 4_s);
    BOOST_CHECK_EQUAL(onUse(tfib, "/A", false, false), TfibUseDecision::Use);
    entry = tfib.findLongestPrefixMatch("/A");
    BOOST_REQUIRE(entry != nullptr);
    BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Active);
    BOOST_CHECK_EQUAL(entry->getHardDeadline(), deadline);
    BOOST_CHECK(entry->getExpiry() <= deadline);
  }

  this->advanceClocks(1_s, 8_s);
  BOOST_REQUIRE_EQUAL(tfib.cleanup().size(), 1);
  BOOST_CHECK(tfib.findLongestPrefixMatch("/A") == nullptr);
}

BOOST_AUTO_TEST_CASE(ActiveToStandbyPinsExpiryToHardDeadline)
{
  auto face = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face, 1, 11);

  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Use);
  this->advanceClocks(1_ms, FIB_STABLE_WINDOW);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Standby);

  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Standby);
  BOOST_CHECK_EQUAL(entry->getExpiry(), entry->getHardDeadline());
  const auto deadline = entry->getHardDeadline();

  this->advanceClocks(1_s, 6_s);
  BOOST_CHECK(tfib.cleanup().empty());
  entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Standby);
  BOOST_CHECK_EQUAL(entry->getExpiry(), deadline);
}

BOOST_AUTO_TEST_CASE(StandbyReturnsToActiveOnDisagreement)
{
  auto face = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face, 1, 11);

  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Use);
  this->advanceClocks(1_ms, FIB_STABLE_WINDOW);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Standby);

  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  const auto deadline = entry->getHardDeadline();

  BOOST_CHECK_EQUAL(onUse(tfib, "/A", false, false), TfibUseDecision::Use);
  entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Active);
  BOOST_CHECK_EQUAL(entry->getHardDeadline(), deadline);
  BOOST_CHECK(entry->getExpiry() <= deadline);
}

BOOST_AUTO_TEST_CASE(ProofWhileActiveHasNoEffect)
{
  auto face = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face, 1, 11);
  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  const auto expiry = entry->getExpiry();
  const auto deadline = entry->getHardDeadline();

  BOOST_CHECK_EQUAL(tfib.acceptNewPathProof("/A", 7), TfibProofDecision::Accepted);
  BOOST_REQUIRE_EQUAL(tfib.findLongestPrefixMatch("/A"), entry);
  BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Active);
  BOOST_CHECK_EQUAL(entry->getExpiry(), expiry);
  BOOST_CHECK_EQUAL(entry->getHardDeadline(), deadline);
  BOOST_CHECK_EQUAL(entry->getNewFaceSeq(), 1);
}

BOOST_AUTO_TEST_CASE(ProofWhileStandbyHasNoEffect)
{
  auto face = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face, 1, 11);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Use);
  this->advanceClocks(1_ms, FIB_STABLE_WINDOW);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Standby);

  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  const auto expiry = entry->getExpiry();
  const auto deadline = entry->getHardDeadline();

  BOOST_CHECK_EQUAL(tfib.acceptNewPathProof("/A", 9), TfibProofDecision::Accepted);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Standby);
  entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Standby);
  BOOST_CHECK_EQUAL(entry->getExpiry(), expiry);
  BOOST_CHECK_EQUAL(entry->getHardDeadline(), deadline);
}

BOOST_AUTO_TEST_CASE(StaleProofAfterReplacementHasNoEffect)
{
  auto face1 = make_shared<DummyFace>();
  auto face2 = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face1, 1, 11);
  tfib.insert("/A", *face2, 3, 33);
  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  const auto deadline = entry->getHardDeadline();

  BOOST_CHECK_EQUAL(tfib.acceptNewPathProof("/A", 1), TfibProofDecision::Accepted);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Use);
  entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  BOOST_CHECK_EQUAL(entry->getNewFaceSeq(), 3);
  BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Active);
  BOOST_CHECK_EQUAL(entry->getHardDeadline(), deadline);
}

BOOST_AUTO_TEST_CASE(StandbyRemainsUntilHardDeadline)
{
  auto face = make_shared<DummyFace>();
  Tfib tfib;
  tfib.insert("/A", *face, 1, 11);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Use);
  this->advanceClocks(1_ms, FIB_STABLE_WINDOW);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Standby);

  auto* entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  const auto deadline = entry->getHardDeadline();

  BOOST_CHECK_EQUAL(tfib.acceptNewPathProof("/A", 4), TfibProofDecision::Accepted);
  this->advanceClocks(1_s, 6_s);
  BOOST_CHECK_EQUAL(onUse(tfib, "/A", true, true), TfibUseDecision::Standby);
  BOOST_CHECK(tfib.cleanup().empty());
  entry = tfib.findLongestPrefixMatch("/A");
  BOOST_REQUIRE(entry != nullptr);
  BOOST_CHECK_EQUAL(entry->getState(), TfibEntryState::Standby);

  this->advanceClocks(1_s, TFIB_STANDBY_MAX_LIFETIME);
  BOOST_REQUIRE_EQUAL(tfib.cleanup().size(), 1);
  BOOST_CHECK(tfib.findLongestPrefixMatch("/A") == nullptr);
  BOOST_CHECK(time::steady_clock::now() >= deadline);
}

BOOST_AUTO_TEST_CASE(EraseByFace)
{
  FaceTable faceTable;
  auto faceA = make_shared<DummyFace>();
  auto faceB = make_shared<DummyFace>();
  faceTable.add(faceA);
  faceTable.add(faceB);

  Tfib tfib;
  tfib.insert("/A", *faceA, 1, 11);
  tfib.insert("/B", *faceB, 1, 12);
  tfib.erase(*faceA);

  BOOST_CHECK(tfib.findLongestPrefixMatch("/A") == nullptr);
  auto* kept = tfib.findLongestPrefixMatch("/B");
  BOOST_REQUIRE(kept != nullptr);
  BOOST_CHECK_EQUAL(&kept->getFace(), faceB.get());
}

BOOST_AUTO_TEST_SUITE_END() // TestTfib
BOOST_AUTO_TEST_SUITE_END() // Table

} // namespace nfd::tests
