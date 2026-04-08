#include "roce-transport-engine.h"

#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RoceTransportEngine");

namespace {

bool
IsRoceBasicLogEnabled(const RoceConfig& cfg) {
  return cfg.logLevel != RoceLogLevel::Off;
}

bool
IsRoceFullLogEnabled(const RoceConfig& cfg) {
  return cfg.logLevel == RoceLogLevel::Full;
}

std::string
ToLowerCopy(std::string v) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return v;
}

RoceUserPhase
ToRoceUserPhase(ThreePHeader::State state) {
  switch (state) {
    case ThreePHeader::REQUEST:
      return RoceUserPhase::Request;
    case ThreePHeader::DATA:
      return RoceUserPhase::Data;
    case ThreePHeader::ACT:
      return RoceUserPhase::Act;
    default:
      return RoceUserPhase::None;
  }
}

RoceOpcode
ToRoceOpcode(ThreePHeader::State state,
             const RoceConfig& cfg,
             const RoceThreePhaseFsm* fsm) {
  const std::string mode = ToLowerCopy(cfg.modeName);
  const bool preferRead = mode.find("read") != std::string::npos;
  const bool preferSend = mode.find("send") != std::string::npos;

  switch (state) {
    case ThreePHeader::REQUEST:
      return preferRead ? RoceOpcode::RdmaReadReq : RoceOpcode::Send;
    case ThreePHeader::DATA:
      if (preferRead) {
        return RoceOpcode::RdmaReadResp;
      }
      if (preferSend) {
        return RoceOpcode::Send;
      }
      if (fsm != nullptr) {
        return fsm->DataOpcode();
      }
      return RoceOpcode::RdmaWrite;
    case ThreePHeader::ACT:
      return RoceOpcode::Send;
    default:
      return RoceOpcode::Send;
  }
}

} // namespace

RoceTransportEngine::~RoceTransportEngine() {
  Reset();
}

void
RoceTransportEngine::SetDispatchWakeup(DispatchWakeupFn wakeup) {
  m_dispatchWakeup = std::move(wakeup);
}

void
RoceTransportEngine::SetNodeId(uint32_t nodeId) {
  m_nodeId = nodeId;
  m_hasNodeId = true;
}

void
RoceTransportEngine::Configure(const RoceConfig& cfg) {
  m_cfg = cfg;
  m_qpEngine.Configure(m_cfg);
  m_fsm.Configure(m_cfg);
  m_txScheduler.Configure(m_cfg);
}

void
RoceTransportEngine::Reset() {
  for (auto& [_, timerState] : m_retransTimers) {
    if (timerState.event.IsPending()) {
      timerState.event.Cancel();
    }
  }
  m_flowToQpn.clear();
  m_outstandingPackets.clear();
  m_retransTimers.clear();
  m_bufferedUpperPackets.clear();
  m_pendingRetransmissions.clear();
  m_recoveryTargetPsns.clear();
  m_nextSyntheticQpn = 1;
  m_nextWorkId = 1;
  m_qpEngine.Reset();
  m_fsm.Reset();
  m_txScheduler.Reset();
}

RoceTransportEngine::RoceLaneRole
RoceTransportEngine::GetLaneRole_(ThreePHeader::State state) {
  switch (state) {
    case ThreePHeader::REQUEST:
      return RoceLaneRole::ReqCtrl;
    case ThreePHeader::DATA:
      return RoceLaneRole::Data;
    case ThreePHeader::ACT:
      return RoceLaneRole::ActCtrl;
    default:
      return RoceLaneRole::Data;
  }
}

const char*
RoceTransportEngine::GetLaneName_(RoceLaneRole lane) {
  switch (lane) {
    case RoceLaneRole::ReqCtrl:
      return "req_ctrl";
    case RoceLaneRole::Data:
      return "data";
    case RoceLaneRole::ActCtrl:
      return "act_ctrl";
    default:
      return "unknown";
  }
}

