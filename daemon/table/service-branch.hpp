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

#ifndef NFD_DAEMON_TABLE_SERVICE_BRANCH_HPP
#define NFD_DAEMON_TABLE_SERVICE_BRANCH_HPP

#include "core/common.hpp"
#include "face/face-common.hpp"

#include <map>
#include <set>

namespace nfd::table {

/**
 * \brief Downstream faces that have carried business traffic for a TFIB prefix.
 *
 * Observation only: the table does not affect forwarding. The prefix key is the
 * same routable prefix used by TFIB. A TFIB insert/replace must not clear the
 * set; only TFIB removal or an explicit erase does.
 */
class ServiceBranchTable
{
public:
  void
  add(const Name& prefix, face::FaceId faceId);

  /** \return the FaceId set for \p prefix, or nullptr if the prefix is absent.
   */
  const std::set<face::FaceId>*
  find(const Name& prefix) const;

  void
  erase(const Name& prefix);

  /** \brief Remove \p faceId from every prefix set.
   *
   *  Prefixes whose set becomes empty are erased.
   */
  void
  eraseFace(face::FaceId faceId);

private:
  std::map<Name, std::set<face::FaceId>> m_table;
};

} // namespace nfd::table

#endif // NFD_DAEMON_TABLE_SERVICE_BRANCH_HPP
