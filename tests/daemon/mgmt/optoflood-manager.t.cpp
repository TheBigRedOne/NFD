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

#include "mgmt/optoflood-manager.hpp"

#include "manager-common-fixture.hpp"
#include "tests/daemon/face/dummy-face.hpp"

#include <ndn-cxx/mgmt/nfd/control-parameters.hpp>
#include <ndn-cxx/mgmt/nfd/fib-entry.hpp>

namespace nfd::tests {

class OptoFloodManagerFixture : public ManagerFixtureWithAuthenticator
{
public:
  OptoFloodManagerFixture()
    : m_manager(m_forwarder, m_dispatcher)
  {
    setTopPrefix();
  }

  shared_ptr<DummyFace>
  addNonLocalFace()
  {
    auto face = make_shared<DummyFace>();
    m_faceTable.add(face);
    advanceClocks(1_ms);
    m_responses.clear();
    return face;
  }

  std::vector<ndn::nfd::FibEntry>
  queryServiceBranches(const Name& interestName)
  {
    m_responses.clear();
    receiveInterest(Interest(interestName).setCanBePrefix(true));
    Block content = concatenateResponses();
    content.parse();
    std::vector<ndn::nfd::FibEntry> entries;
    for (const auto& element : content.elements()) {
      entries.emplace_back(element);
    }
    return entries;
  }

protected:
  OptoFloodManager m_manager;
};

BOOST_AUTO_TEST_SUITE(Mgmt)
BOOST_FIXTURE_TEST_SUITE(TestOptoFloodManager, OptoFloodManagerFixture)

BOOST_AUTO_TEST_CASE(QueryEmpty)
{
  auto entries = queryServiceBranches("/localhost/nfd/optoflood/service-branches");
  BOOST_CHECK(entries.empty());
}

BOOST_AUTO_TEST_CASE(QueryExactOneFace)
{
  auto face = addNonLocalFace();
  m_forwarder.observeServiceBranch("/LiveStream", *face);
  advanceClocks(1_ms);

  auto entries = queryServiceBranches("/localhost/nfd/optoflood/service-branches/LiveStream");
  BOOST_REQUIRE_EQUAL(entries.size(), 1);
  BOOST_CHECK_EQUAL(entries.front().getPrefix(), Name("/LiveStream"));
  BOOST_REQUIRE_EQUAL(entries.front().getNextHopRecords().size(), 1);
  BOOST_CHECK_EQUAL(entries.front().getNextHopRecords().front().getFaceId(), face->getId());
  BOOST_CHECK_EQUAL(entries.front().getNextHopRecords().front().getCost(), 0);
}

BOOST_AUTO_TEST_CASE(QueryExactMultipleFaces)
{
  auto face1 = addNonLocalFace();
  auto face2 = addNonLocalFace();
  m_forwarder.observeServiceBranch("/LiveStream", *face1);
  m_forwarder.observeServiceBranch("/LiveStream", *face2);
  advanceClocks(1_ms);

  auto entries = queryServiceBranches("/localhost/nfd/optoflood/service-branches/LiveStream");
  BOOST_REQUIRE_EQUAL(entries.size(), 1);
  BOOST_CHECK_EQUAL(entries.front().getNextHopRecords().size(), 2);
}

BOOST_AUTO_TEST_CASE(QueryListAll)
{
  auto face1 = addNonLocalFace();
  auto face2 = addNonLocalFace();
  m_forwarder.observeServiceBranch("/A", *face1);
  m_forwarder.observeServiceBranch("/B", *face2);
  advanceClocks(1_ms);

  auto entries = queryServiceBranches("/localhost/nfd/optoflood/service-branches");
  BOOST_REQUIRE_EQUAL(entries.size(), 2);
}

BOOST_AUTO_TEST_CASE(DuplicateAddEmitsOneNotification)
{
  auto face = addNonLocalFace();
  m_responses.clear();
  m_forwarder.observeServiceBranch("/LiveStream", *face);
  advanceClocks(1_ms, 10);
  BOOST_REQUIRE_EQUAL(m_responses.size(), 1);
  BOOST_CHECK(Name("/localhost/nfd/optoflood/service-branch-events").isPrefixOf(m_responses.front().getName()));

  ndn::nfd::ControlParameters params(m_responses.front().getContent().blockFromValue());
  BOOST_CHECK_EQUAL(params.getName(), Name("/LiveStream"));
  BOOST_CHECK_EQUAL(params.getFaceId(), face->getId());

  m_responses.clear();
  m_forwarder.observeServiceBranch("/LiveStream", *face);
  advanceClocks(1_ms, 10);
  BOOST_CHECK(m_responses.empty());
}

BOOST_AUTO_TEST_CASE(PrefixEraseReflectedInDataset)
{
  auto face = addNonLocalFace();
  m_forwarder.observeServiceBranch("/LiveStream", *face);
  advanceClocks(1_ms);
  m_forwarder.getServiceBranchTable().erase("/LiveStream");

  auto entries = queryServiceBranches("/localhost/nfd/optoflood/service-branches/LiveStream");
  BOOST_CHECK(entries.empty());
}

BOOST_AUTO_TEST_CASE(FaceRemovalReflectedInDataset)
{
  auto face1 = addNonLocalFace();
  auto face2 = addNonLocalFace();
  m_forwarder.observeServiceBranch("/LiveStream", *face1);
  m_forwarder.observeServiceBranch("/LiveStream", *face2);
  face1->close();
  advanceClocks(1_ms);

  auto entries = queryServiceBranches("/localhost/nfd/optoflood/service-branches/LiveStream");
  BOOST_REQUIRE_EQUAL(entries.size(), 1);
  BOOST_REQUIRE_EQUAL(entries.front().getNextHopRecords().size(), 1);
  BOOST_CHECK_EQUAL(entries.front().getNextHopRecords().front().getFaceId(), face2->getId());
}

BOOST_AUTO_TEST_CASE(ExcludedObservationEmitsNoNotification)
{
  auto local = make_shared<DummyFace>("dummy://", "dummy://", ndn::nfd::FACE_SCOPE_LOCAL);
  m_faceTable.add(local);
  advanceClocks(1_ms);
  m_responses.clear();

  m_forwarder.observeServiceBranch("/LiveStream", *local);
  advanceClocks(1_ms, 10);
  BOOST_CHECK(m_responses.empty());
  BOOST_CHECK(m_forwarder.getServiceBranchTable().find("/LiveStream") == nullptr);
}

BOOST_AUTO_TEST_SUITE_END() // TestOptoFloodManager
BOOST_AUTO_TEST_SUITE_END() // Mgmt

} // namespace nfd::tests