bool
RoceTransportEngine::HasPendingWork() const {
  return HasPendingSchedulerWork();
}

bool
RoceTransportEngine::HasPendingSchedulerWork() const {
  return m_txScheduler.HasPendingWork();
}

bool
RoceTransportEngine::HasPendingRetransmissions() const {
  return false;
}

bool
RoceTransportEngine::Enqueue(RoceTxWorkItem* item) {
  if (item == nullptr) {
    return false;
  }

  const RoceLaneRole lane = GetLaneRole_(item->state);
  if (item->qpKey.qpn == 0) {
    item->qpKey.qpn = GetOrAllocQpn_(item->flowKey, lane);
  }
  if (item->workId == 0) {
    item->workId = m_nextWorkId++;
  }
  return m_txScheduler.Enqueue(*item);
}

bool
RoceTransportEngine::TryGetNextDispatchAction(uint32_t mtu, DispatchAction* out) {
  if (out == nullptr) {
    return false;
  }
  out->kind = DispatchKind::None;

  const std::set<RoceQpKey> blockedQps = CollectBlockedQps_();
  if (!m_txScheduler.PickNextSlice(mtu, &blockedQps, &out->slice)) {
    return false;
  }
  out->kind = DispatchKind::NewSlice;
  return true;
}

bool
RoceTransportEngine::BuildTxHeader(ThreePHeader::State state,
                                   uint64_t totalDataSize,
                                   uint32_t payloadBytes,
                                   uint32_t tag,
                                   uint32_t src,
                                   uint32_t dst,
                                   RoceSimHeader* headerOut) {
  if (!m_cfg.enable || headerOut == nullptr) {
    return false;
  }

  const RoceFlowKey flowKey{tag, src, dst};
  const RoceLaneRole lane = GetLaneRole_(state);
  const uint32_t qpn = GetOrAllocQpn_(flowKey, lane);
  const RoceQpKey qpKey{src, dst, qpn};
  if (!EnsureQpReadyToSend_(qpKey) || !m_qpEngine.CanPostSend(qpKey)) {
    NS_LOG_WARN("[ROCE][TX] QP not ready tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " lane=" << GetLaneName_(lane)
                << " qpn=" << qpn);
    return false;
  }

  const RoceOpcode opcode = ToRoceOpcode(state, m_cfg, &m_fsm);
  const RoceUserPhase phase = ToRoceUserPhase(state);
  const uint32_t encodedBytes =
      payloadBytes > 0
          ? payloadBytes
          : static_cast<uint32_t>(std::min<uint64_t>(totalDataSize, 0xffffffffULL));
  if (!m_qpEngine.BuildSendHeader(
          qpKey, opcode, phase, encodedBytes, tag, headerOut)) {
    return false;
  }

  const uint64_t remoteAddr =
      (static_cast<uint64_t>(dst) << 32) | static_cast<uint64_t>(tag);
  const uint32_t rkey =
      (tag * 2654435761u) ^ (src << 16) ^ (dst & 0xffffu);
  uint8_t flags = 0;
  if (opcode == RoceOpcode::RdmaWrite || opcode == RoceOpcode::RdmaReadReq ||
      opcode == RoceOpcode::RdmaReadResp) {
    flags |= 0x1;
  }
  if (opcode == RoceOpcode::RdmaReadResp) {
    flags |= 0x2;
  }
  if (opcode == RoceOpcode::Ack) {
    flags |= 0x4;
  }
  headerOut->SetFlags(flags);
  headerOut->SetRemoteAddr(remoteAddr);
  headerOut->SetRkey(rkey);
  headerOut->SetImmData(
      static_cast<uint32_t>(std::min<uint64_t>(totalDataSize, 0xffffffffULL)));
  if (opcode == RoceOpcode::RdmaReadReq) {
    headerOut->SetLength(
        static_cast<uint32_t>(std::min<uint64_t>(totalDataSize, 0xffffffffULL)));
  }

  if (state == ThreePHeader::REQUEST) {
    m_fsm.OnRequestIssued(flowKey);
  } else if (state == ThreePHeader::DATA) {
    m_fsm.OnDataIssued(flowKey);
  }
  return true;
}

