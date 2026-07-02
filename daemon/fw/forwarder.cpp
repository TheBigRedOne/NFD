/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2024,  Regents of the University of California,
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

#include "forwarder.hpp"
#include <ndn-cxx/optoflood.hpp>

#include "table/cs-entry.hpp" // Added to provide full definition for CsEntry

#include "algorithm.hpp"
#include "best-route-strategy.hpp"
#include "scope-prefix.hpp"
#include "strategy.hpp"
#include "common/global.hpp"
#include "common/logger.hpp"
#include "table/cleanup.hpp"

#include <ndn-cxx/lp/pit-token.hpp>
#include <ndn-cxx/lp/tags.hpp>
#include <ndn-cxx/mgmt/control-parameters.hpp>
#include <ndn-cxx/mgmt/nfd/command-options.hpp>
#include <ndn-cxx/mgmt/nfd/control-command.hpp>
#include <ndn-cxx/mgmt/control-response.hpp>
#include <boost/endian/conversion.hpp>
#include <cstring>

namespace nfd {

NFD_LOG_INIT(Forwarder);

const std::string CFG_FORWARDER = "forwarder";

namespace {

class FastLsaTriggerCommand : public ndn::nfd::ControlCommand
{
public:
  FastLsaTriggerCommand()
    : ControlCommand("fast-lsa", "trigger")
  {
    m_requestValidator
      .required(ndn::nfd::CONTROL_PARAMETER_NAME)
      .optional(ndn::nfd::CONTROL_PARAMETER_FACE_ID)
      .optional(ndn::nfd::CONTROL_PARAMETER_EXPIRATION_PERIOD)
      .optional(ndn::nfd::CONTROL_PARAMETER_COST);
    m_responseValidator = m_requestValidator;
  }
};

constexpr time::milliseconds TFIB_IDLE_TTL = 5000_ms;
constexpr time::milliseconds TFIB_FIB_STABLE_WINDOW = 5000_ms;

} // namespace

static Name
getDefaultStrategyName()
{
  return fw::BestRouteStrategy::getStrategyName();
}

Forwarder::InterestFloodKey::InterestFloodKey(const Name& n, ndn::Interest::Nonce nonceId)
  : name(n)
  , nonce(nonceId)
{
}

bool
Forwarder::InterestFloodKey::operator==(const InterestFloodKey& other) const
{
  return nonce == other.nonce && name == other.name;
}

size_t
Forwarder::InterestFloodKeyHash::operator()(const InterestFloodKey& key) const noexcept
{
  size_t seed = std::hash<Name>()(key.name);
  uint32_t nonceValue = 0;
  std::memcpy(&nonceValue, key.nonce.data(), sizeof(nonceValue));
  boost::endian::big_to_native_inplace(nonceValue);
  seed ^= std::hash<uint32_t>()(nonceValue) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
  return seed;
}

Forwarder::Forwarder(FaceTable& faceTable)
  : m_faceTable(faceTable)
  , m_unsolicitedDataPolicy(make_unique<fw::DefaultUnsolicitedDataPolicy>())
  , m_fib(m_nameTree)
  , m_pit(m_nameTree)
  , m_measurements(m_nameTree)
  , m_strategyChoice(*this)
{
  scheduleTfibCleanup();
  scheduleFloodRateReset();
  scheduleInterestFloodCleanup();

  m_faceTable.afterAdd.connect([this] (const Face& face) {
    face.afterReceiveInterest.connect(
      [this, &face] (const Interest& interest, const EndpointId& endpointId) {
        this->onIncomingInterest(interest, FaceEndpoint(const_cast<Face&>(face), endpointId));
      });
    face.afterReceiveData.connect(
      [this, &face] (const Data& data, const EndpointId& endpointId) {
        this->onIncomingData(data, FaceEndpoint(const_cast<Face&>(face), endpointId));
      });
    face.afterReceiveNack.connect(
      [this, &face] (const lp::Nack& nack, const EndpointId& endpointId) {
        this->onIncomingNack(nack, FaceEndpoint(const_cast<Face&>(face), endpointId));
      });
    face.onDroppedInterest.connect(
      [this, &face] (const Interest& interest) {
        this->onDroppedInterest(interest, const_cast<Face&>(face));
      });
  });

  m_faceTable.beforeRemove.connect([this] (const Face& face) {
    cleanupOnFaceRemoval(m_nameTree, m_fib, m_pit, face);
  });

  m_fib.afterNewNextHop.connect([this] (const Name& prefix, const fib::NextHop& nextHop) {
    this->onNewNextHop(prefix, nextHop);
  });

  m_strategyChoice.setDefaultStrategy(getDefaultStrategyName());
}
void
Forwarder::scheduleTfibCleanup()
{
  m_tfibCleanupEvent = getScheduler().schedule(TFIB_CLEANUP_INTERVAL, [this] {
    m_tfib.cleanup();
    scheduleTfibCleanup();
  });
}

void
Forwarder::scheduleFloodRateReset()
{
  m_floodRateResetEvent = getScheduler().schedule(FLOOD_RATE_RESET_INTERVAL, [this] {
    m_floodRateMap.clear();
    scheduleFloodRateReset();
  });
}

void
Forwarder::scheduleInterestFloodCleanup()
{
  m_interestFloodCleanupEvent = getScheduler().schedule(INTEREST_FLOOD_CLEANUP_INTERVAL, [this] {
    auto now = time::steady_clock::now();
    for (auto it = m_interestFloodCache.begin(); it != m_interestFloodCache.end();) {
      if (it->second <= now) {
        it = m_interestFloodCache.erase(it);
      }
      else {
        ++it;
      }
    }
    scheduleInterestFloodCleanup();
  });
}

bool
Forwarder::markInterestFlooded(const Interest& interest)
{
  InterestFloodKey key(interest.getName(), interest.getNonce());
  auto expiry = time::steady_clock::now() + INTEREST_FLOOD_CACHE_TTL;
  auto result = m_interestFloodCache.emplace(std::move(key), expiry);
  if (!result.second) {
    return false;
  }
  return true;
}

void
Forwarder::onIncomingInterest(const Interest& interest, const FaceEndpoint& ingress)
{
  interest.setTag(make_shared<lp::IncomingFaceIdTag>(ingress.face.getId()));
  ++m_counters.nInInterests;

  // ensure the received Interest has a Nonce
  auto nonce = interest.getNonce();
  auto hopLimit = interest.getHopLimit();

  // drop if HopLimit zero, decrement otherwise (if present)
  if (hopLimit) {
    NFD_LOG_DEBUG("onIncomingInterest in=" << ingress << " interest=" << interest.getName()
                  << " nonce=" << nonce << " hop-limit=" << static_cast<unsigned>(*hopLimit));
    if (*hopLimit == 0) {
      ++ingress.face.getCounters().nInHopLimitZero;
      // drop
      return;
    }
    const_cast<Interest&>(interest).setHopLimit(*hopLimit - 1);
  }
  else {
    NFD_LOG_DEBUG("onIncomingInterest in=" << ingress << " interest=" << interest.getName()
                  << " nonce=" << nonce);
  }

  // /localhost scope control
  bool isViolatingLocalhost = ingress.face.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(interest.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onIncomingInterest in=" << ingress << " interest=" << interest.getName()
                  << " nonce=" << nonce << " violates /localhost");
    // drop
    return;
  }

