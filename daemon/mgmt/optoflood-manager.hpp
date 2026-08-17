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

#ifndef NFD_DAEMON_MGMT_OPTOFLOOD_MANAGER_HPP
#define NFD_DAEMON_MGMT_OPTOFLOOD_MANAGER_HPP

#include "manager-base.hpp"
#include "fw/forwarder.hpp"

#include <set>

namespace nfd {

/**
 * \brief Localhost management of OptoFlood ServiceBranch observation state.
 *
 * Encoding reuses ndn::nfd::FibEntry as transport only; this manager does not
 * read or write the FIB.
 */
class OptoFloodManager final : public ManagerBase
{
public:
  OptoFloodManager(Forwarder& forwarder, Dispatcher& dispatcher);

private:
  void
  listServiceBranches(const Name& prefix, const Interest& interest,
                      ndn::mgmt::StatusDatasetContext& context);

  void
  appendBranch(const Name& prefix, const std::set<face::FaceId>& faces,
               ndn::mgmt::StatusDatasetContext& context);

  void
  notifyServiceBranchAdded(const Name& prefix, face::FaceId faceId);

private:
  Forwarder& m_forwarder;
  ndn::mgmt::PostNotification m_postNotification;
  signal::ScopedConnection m_afterServiceBranchAdded;
};

} // namespace nfd

#endif // NFD_DAEMON_MGMT_OPTOFLOOD_MANAGER_HPP