bool
RoceTransportEngine::BuildAckHeader(uint32_t tag,
                                    uint32_t src,
                                    uint32_t dst,
                                    uint32_t qpn,
                                    RoceSimHeader* headerOut) {
  if (!m_cfg.enable || headerOut == nullptr) {
    return false;
  }

  const RoceQpKey qpKey{src, dst, qpn};
  if (!EnsureQpReadyToSend_(qpKey) || !m_qpEngine.CanPostSend(qpKey)) {
    return false;
  }
  if (!m_qpEngine.BuildSendHeader(
          qpKey,
          RoceOpcode::Ack,
          RoceUserPhase::None,
          0,
          tag,
          headerOut)) {
    return false;
  }
  headerOut->SetFlags(0x4);
  headerOut->SetRemoteAddr(0);
  headerOut->SetRkey(0);
  headerOut->SetImmData(0);
  headerOut->SetLength(0);
  return true;
}

void
RoceTransportEngine::OnPacketSent(const RoceTxSlice& slice,
                                  const Ptr<Packet>& packet,
                                  const RoceSimHeader& header) {
  if (!m_cfg.enable || !m_cfg.enableTransportAck ||
      header.GetOpcode() == RoceOpcode::Ack || packet == nullptr) {
    return;
  }

  OutstandingPacket outstanding;
  outstanding.packet = packet->Copy();
  outstanding.dest = slice.dest;
  outstanding.sendMode = slice.sendMode;
  outstanding.mindex = slice.mindex;
  outstanding.flowKey = slice.flowKey;
  outstanding.qpKey = slice.qpKey;
  outstanding.psn = header.GetPsn();
  outstanding.retries = 0;
  m_outstandingPackets[slice.qpKey][header.GetPsn()] = outstanding;
}

void
RoceTransportEngine::OnPacketWireTx(const RoceQpKey& qpKey,
                                    uint32_t psn,
                                    bool isRetransmission) {
  (void)qpKey;
  (void)psn;
  (void)isRetransmission;
}

void
RoceTransportEngine::OnRetransmissionSent(const RoceQpKey& qpKey) {
  (void)qpKey;
}