  // detect duplicate Nonce with Dead Nonce List
  bool hasDuplicateNonceInDnl = m_deadNonceList.has(interest.getName(), nonce);
  if (hasDuplicateNonceInDnl) {
    // go to Interest loop pipeline
    this->onInterestLoop(interest, ingress);
    return;
  }

  // strip forwarding hint if Interest has reached producer region
  if (!interest.getForwardingHint().empty() &&
      m_networkRegionTable.isInProducerRegion(interest.getForwardingHint())) {
    NFD_LOG_DEBUG("onIncomingInterest in=" << ingress << " interest=" << interest.getName()
                  << " nonce=" << nonce << " reaching-producer-region");
    const_cast<Interest&>(interest).setForwardingHint({});
  }

  // PIT insert
  shared_ptr<pit::Entry> pitEntry = m_pit.insert(interest).first;

  // detect duplicate Nonce in PIT entry
  int dnw = fw::findDuplicateNonce(*pitEntry, nonce, ingress.face);
  bool hasDuplicateNonceInPit = dnw != fw::DUPLICATE_NONCE_NONE;
  if (ingress.face.getLinkType() == ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    // for p2p face: duplicate Nonce from same incoming face is not loop
    hasDuplicateNonceInPit = hasDuplicateNonceInPit && !(dnw & fw::DUPLICATE_NONCE_IN_SAME);
  }
  if (hasDuplicateNonceInPit) {
    // go to Interest loop pipeline
    this->onInterestLoop(interest, ingress);
    return;
  }

  // is pending?
  if (!pitEntry->hasInRecords()) {
    m_cs.find(interest,
              [=] (const Interest& i, const Data& d) { onContentStoreHit(i, ingress, pitEntry, d); },
              [=] (const Interest& i) { onContentStoreMiss(i, ingress, pitEntry); });
  }
  else {
    this->onContentStoreMiss(interest, ingress, pitEntry);
  }
}

void
Forwarder::onInterestLoop(const Interest& interest, const FaceEndpoint& ingress)
{
  // if multi-access or ad hoc face, drop
  if (ingress.face.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    NFD_LOG_DEBUG("onInterestLoop in=" << ingress << " interest=" << interest.getName()
                  << " nonce=" << interest.getNonce() << " drop");
    return;
  }

  NFD_LOG_DEBUG("onInterestLoop in=" << ingress << " interest=" << interest.getName()
                << " nonce=" << interest.getNonce());

  // leave loop handling up to the strategy (e.g., whether to reply with a Nack)
  m_strategyChoice.findEffectiveStrategy(interest.getName()).onInterestLoop(interest, ingress);
}

