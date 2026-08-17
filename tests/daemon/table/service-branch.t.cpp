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

#include "table/service-branch.hpp"

#include "tests/test-common.hpp"

namespace nfd::tests {

BOOST_AUTO_TEST_SUITE(Table)
BOOST_AUTO_TEST_SUITE(TestServiceBranch)

BOOST_AUTO_TEST_CASE(AddOneFace)
{
  table::ServiceBranchTable table;
  table.add("/A", 1);
  auto* set = table.find("/A");
  BOOST_REQUIRE(set != nullptr);
  BOOST_CHECK_EQUAL(set->size(), 1);
  BOOST_CHECK_EQUAL(set->count(1), 1);
}

BOOST_AUTO_TEST_CASE(DuplicateAddDeduplicated)
{
  table::ServiceBranchTable table;
  table.add("/A", 1);
  table.add("/A", 1);
  auto* set = table.find("/A");
  BOOST_REQUIRE(set != nullptr);
  BOOST_CHECK_EQUAL(set->size(), 1);
}

BOOST_AUTO_TEST_CASE(MultipleFaces)
{
  table::ServiceBranchTable table;
  table.add("/A", 1);
  table.add("/A", 2);
  auto* set = table.find("/A");
  BOOST_REQUIRE(set != nullptr);
  BOOST_CHECK_EQUAL(set->size(), 2);
  BOOST_CHECK_EQUAL(set->count(1), 1);
  BOOST_CHECK_EQUAL(set->count(2), 1);
}

BOOST_AUTO_TEST_CASE(ErasePrefix)
{
  table::ServiceBranchTable table;
  table.add("/A", 1);
  table.add("/B", 2);
  table.erase("/A");
  BOOST_CHECK(table.find("/A") == nullptr);
  auto* setB = table.find("/B");
  BOOST_REQUIRE(setB != nullptr);
  BOOST_CHECK_EQUAL(setB->count(2), 1);
}

BOOST_AUTO_TEST_CASE(EraseFaceRemovesFromRelevantPrefixes)
{
  table::ServiceBranchTable table;
  table.add("/A", 1);
  table.add("/A", 2);
  table.add("/B", 1);
  table.eraseFace(1);
  auto* setA = table.find("/A");
  BOOST_REQUIRE(setA != nullptr);
  BOOST_CHECK_EQUAL(setA->count(1), 0);
  BOOST_CHECK_EQUAL(setA->count(2), 1);
  BOOST_CHECK(table.find("/B") == nullptr);
}

BOOST_AUTO_TEST_CASE(EraseFaceDoesNotAffectOtherFaces)
{
  table::ServiceBranchTable table;
  table.add("/A", 1);
  table.add("/A", 2);
  table.eraseFace(1);
  auto* set = table.find("/A");
  BOOST_REQUIRE(set != nullptr);
  BOOST_CHECK_EQUAL(set->size(), 1);
  BOOST_CHECK_EQUAL(set->count(2), 1);
}

BOOST_AUTO_TEST_CASE(FindMissingPrefix)
{
  table::ServiceBranchTable table;
  BOOST_CHECK(table.find("/A") == nullptr);
}

BOOST_AUTO_TEST_SUITE_END() // TestServiceBranch
BOOST_AUTO_TEST_SUITE_END() // Table

} // namespace nfd::tests