RoceTransportEngine::RxResult
RoceTransportEngine::OnRxPacket(const RoceSimHeader& roceHeader,
                                const ThreePHeader& tpHeader,
                                const Ptr<Packet>& payload,
                                const Address& from,
                                uint32_t formIndex) {
  RxResult result;
  result.accepted = true;
  if (!m_cfg.enable) {
    return result;
  }

  const uint32_t tag = tpHeader.GetTag();
  const uint32_t src = tpHeader.GetSrc();
  const uint32_t dst = tpHeader.GetDst();
  const RoceFlowKey flowKey{tag, src, dst};
  const RoceQpKey qpKey{src, dst, roceHeader.GetQpn()};

  if (roceHeader.GetOpcode() == RoceOpcode::Ack) {
    HandleAck_(flowKey, qpKey, roceHeader);
    return result;
  }

  const RoceLaneRole lane = GetLaneRole_(tpHeader.GetState());
  RememberQpn_(flowKey, lane, roceHeader.GetQpn());

  EnsureQpReadyToSend_(qpKey);

  bool shouldDeliverUpper = true;
  RoceCompletion cqe;
  result.accepted = m_qpEngine.OnRxHeader(qpKey,
                                          roceHeader,
                                          &cqe,
                                          &shouldDeliverUpper,
                                          &result.bufferedForReorder,
                                          &result.overflowedReorderWindow);
  if (!result.accepted) {
    NS_LOG_WARN("[ROCE][RX] QP rejected packet tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << roceHeader.GetQpn()
                << " psn=" << roceHeader.GetPsn());
    return result;
  }

  if (result.overflowedReorderWindow) {
    if (m_cfg.enableTransportAck) {
      result.needAck = true;
    }
    NS_LOG_WARN("[ROCE][RX][REORDER_OVERFLOW] tag=" << tag
                << " node=" << (m_hasNodeId ? m_nodeId : 0)
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << roceHeader.GetQpn()
                << " psn=" << roceHeader.GetPsn()
                << " expectedRxPsn=" << m_qpEngine.GetExpectedRxPsn(qpKey)
                << " pendingRxCount=" << m_qpEngine.GetPendingRxCount(qpKey)
                << " reorderWindowPackets=" << m_cfg.reorderWindowPackets);
  }

  if (result.bufferedForReorder) {
    if (m_cfg.enableTransportAck) {
      result.needAck = true;
    }
    BufferedUpperPacket buffered;
    buffered.payload = payload != nullptr ? payload->Copy() : nullptr;
    buffered.tpHeader = tpHeader;
    buffered.from = from;
    buffered.formIndex = formIndex;
    buffered.hasTransportFragmentId = (tpHeader.GetState() == ThreePHeader::DATA);
    buffered.transportFragmentId = roceHeader.GetPsn();
    m_bufferedUpperPackets[qpKey][roceHeader.GetPsn()] = buffered;

    if (IsRoceFullLogEnabled(m_cfg)) {
      NS_LOG_INFO("[ROCE][RX][REORDER_BUFFER] node=" << (m_hasNodeId ? m_nodeId : 0)
                  << " src=" << src
                  << " dst=" << dst
                  << " qpn=" << roceHeader.GetQpn()
                  << " psn=" << roceHeader.GetPsn()
                  << " expectedRxPsn=" << m_qpEngine.GetExpectedRxPsn(qpKey)
                  << " pendingRxCount=" << m_qpEngine.GetPendingRxCount(qpKey)
                  << " bufferedUpperCount=" << m_bufferedUpperPackets[qpKey].size());
    }
    return result;
  }

  if (m_cfg.enableTransportAck) {
    result.needAck = true;
  }

  if (!shouldDeliverUpper) {
    return result;
  }

  if (roceHeader.GetUserPhase() != ToRoceUserPhase(tpHeader.GetState())) {
    NS_LOG_WARN("[ROCE][RX] phase mismatch tag=" << tag
                << " expected=" << static_cast<uint32_t>(ToRoceUserPhase(tpHeader.GetState()))
                << " got=" << static_cast<uint32_t>(roceHeader.GetUserPhase()));
  }
  const RoceOpcode expectedOpcode = ToRoceOpcode(tpHeader.GetState(), m_cfg, &m_fsm);
  if (roceHeader.GetOpcode() != expectedOpcode) {
    NS_LOG_WARN("[ROCE][RX] opcode mismatch tag=" << tag
                << " expected=" << static_cast<uint32_t>(expectedOpcode)
                << " got=" << static_cast<uint32_t>(roceHeader.GetOpcode()));
  }

  BufferedUpperPacket current;
  current.payload = payload != nullptr ? payload->Copy() : nullptr;
  current.tpHeader = tpHeader;
  current.from = from;
  current.formIndex = formIndex;
  current.hasTransportFragmentId = (tpHeader.GetState() == ThreePHeader::DATA);
  current.transportFragmentId = roceHeader.GetPsn();
  result.readyPackets.push_back(current);
  CollectReadyBufferedPackets_(qpKey, &result.readyPackets);

  if (tpHeader.GetState() == ThreePHeader::ACT) {
    m_fsm.OnActReceived(flowKey);
  }
  return result;
}

uint32_t
RoceTransportEngine::GetExpectedRxPsn(const RoceQpKey& qpKey) const {
  return m_qpEngine.GetExpectedRxPsn(qpKey);
}

