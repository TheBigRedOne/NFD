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

#include "optoflood-manager.hpp"

#include "common/logger.hpp"

#include <ndn-cxx/mgmt/nfd/control-parameters.hpp>
#include <ndn-cxx/mgmt/nfd/fib-entry.hpp>

namespace nfd {

NFD_LOG_INIT(OptoFloodManager);

OptoFloodManager::OptoFloodManager(Forwarder& forwarder, Dispatcher& dispatcher)
  : ManagerBase("optoflood", dispatcher)
  , m_forwarder(forwarder)
{
  registerStatusDatasetHandler("service-branches",
    [this] (const auto& prefix, const auto& interest, auto&&... args) {
      listServiceBranches(prefix, interest, std::forward<decltype(args)>(args)...);
    });
  m_postNotification = registerNotificationStream("service-branch-events");
  m_afterServiceBranchAdded = m_forwarder.afterServiceBranchAdded.connect(
    [this] (const Name& prefix, face::FaceId faceId) {
      notifyServiceBranchAdded(prefix, faceId);
    });
}

void
OptoFloodManager::appendBranch(const Name& prefix, const std::set<face::FaceId>& faces,
                               ndn::mgmt::StatusDatasetContext& context)
{
  ndn::nfd::FibEntry entry;
  entry.setPrefix(prefix);
  for (face::FaceId faceId : faces) {
    entry.addNextHopRecord(ndn::nfd::NextHopRecord()
                           .setFaceId(faceId)
                           .setCost(0));
  }
  context.append(entry.wireEncode());
}

void
OptoFloodManager::listServiceBranches(const Name& prefix, const Interest& interest,
                                      ndn::mgmt::StatusDatasetContext& context)
{
  const auto& table = m_forwarder.getServiceBranchTable();
  const Name datasetPrefix = Name(prefix).append("optoflood").append("service-branches");
  Name suffix;
  if (datasetPrefix.isPrefixOf(interest.getName())) {
    suffix = interest.getName().getSubName(datasetPrefix.size());
  }

  if (suffix.empty()) {
    for (const auto& [branchPrefix, faces] : table) {
      appendBranch(branchPrefix, faces, context);
    }
  }
  else if (const auto* faces = table.find(suffix); faces != nullptr) {
    appendBranch(suffix, *faces, context);
  }

  context.end();
}

void
OptoFloodManager::notifyServiceBranchAdded(const Name& prefix, face::FaceId faceId)
{
  NFD_LOG_DEBUG("service-branch-events prefix=" << prefix << " face=" << faceId);
  ndn::nfd::ControlParameters params;
  params.setName(prefix).setFaceId(faceId);
  m_postNotification(params.wireEncode());
}

} // namespace nfd