void
Forwarder::onContentStoreMiss(const Interest& interest, const FaceEndpoint& ingress,
                              const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("onContentStoreMiss interest=" << interest.getName() << " nonce=" << interest.getNonce());
  ++m_counters.nCsMisses;

  // attach HopLimit if configured and not present in Interest
  if (m_config.defaultHopLimit > 0 && !interest.getHopLimit()) {
    const_cast<Interest&>(interest).setHopLimit(m_config.defaultHopLimit);
  }

  // insert in-record
  pitEntry->insertOrUpdateInRecord(ingress.face, interest);

  // set PIT expiry timer to the time that the last PIT in-record expires
  auto lastExpiring = std::max_element(pitEntry->in_begin(), pitEntry->in_end(),
                                       [] (const auto& a, const auto& b) {
                                         return a.getExpiry() < b.getExpiry();
                                       });
  auto lastExpiryFromNow = lastExpiring->getExpiry() - time::steady_clock::now();
  this->setExpiryTimer(pitEntry, time::duration_cast<time::milliseconds>(lastExpiryFromNow));

  // has NextHopFaceId?
  auto nextHopTag = interest.getTag<lp::NextHopFaceIdTag>();
  if (nextHopTag != nullptr) {
    // chosen NextHop face exists?
    Face* nextHopFace = m_faceTable.get(*nextHopTag);
    if (nextHopFace != nullptr) {
      NFD_LOG_DEBUG("onContentStoreMiss interest=" << interest.getName()
                    << " nonce=" << interest.getNonce() << " nexthop-faceid=" << nextHopFace->getId());
      // go to outgoing Interest pipeline
      // scope control is unnecessary, because privileged app explicitly wants to forward
      this->onOutgoingInterest(interest, *nextHopFace, pitEntry);
    }
    return;
  }

  const fib::Entry& fibEntry = m_fib.findLongestPrefixMatch(*pitEntry);

  // TFIB takes precedence
  if (auto* tfibEntry = m_tfib.findLongestPrefixMatch(interest.getName())) {
    Face& tfibFace = tfibEntry->getFace();
    if (tfibFace.getId() == ingress.face.getId()) {
      if (!interest.getHopLimit()) {
        if (markInterestFlooded(interest)) {
          Interest floodInterest = interest;
          floodInterest.refreshNonce();
          markInterestFlooded(floodInterest);
          NFD_LOG_DEBUG("OptoFlood tfib-ingress flood interest=" << floodInterest.getName()
                        << " nonce=" << floodInterest.getNonce());
          handleInterestFlooding(floodInterest, ingress, pitEntry);
        }
        else {
          NFD_LOG_DEBUG("OptoFlood tfib-ingress flood skipped interest=" << interest.getName()
                        << " nonce=" << interest.getNonce() << " reason=already-flooded");
        }
        return;
      }
    }
    bool fibHasP2pNextHop = false;
    for (const auto& nh : fibEntry.getNextHops()) {
      if (nh.getFace().getLinkType() == ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
        fibHasP2pNextHop = true;
        break;
      }
    }
    const bool tfibIsP2p = tfibFace.getLinkType() == ndn::nfd::LINK_TYPE_POINT_TO_POINT;
    const bool fibStableCandidate = tfibIsP2p ? fibEntry.hasNextHop(tfibFace) : fibHasP2pNextHop;
    auto decision = m_tfib.onUse(tfibEntry->getPrefix(), fibStableCandidate, TFIB_IDLE_TTL,
                                 TFIB_FIB_STABLE_WINDOW);
    if (decision == table::TfibUseDecision::Retired) {
      NFD_LOG_DEBUG("OptoFlood tfib-retire prefix=" << tfibEntry->getPrefix()
                    << " reason=fib-stable");
    }
    else if (decision == table::TfibUseDecision::Use) {
      NFD_LOG_DEBUG("OptoFlood tfib-forward interest=" << interest.getName()
                    << " nonce=" << interest.getNonce());
      onOutgoingInterest(interest, tfibEntry->getFace(), pitEntry);
      return;
    }
  }

  // Continue flooding for Interests already carrying HopLimit
  if (interest.getHopLimit()) {
    if (markInterestFlooded(interest)) {
      NFD_LOG_DEBUG("OptoFlood continue flood interest=" << interest.getName()
                    << " nonce=" << interest.getNonce());
      handleInterestFlooding(interest, ingress, pitEntry);
    }
    else {
      NFD_LOG_DEBUG("OptoFlood continue flood skipped interest=" << interest.getName()
                    << " nonce=" << interest.getNonce() << " reason=already-flooded");
    }
    return;
  }

  // dispatch to strategy: after receive Interest
  m_strategyChoice.findEffectiveStrategy(*pitEntry)
    .afterReceiveInterest(interest, FaceEndpoint(ingress.face), pitEntry);
}

void
Forwarder::onContentStoreHit(const Interest& interest, const FaceEndpoint& ingress,
                             const shared_ptr<pit::Entry>& pitEntry, const Data& data)
{
  NFD_LOG_DEBUG("onContentStoreHit interest=" << interest.getName() << " nonce=" << interest.getNonce());
  ++m_counters.nCsHits;

  data.setTag(make_shared<lp::IncomingFaceIdTag>(face::FACEID_CONTENT_STORE));
  data.setTag(interest.getTag<lp::PitToken>());
  // FIXME Should we lookup PIT for other Interests that also match the data?

  pitEntry->isSatisfied = true;
  pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

  // set PIT expiry timer to now
  this->setExpiryTimer(pitEntry, 0_ms);

  // dispatch to strategy: after Content Store hit
  m_strategyChoice.findEffectiveStrategy(*pitEntry).afterContentStoreHit(data, ingress, pitEntry);
}