uint32_t
RoceTransportEngine::GetPendingRxCount(const RoceQpKey& qpKey) const {
  return m_qpEngine.GetPendingRxCount(qpKey);
}

uint32_t
RoceTransportEngine::GetOutstandingCount(const RoceQpKey& qpKey) const {
  auto it = m_outstandingPackets.find(qpKey);
  if (it == m_outstandingPackets.end()) {
    return 0;
  }
  return static_cast<uint32_t>(it->second.size());
}

uint32_t
RoceTransportEngine::GetOldestOutstandingPsn(const RoceQpKey& qpKey) const {
  auto it = m_outstandingPackets.find(qpKey);
  if (it == m_outstandingPackets.end() || it->second.empty()) {
    return 0;
  }
  return it->second.begin()->first;
}

uint32_t
RoceTransportEngine::GetRecoveryTargetPsn(const RoceQpKey& qpKey) const {
  auto it = m_recoveryTargetPsns.find(qpKey);
  if (it == m_recoveryTargetPsns.end()) {
    return 0;
  }
  return it->second;
}

uint32_t
RoceTransportEngine::GetOrAllocQpn_(const RoceFlowKey& flowKey,
                                    RoceLaneRole lane) {
  const FlowLaneKey flowLaneKey{flowKey, lane};
  auto it = m_flowToQpn.find(flowLaneKey);
  if (it != m_flowToQpn.end()) {
    return it->second;
  }

  uint32_t qpn = m_nextSyntheticQpn++;
  if (qpn == 0) {
    qpn = m_nextSyntheticQpn++;
  }
  m_flowToQpn[flowLaneKey] = qpn;
  return qpn;
}

void
RoceTransportEngine::RememberQpn_(const RoceFlowKey& flowKey,
                                  RoceLaneRole lane,
                                  uint32_t qpn) {
  if (qpn == 0) {
    return;
  }

  const FlowLaneKey flowLaneKey{flowKey, lane};
  auto it = m_flowToQpn.find(flowLaneKey);
  if (it != m_flowToQpn.end() && it->second != qpn) {
    NS_LOG_WARN("[ROCE][QPN] flow remap detected tag=" << flowKey.tag
                << " src=" << flowKey.src
                << " dst=" << flowKey.dst
                << " lane=" << GetLaneName_(lane)
                << " oldQpn=" << it->second
                << " newQpn=" << qpn);
  }
  m_flowToQpn[flowLaneKey] = qpn;
  if (m_nextSyntheticQpn <= qpn) {
    m_nextSyntheticQpn = qpn + 1;
  }
}

bool
RoceTransportEngine::EnsureQpReadyToSend_(const RoceQpKey& key) {
  m_qpEngine.EnsureQp(key);
  RoceQpState state = m_qpEngine.GetQpState(key);
  if (state == RoceQpState::Err) {
    m_qpEngine.ModifyQpState(key, RoceQpState::Reset);
    state = m_qpEngine.GetQpState(key);
  }
  if (state == RoceQpState::Reset) {
    m_qpEngine.ModifyQpState(key, RoceQpState::Init);
    state = m_qpEngine.GetQpState(key);
  }
  if (state == RoceQpState::Init) {
    m_qpEngine.ModifyQpState(key, RoceQpState::Rtr);
    state = m_qpEngine.GetQpState(key);
  }
  if (state == RoceQpState::Rtr) {
    m_qpEngine.ModifyQpState(key, RoceQpState::Rts);
  }
  return m_qpEngine.GetQpState(key) == RoceQpState::Rts;
}