pit::OutRecord*
Forwarder::onOutgoingInterest(const Interest& interest, Face& egress,
                              const shared_ptr<pit::Entry>& pitEntry)
{
  // drop if HopLimit == 0 but sending on non-local face
  if (interest.getHopLimit() == 0 && egress.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL) {
    NFD_LOG_DEBUG("onOutgoingInterest out=" << egress.getId() << " interest=" << interest.getName()
                  << " nonce=" << interest.getNonce() << " non-local hop-limit=0");
    ++egress.getCounters().nOutHopLimitZero;
    return nullptr;
  }

  NFD_LOG_DEBUG("onOutgoingInterest out=" << egress.getId() << " interest=" << interest.getName()
                << " nonce=" << interest.getNonce());

  // insert out-record
  auto it = pitEntry->insertOrUpdateOutRecord(egress, interest);
  BOOST_ASSERT(it != pitEntry->out_end());

  // send Interest
  egress.sendInterest(interest);
  ++m_counters.nOutInterests;

  return &*it;
}

void
Forwarder::onInterestFinalize(const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("onInterestFinalize interest=" << pitEntry->getName()
                << (pitEntry->isSatisfied ? " satisfied" : " unsatisfied"));

  // Dead Nonce List insert if necessary
  this->insertDeadNonceList(*pitEntry, nullptr);

  // Increment satisfied/unsatisfied Interests counter
  if (pitEntry->isSatisfied) {
    ++m_counters.nSatisfiedInterests;
  }
  else {
    ++m_counters.nUnsatisfiedInterests;
  }

  // PIT delete
  pitEntry->expiryTimer.cancel();
  m_pit.erase(pitEntry.get());
}

void
Forwarder::onIncomingData(const Data& data, const FaceEndpoint& ingress)
{
  data.setTag(make_shared<lp::IncomingFaceIdTag>(ingress.face.getId()));
  ++m_counters.nInData;
  NFD_LOG_DEBUG("onIncomingData in=" << ingress << " data=" << data.getName());

  // /localhost scope control
  bool isViolatingLocalhost = ingress.face.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(data.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onIncomingData in=" << ingress << " data=" << data.getName() << " violates /localhost");
    // drop
    return;
  }

  // PIT match
  pit::DataMatchResult pitMatches = m_pit.findAllDataMatches(data);

  // OptoFlood: Treat as mobility Data if LP.MobilityFlag is present, or FloodId exists (MetaInfo)
  bool isOptoFloodData = (data.getTag<ndn::lp::OptoMobilityFlag>() != nullptr) ||
                         (::ndn::optoflood::getFloodId(data.getMetaInfo()).has_value());
  if (isOptoFloodData) {
    std::unordered_set<uint64_t> suppressedFaces;
    suppressedFaces.insert(ingress.face.getId());
    for (const auto& pitEntry : pitMatches) {
      for (const pit::InRecord& inRecord : pitEntry->getInRecords()) {
        suppressedFaces.insert(inRecord.getFace().getId());
      }
    }
    Data mutableData = data;
    handleOptoFloodData(std::move(mutableData), ingress, suppressedFaces);
    // After flooding, we still let the Data packet proceed through the normal path
    // to satisfy any matching PIT entries.
  }
  
  if (pitMatches.size() == 0) {
    // go to Data unsolicited pipeline
    this->onDataUnsolicited(data, ingress);
    return;
  }

  // CS insert
  m_cs.insert(data);

  // when only one PIT entry is matched, trigger strategy: after receive Data
  if (pitMatches.size() == 1) {
    auto& pitEntry = pitMatches.front();

    NFD_LOG_DEBUG("onIncomingData matching=" << pitEntry->getName());

    // set PIT expiry timer to now
    this->setExpiryTimer(pitEntry, 0_ms);

    // trigger strategy: after receive Data
    m_strategyChoice.findEffectiveStrategy(*pitEntry).afterReceiveData(data, ingress, pitEntry);

    // mark PIT satisfied
    pitEntry->isSatisfied = true;
    pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

    // Dead Nonce List insert if necessary (for out-record of ingress face)
    this->insertDeadNonceList(*pitEntry, &ingress.face);

    // delete PIT entry's out-record
    pitEntry->deleteOutRecord(ingress.face);

    // OptoFlood: at PIT hit, clear LP mobility semantics on the outgoing copies (stop flooding)
    // The clearing is realized by not re-attaching LP tags when sending via GenericLinkService
    // and by removing tags from the in-memory Data before onOutgoingData.
    const_cast<Data&>(data).removeTag<ndn::lp::OptoMobilityFlag>();
    const_cast<Data&>(data).removeTag<ndn::lp::OptoHopLimit>();
  }
  // when more than one PIT entry is matched, trigger strategy: before satisfy Interest,
  // and send Data to all matched out faces
  else {
    std::set<Face*> pendingDownstreams;
    auto now = time::steady_clock::now();

    for (const auto& pitEntry : pitMatches) {
      NFD_LOG_DEBUG("onIncomingData matching=" << pitEntry->getName());

      // remember pending downstreams
      for (const pit::InRecord& inRecord : pitEntry->getInRecords()) {
        if (inRecord.getExpiry() > now) {
          pendingDownstreams.insert(&inRecord.getFace());
        }
      }

      // set PIT expiry timer to now
      this->setExpiryTimer(pitEntry, 0_ms);

      // invoke PIT satisfy callback
      m_strategyChoice.findEffectiveStrategy(*pitEntry).beforeSatisfyInterest(data, ingress, pitEntry);

      // mark PIT satisfied
      pitEntry->isSatisfied = true;
      pitEntry->dataFreshnessPeriod = data.getFreshnessPeriod();

      // Dead Nonce List insert if necessary (for out-record of ingress face)
      this->insertDeadNonceList(*pitEntry, &ingress.face);

      // clear PIT entry's in and out records
      pitEntry->clearInRecords();
      pitEntry->deleteOutRecord(ingress.face);
    }

    // OptoFlood: clear LP mobility semantics before satisfying downstreams (stop flooding)
    const_cast<Data&>(data).removeTag<ndn::lp::OptoMobilityFlag>();
    const_cast<Data&>(data).removeTag<ndn::lp::OptoHopLimit>();

    for (Face* pendingDownstream : pendingDownstreams) {
      if (pendingDownstream->getId() == ingress.face.getId() &&
          pendingDownstream->getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) {
        continue;
      }
      // go to outgoing Data pipeline
      this->onOutgoingData(data, *pendingDownstream);
    }
  }
}