void
RoceTransportEngine::HandleAck_(const RoceFlowKey& flowKey,
                                const RoceQpKey& qpKey,
                                const RoceSimHeader& roceHeader) {
  auto mapIt = m_outstandingPackets.find(qpKey);
  if (mapIt == m_outstandingPackets.end()) {
    return;
  }

  const uint32_t ackPsn = roceHeader.GetAckPsn();
  const uint32_t outstandingBefore = static_cast<uint32_t>(mapIt->second.size());
  const uint32_t oldestOutstandingBefore = mapIt->second.begin()->first;
  const uint32_t recoveryTargetBefore = GetRecoveryTargetPsn(qpKey);
  auto& packets = mapIt->second;
  for (auto it = packets.begin(); it != packets.end();) {
    if (it->first > ackPsn) {
      ++it;
      continue;
    }
    it = packets.erase(it);
  }
  if (packets.empty()) {
    CancelRetransTimer_(qpKey);
    m_outstandingPackets.erase(mapIt);
  } else {
    ArmRetransTimer_(qpKey);
  }

  auto recoveryIt = m_recoveryTargetPsns.find(qpKey);
  if (recoveryIt != m_recoveryTargetPsns.end() &&
      ackPsn >= recoveryIt->second) {
    m_recoveryTargetPsns.erase(recoveryIt);
    m_pendingRetransmissions.erase(qpKey);
  }

  if (IsRoceFullLogEnabled(m_cfg)) {
    NS_LOG_INFO("[ROCE][ACK][RX] tag=" << flowKey.tag
                << " node=" << (m_hasNodeId ? m_nodeId : 0)
                << " src=" << flowKey.src
                << " dst=" << flowKey.dst
                << " qpn=" << qpKey.qpn
                << " psn=" << roceHeader.GetPsn()
                << " ackPsn=" << ackPsn
                << " outstandingBefore=" << outstandingBefore
                << " outstandingAfter=" << GetOutstandingCount(qpKey)
                << " oldestOutstandingBefore=" << oldestOutstandingBefore
                << " oldestOutstandingAfter=" << GetOldestOutstandingPsn(qpKey)
                << " recoveryTargetBefore=" << recoveryTargetBefore
                << " recoveryTargetAfter=" << GetRecoveryTargetPsn(qpKey)
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }
  NotifyDispatchNeeded_();
}

void
RoceTransportEngine::HandleRetransTimeout_(RoceQpKey qpKey) {
  auto mapIt = m_outstandingPackets.find(qpKey);
  if (mapIt == m_outstandingPackets.end()) {
    CancelRetransTimer_(qpKey);
    return;
  }
  auto packetIt = mapIt->second.begin();
  if (packetIt == mapIt->second.end()) {
    CancelRetransTimer_(qpKey);
    return;
  }

  OutstandingPacket& outstanding = packetIt->second;
  const uint32_t psn = outstanding.psn;
  if (outstanding.retries >= m_cfg.maxRetransmissions) {
    NS_LOG_WARN("[ROCE][RTX][DROP] tag=" << outstanding.flowKey.tag
                << " node=" << (m_hasNodeId ? m_nodeId : 0)
                << " src=" << outstanding.flowKey.src
                << " dst=" << outstanding.flowKey.dst
                << " qpn=" << qpKey.qpn
                << " psn=" << psn
                << " retries=" << outstanding.retries
                << " outstandingCount=" << GetOutstandingCount(qpKey)
                << " oldestOutstanding=" << GetOldestOutstandingPsn(qpKey)
                << " recoveryTarget=" << GetRecoveryTargetPsn(qpKey));
    mapIt->second.erase(packetIt);
    if (mapIt->second.empty()) {
      CancelRetransTimer_(qpKey);
      m_outstandingPackets.erase(mapIt);
    } else {
      ArmRetransTimer_(qpKey);
    }
    m_pendingRetransmissions.erase(qpKey);
    m_recoveryTargetPsns.erase(qpKey);
    NotifyDispatchNeeded_();
    return;
  }

  if (IsRoceBasicLogEnabled(m_cfg)) {
    NS_LOG_INFO("[ROCE][RTX][TIMEOUT] tag=" << outstanding.flowKey.tag
                << " node=" << (m_hasNodeId ? m_nodeId : 0)
                << " src=" << outstanding.flowKey.src
                << " dst=" << outstanding.flowKey.dst
                << " qpn=" << qpKey.qpn
                << " psn=" << psn
                << " retry=" << outstanding.retries
                << " outstandingCount=" << GetOutstandingCount(qpKey)
                << " oldestOutstanding=" << GetOldestOutstandingPsn(qpKey)
                << " recoveryTarget=" << GetRecoveryTargetPsn(qpKey)
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }
  m_pendingRetransmissions[qpKey] = outstanding;
  m_recoveryTargetPsns[qpKey] = psn;
  NotifyDispatchNeeded_();
}

void
RoceTransportEngine::ArmRetransTimer_(const RoceQpKey& qpKey) {
  auto outstandingIt = m_outstandingPackets.find(qpKey);
  if (outstandingIt == m_outstandingPackets.end() ||
      outstandingIt->second.empty()) {
    CancelRetransTimer_(qpKey);
    return;
  }

  const uint32_t oldestPsn = outstandingIt->second.begin()->first;
  auto& timerState = m_retransTimers[qpKey];
  if (timerState.event.IsPending()) {
    timerState.event.Cancel();
  }
  timerState.trackedPsn = oldestPsn;
  timerState.event = Simulator::Schedule(
      NanoSeconds(m_cfg.ackTimeoutNs),
      &RoceTransportEngine::HandleRetransTimeout_,
      this,
      qpKey);
}

void
RoceTransportEngine::CancelRetransTimer_(const RoceQpKey& qpKey) {
  auto it = m_retransTimers.find(qpKey);
  if (it == m_retransTimers.end()) {
    return;
  }
  if (it->second.event.IsPending()) {
    it->second.event.Cancel();
  }
  m_retransTimers.erase(it);
}

uint32_t
RoceTransportEngine::GetTransportWindowPackets_() const {
  return std::max<uint32_t>(1, m_cfg.transportWindowPackets);
}

std::set<RoceQpKey>
RoceTransportEngine::CollectBlockedQps_() const {
  std::set<RoceQpKey> blocked;
  const uint32_t windowPackets = GetTransportWindowPackets_();
  for (const auto& [qpKey, packets] : m_outstandingPackets) {
    if (packets.size() >= windowPackets) {
      blocked.insert(qpKey);
    }
  }
  for (const auto& [qpKey, _] : m_recoveryTargetPsns) {
    blocked.insert(qpKey);
  }
  return blocked;
}

void
RoceTransportEngine::CollectReadyBufferedPackets_(
    const RoceQpKey& qpKey,
    std::vector<BufferedUpperPacket>* out) {
  if (out == nullptr) {
    return;
  }

  auto bufferedIt = m_bufferedUpperPackets.find(qpKey);
  if (bufferedIt == m_bufferedUpperPackets.end()) {
    return;
  }

  const uint32_t deliverUpToPsn = m_qpEngine.GetExpectedRxPsn(qpKey);
  auto& packets = bufferedIt->second;
  while (!packets.empty()) {
    auto it = packets.begin();
    if (it->first >= deliverUpToPsn) {
      break;
    }
    if (IsRoceFullLogEnabled(m_cfg)) {
      NS_LOG_INFO("[ROCE][RX][REORDER_RELEASE] node=" << (m_hasNodeId ? m_nodeId : 0)
                  << " src=" << qpKey.src
                  << " dst=" << qpKey.dst
                  << " qpn=" << qpKey.qpn
                  << " psn=" << it->first
                  << " deliverUpToPsn=" << deliverUpToPsn
                  << " remainingBuffered=" << packets.size());
    }
    out->push_back(it->second);
    packets.erase(it);
  }
  if (packets.empty()) {
    m_bufferedUpperPackets.erase(bufferedIt);
  }
}

void
RoceTransportEngine::NotifyDispatchNeeded_() {
  if (m_dispatchWakeup) {
    m_dispatchWakeup();
  }
}

} // namespace ns3