void
Forwarder::onDataUnsolicited(const Data& data, const FaceEndpoint& ingress)
{
  ++m_counters.nUnsolicitedData;

  // accept to cache?
  auto decision = m_unsolicitedDataPolicy->decide(ingress.face, data);
  NFD_LOG_DEBUG("onDataUnsolicited in=" << ingress << " data=" << data.getName()
                << " decision=" << decision);
  if (decision == fw::UnsolicitedDataDecision::CACHE) {
    // CS insert
    m_cs.insert(data, true);
  }
}

bool
Forwarder::onOutgoingData(const Data& data, Face& egress)
{
  if (egress.getId() == face::INVALID_FACEID) {
    NFD_LOG_WARN("onOutgoingData out=(invalid) data=" << data.getName());
    return false;
  }

  // /localhost scope control
  bool isViolatingLocalhost = egress.getScope() == ndn::nfd::FACE_SCOPE_NON_LOCAL &&
                              scope_prefix::LOCALHOST.isPrefixOf(data.getName());
  if (isViolatingLocalhost) {
    NFD_LOG_DEBUG("onOutgoingData out=" << egress.getId() << " data=" << data.getName()
                  << " violates /localhost");
    // drop
    return false;
  }

  NFD_LOG_DEBUG("onOutgoingData out=" << egress.getId() << " data=" << data.getName());

  // send Data
  egress.sendData(data);
  ++m_counters.nOutData;

  return true;
}

void
Forwarder::onIncomingNack(const lp::Nack& nack, const FaceEndpoint& ingress)
{
  nack.setTag(make_shared<lp::IncomingFaceIdTag>(ingress.face.getId()));
  ++m_counters.nInNacks;

  // if multi-access or ad hoc face, drop
  if (ingress.face.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                  << "~" << nack.getReason() << " link-type=" << ingress.face.getLinkType());
    return;
  }

  // PIT match
  shared_ptr<pit::Entry> pitEntry = m_pit.find(nack.getInterest());
  // if no PIT entry found, drop
  if (pitEntry == nullptr) {
    NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                  << "~" << nack.getReason() << " no-pit-entry");
    return;
  }

  // has out-record?
  auto outRecord = pitEntry->findOutRecord(ingress.face);
  // if no out-record found, drop
  if (outRecord == pitEntry->out_end()) {
    NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                  << "~" << nack.getReason() << " no-out-record");
    return;
  }

  // if out-record has different Nonce, drop
  if (nack.getInterest().getNonce() != outRecord->getLastNonce()) {
    NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                  << "~" << nack.getReason() << " nonce-mismatch " << nack.getInterest().getNonce()
                  << "!=" << outRecord->getLastNonce());
    return;
  }

  NFD_LOG_DEBUG("onIncomingNack in=" << ingress << " nack=" << nack.getInterest().getName()
                << "~" << nack.getReason());

  // record Nack on out-record
  outRecord->setIncomingNack(nack);

  // set PIT expiry timer to now when all out-record receive Nack
  if (!fw::hasPendingOutRecords(*pitEntry)) {
    this->setExpiryTimer(pitEntry, 0_ms);
  }

  // trigger strategy: after receive Nack
  m_strategyChoice.findEffectiveStrategy(*pitEntry).afterReceiveNack(nack, ingress, pitEntry);
}

bool
Forwarder::onOutgoingNack(const lp::NackHeader& nack, Face& egress,
                          const shared_ptr<pit::Entry>& pitEntry)
{
  if (egress.getId() == face::INVALID_FACEID) {
    NFD_LOG_WARN("onOutgoingNack out=(invalid)" << " nack=" << pitEntry->getName()
                 << "~" << nack.getReason());
    return false;
  }

  // has in-record?
  auto inRecord = pitEntry->findInRecord(egress);

  // if no in-record found, drop
  if (inRecord == pitEntry->in_end()) {
    NFD_LOG_DEBUG("onOutgoingNack out=" << egress.getId() << " nack=" << pitEntry->getName()
                  << "~" << nack.getReason() << " no-in-record");
    return false;
  }

  // if multi-access or ad hoc face, drop
  if (egress.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
    NFD_LOG_DEBUG("onOutgoingNack out=" << egress.getId() << " nack=" << pitEntry->getName()
                  << "~" << nack.getReason() << " link-type=" << egress.getLinkType());
    return false;
  }

  NFD_LOG_DEBUG("onOutgoingNack out=" << egress.getId() << " nack=" << pitEntry->getName()
                << "~" << nack.getReason());

  // create Nack packet with the Interest from in-record
  lp::Nack nackPkt(inRecord->getInterest());
  nackPkt.setHeader(nack);

  // erase in-record
  pitEntry->deleteInRecord(inRecord);

  // send Nack on face
  egress.sendNack(nackPkt);
  ++m_counters.nOutNacks;

  return true;
}

void
Forwarder::onDroppedInterest(const Interest& interest, Face& egress)
{
  m_strategyChoice.findEffectiveStrategy(interest.getName()).onDroppedInterest(interest, egress);
}

void
Forwarder::onNewNextHop(const Name& prefix, const fib::NextHop& nextHop)
{
  const auto affectedEntries = this->getNameTree().partialEnumerate(prefix,
    [&] (const name_tree::Entry& nte) -> std::pair<bool, bool> {
      // we ignore an NTE and skip visiting its descendants if that NTE has an
      // associated FIB entry (1st condition), since in that case the new nexthop
      // won't affect any PIT entries anywhere in that subtree, *unless* this is
      // the initial NTE from which the enumeration started (2nd condition), which
      // must always be considered
      if (nte.getFibEntry() != nullptr && nte.getName().size() > prefix.size()) {
        return {false, false};
      }
      return {nte.hasPitEntries(), true};
    });

  for (const auto& nte : affectedEntries) {
    for (const auto& pitEntry : nte.getPitEntries()) {
      m_strategyChoice.findEffectiveStrategy(*pitEntry).afterNewNextHop(nextHop, pitEntry);
    }
  }
}

void
Forwarder::setExpiryTimer(const shared_ptr<pit::Entry>& pitEntry, time::milliseconds duration)
{
  BOOST_ASSERT(pitEntry);
  duration = std::max(duration, 0_ms);

  pitEntry->expiryTimer.cancel();
  pitEntry->expiryTimer = getScheduler().schedule(duration, [=] { onInterestFinalize(pitEntry); });
}

void
Forwarder::insertDeadNonceList(pit::Entry& pitEntry, const Face* upstream)
{
  // need Dead Nonce List insert?
  bool needDnl = true;
  if (pitEntry.isSatisfied) {
    BOOST_ASSERT(pitEntry.dataFreshnessPeriod >= 0_ms);
    needDnl = pitEntry.getInterest().getMustBeFresh() &&
              pitEntry.dataFreshnessPeriod < m_deadNonceList.getLifetime();
  }

  if (!needDnl) {
    return;
  }

  // Dead Nonce List insert
  if (upstream == nullptr) {
    // insert all outgoing Nonces
    std::for_each(pitEntry.out_begin(), pitEntry.out_end(), [&] (const auto& outRecord) {
      m_deadNonceList.add(pitEntry.getName(), outRecord.getLastNonce());
    });
  }
  else {
    // insert outgoing Nonce of a specific face
    auto outRecord = pitEntry.findOutRecord(*upstream);
    if (outRecord != pitEntry.out_end()) {
      m_deadNonceList.add(pitEntry.getName(), outRecord->getLastNonce());
    }
  }
}

void
Forwarder::setConfigFile(ConfigFile& configFile)
{
  configFile.addSectionHandler(CFG_FORWARDER, [this] (auto&&... args) {
    processConfig(std::forward<decltype(args)>(args)...);
  });
}

void
Forwarder::processConfig(const ConfigSection& configSection, bool isDryRun, const std::string&)
{
  Config config;

  for (const auto& pair : configSection) {
    const std::string& key = pair.first;
    if (key == "default_hop_limit") {
      config.defaultHopLimit = ConfigFile::parseNumber<uint8_t>(pair, CFG_FORWARDER);
    }
    else {
      NDN_THROW(ConfigFile::Error("Unrecognized option " + CFG_FORWARDER + "." + key));
    }
  }

  if (!isDryRun) {
    m_config = config;
  }
}

// --- OptoFlood Implementation ---

void
Forwarder::handleOptoFloodData(Data data, const FaceEndpoint& ingress,
                               const std::unordered_set<uint64_t>& suppressedFaces)
{
  auto floodIdOpt = ::ndn::optoflood::getFloodId(data.getMetaInfo());
  if (!floodIdOpt) {
    NFD_LOG_DEBUG("OptoFlood skip data=" << data.getName() << " reason=no FloodId");
    return; // Malformed, no FloodId
  }

  // Deduplication
  if (m_floodIdCache.count(*floodIdOpt) > 0) {
    NFD_LOG_DEBUG("OptoFlood dedup drop data=" << data.getName()
                  << " floodId=" << *floodIdOpt
                  << " ingress=" << ingress.face.getId());
    return; // Already processed this flood packet
  }
  if (m_floodIdCache.size() >= OPTOFLOOD_FLOOD_ID_CACHE_MAX) {
    // Bound memory. FloodIds are rate-limited and accumulate slowly, while
    // duplicates of a given flood arrive within sub-second propagation. Clearing
    // before inserting the current FloodId drops only long-past entries and never
    // an in-flight duplicate.
    m_floodIdCache.clear();
  }
  m_floodIdCache.insert(*floodIdOpt);

  // Derive the routable prefix from the routing layer (FIB longest-prefix match)
  // instead of a fixed name-component offset. This keys OptoFlood state
  // (rate limiting, TFIB, Fast-LSA) on the advertised producer prefix and makes
  // it independent of the application naming below that prefix (e.g. version and
  // segment components), so Data naming changes do not require forwarder changes.
  const fib::Entry& fibEntry = m_fib.findLongestPrefixMatch(data.getName());
  const Name producerPrefix = fibEntry.getPrefix();
  const bool hasFibNextHops = fibEntry.hasNextHops();

  // OptoFlood state must be keyed on a routable prefix. If no FIB entry covers the
  // Data name (empty match, or only a default route), there is no meaningful key:
  // skip rate limiting, TFIB and Fast-LSA rather than key on the root prefix
  // (over-broad, would capture all Interests) or a per-frame component (ineffective
  // for other frames). The Data still propagates via the flooding path below.
  if (!producerPrefix.empty()) {
    // Rate Limiting
    if (!checkFloodRate(producerPrefix)) {
      NFD_LOG_WARN("OptoFlood rate-limit drop data=" << data.getName()
                   << " floodId=" << *floodIdOpt);
      return; // Rate limit exceeded for this producer
    }

    // Update TFIB
    std::optional<uint32_t> newFaceSeqOpt = ::ndn::optoflood::getNewFaceSeq(data.getMetaInfo());
    if (newFaceSeqOpt) {
      if (ingress.face.getLinkType() == ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
        m_tfib.insert(producerPrefix, ingress.face, *newFaceSeqOpt, *floodIdOpt);
        NFD_LOG_DEBUG("OptoFlood TFIB update prefix=" << producerPrefix
                      << " face=" << ingress.face.getId()
                      << " newFaceSeq=" << *newFaceSeqOpt
                      << " floodId=" << *floodIdOpt);
        triggerFastLsaIfNeeded(producerPrefix, ingress.face, newFaceSeqOpt);
      }
      else {
        NFD_LOG_DEBUG("OptoFlood TFIB skip prefix=" << producerPrefix
                      << " face=" << ingress.face.getId()
                      << " reason=non-p2p-ingress");
      }
    }
    else {
      NFD_LOG_DEBUG("OptoFlood data=" << data.getName()
                    << " floodId=" << *floodIdOpt
                    << " missing NewFaceSeq");
    }
  }

  uint64_t hopLimit = 0;
  // No FIB next-hop: fall back to hop-limited blind flooding over adjacent faces.
  // Defensive path, inert when the producer prefix is advertised network-wide.
  const bool useHopLimit = !hasFibNextHops;
  if (useHopLimit) {
    if (auto tag = data.getTag<ndn::lp::OptoHopLimit>()) {
      hopLimit = *tag;
    }
    else {
      hopLimit = OPTOFLOOD_DATA_HOP_LIMIT;
    }
  }
  else if (auto tag = data.getTag<ndn::lp::OptoHopLimit>()) {
    hopLimit = *tag;
  }

  if (useHopLimit) {
    NFD_LOG_DEBUG("OptoFlood process data=" << data.getName()
                  << " floodId=" << *floodIdOpt
                  << " ingress=" << ingress.face.getId()
                  << " hopLimit=" << hopLimit);
  }
  else {
    NFD_LOG_DEBUG("OptoFlood process data=" << data.getName()
                  << " floodId=" << *floodIdOpt
                  << " ingress=" << ingress.face.getId()
                  << " hopLimit=disabled");
  }

  if (useHopLimit && hopLimit == 0) {
    NFD_LOG_DEBUG("OptoFlood stop data=" << data.getName()
                  << " floodId=" << *floodIdOpt
                  << " reason=hopLimit-zero");
    return;
  }

  // decrement and forward according to policy:
  // (1) Prefer FIB next-hops (non-LOCAL), excluding ingress
  // (2) If no FIB next-hops, fallback to adjacent faces (non-LOCAL), excluding ingress
  if (useHopLimit) {
    data.setTag(std::make_shared<ndn::lp::OptoHopLimit>(hopLimit - 1));
  }
  // Ensure LP MobilityFlag is present during flooding
  data.setTag(std::make_shared<ndn::lp::OptoMobilityFlag>(ndn::lp::EmptyValue()));

  std::vector<Face*> outFaces;
  outFaces.reserve(8);

  if (hasFibNextHops) {
    // Use FIB next hops first
    for (const auto& nh : fibEntry.getNextHops()) {
      Face& out = nh.getFace();
      if (out.getId() == ingress.face.getId()) {
        continue;
      }
      if (suppressedFaces.find(out.getId()) != suppressedFaces.end()) {
        continue;
      }
      if (out.getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {
        continue;
      }
      if (out.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
        continue;
      }
      outFaces.push_back(&out);
    }
  }

  if (outFaces.empty()) {
    // Fallback to adjacent faces (exclude LOCAL and ingress)
    for (auto& f : m_faceTable) {
      if (f.getId() == ingress.face.getId()) {
        continue;
      }
      if (suppressedFaces.find(f.getId()) != suppressedFaces.end()) {
        continue;
      }
      if (f.getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {
        continue;
      }
      if (f.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
        continue;
      }
      outFaces.push_back(&f);
    }
  }

  // Deduplicate and send
  std::unordered_set<uint64_t> sentIds;
  for (Face* f : outFaces) {
    if (!f) continue;
    if (!sentIds.insert(f->getId()).second) {
      continue;
    }
    if (useHopLimit) {
      NFD_LOG_DEBUG("OptoFlood forward data=" << data.getName()
                    << " floodId=" << *floodIdOpt
                    << " outFace=" << f->getId()
                    << " remainingHopLimit=" << (hopLimit - 1));
    }
    else {
      NFD_LOG_DEBUG("OptoFlood forward data=" << data.getName()
                    << " floodId=" << *floodIdOpt
                    << " outFace=" << f->getId()
                    << " remainingHopLimit=disabled");
    }
    f->sendData(data);
  }

  if (outFaces.empty()) {
    NFD_LOG_DEBUG("OptoFlood no eligible outFace data=" << data.getName()
                  << " floodId=" << *floodIdOpt);
  }
}


void
Forwarder::handleInterestFlooding(const Interest& interest, const FaceEndpoint& ingress,
                                  const shared_ptr<pit::Entry>& pitEntry,
                                  std::optional<uint64_t> excludeFaceId,
                                  bool allowIngress)
{
  Interest floodInterest = interest;
  if (!floodInterest.getHopLimit()) {
    floodInterest.setHopLimit(OPTOFLOOD_HOP_LIMIT);
  }

  std::unordered_set<uint64_t> sentFaces;
  for (auto& face : m_faceTable) {
    if (excludeFaceId && face.getId() == *excludeFaceId) {
      continue;
    }
    if (!allowIngress && face.getId() == ingress.face.getId()) {
      continue;
    }
    if (face.getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {
      continue;
    }
    if (face.getLinkType() != ndn::nfd::LINK_TYPE_POINT_TO_POINT) {
      continue;
    }
    if (!sentFaces.insert(face.getId()).second) {
      continue;
    }
    NFD_LOG_DEBUG("OptoFlood forward interest=" << floodInterest.getName()
                  << " nonce=" << floodInterest.getNonce()
                  << " outFace=" << face.getId()
                  << " hopLimit=" << static_cast<unsigned>(*floodInterest.getHopLimit()));
    onOutgoingInterest(floodInterest, face, pitEntry);
  }

  if (sentFaces.empty()) {
    NFD_LOG_DEBUG("OptoFlood interest had no eligible outFace name=" << floodInterest.getName());
  }
}

bool
Forwarder::checkFloodRate(const ndn::Name& producerPrefix)
{
  size_t& counter = m_floodRateMap[producerPrefix];
  ++counter;
  if (counter > OPTOFLOOD_RATE_LIMIT) {
    NFD_LOG_DEBUG("OptoFlood rate counter exceeded prefix=" << producerPrefix
                  << " count=" << counter
                  << " limit=" << OPTOFLOOD_RATE_LIMIT);
    return false;
  }
  return true;
}

void
Forwarder::triggerFastLsaIfNeeded(const ndn::Name& producerPrefix, const Face& face,
                                  std::optional<uint32_t> newFaceSeq)
{
  if (!m_internalController) {
    return;
  }

  auto now = time::steady_clock::now();
  auto it = m_fastLsaThrottle.find(producerPrefix);
  if (it != m_fastLsaThrottle.end() && (now - it->second) < FAST_LSA_THROTTLE) {
    return;
  }
  m_fastLsaThrottle[producerPrefix] = now;

  ndn::nfd::ControlParameters params;
  params.setName(producerPrefix)
        .setFaceId(face.getId())
        .setExpirationPeriod(FAST_LSA_LIFETIME);
  if (newFaceSeq) {
    params.setCost(*newFaceSeq);
  }

  ndn::nfd::CommandOptions opts;
  opts.setPrefix(ndn::Name("/localhost/nlsr"));
  m_internalController->start<FastLsaTriggerCommand>(
    params,
    [] (const ndn::nfd::ControlParameters&) {},
    [] (const ndn::nfd::ControlResponse&) {},
    opts);
}

void
Forwarder::onFaceAdded(const Face& face)
{
  // Existing onFaceAdded logic...
  // For TFIB, when a face is added, there's no immediate action needed.
  // Removal is handled by erase().
}

} // namespace nfd
