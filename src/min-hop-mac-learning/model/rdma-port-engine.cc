#include "rdma-port-engine.h"

#include "PppRoundRobinQueue.h"
#include "ns3/log.h"
#include "ns3/mac48-address.h"
#include "ns3/nstime.h"
#include "ns3/queue.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "rdma-dispatch-tag.h"

#include <algorithm>
#include <utility>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RdmaPortEngine");
NS_OBJECT_ENSURE_REGISTERED(RdmaPortEngine);

TypeId
RdmaPortEngine::GetTypeId() {
  static TypeId tid =
      TypeId("ns3::RdmaPortEngine")
          .SetParent<Object>()
          .SetGroupName("Applications")
          .AddConstructor<RdmaPortEngine>();
  return tid;
}

RdmaPortEngine::RdmaPortEngine() = default;

RdmaPortEngine::~RdmaPortEngine() = default;

void
RdmaPortEngine::AttachToDevice(Ptr<PointToPointNetDevice> device) {
  if (m_device == device && m_tracesConnected) {
    return;
  }

  m_device = device;
  EnsureQueueState_();
  if (!m_device || m_tracesConnected) {
    return;
  }

  m_device->TraceConnectWithoutContext("PhyTxEnd",
                                       MakeCallback(&RdmaPortEngine::OnPhyTxEnd_, this));
  m_device->TraceConnectWithoutContext("QueuePauseState",
                                       MakeCallback(&RdmaPortEngine::OnQueuePauseState_, this));
  m_tracesConnected = true;
}

void
RdmaPortEngine::Configure(const RoceConfig& cfg) {
  m_cfg = cfg;
  EnsureQueueState_();
}

void
RdmaPortEngine::Reset() {
  if (m_drainEvent.IsPending()) {
    Simulator::Cancel(m_drainEvent);
  }
  for (auto& [_, timerState] : m_retransTimers) {
    if (timerState.event.IsPending()) {
      timerState.event.Cancel();
    }
  }
  for (auto& q : m_controlPending) {
    q.clear();
  }
  for (auto& q : m_dataPending) {
    q.clear();
  }
  std::fill(m_inflightPackets.begin(), m_inflightPackets.end(), 0);
  m_outstandingPackets.clear();
  m_retransTimers.clear();
  m_pendingRetransmissions.clear();
  m_recoveryTargetPsns.clear();
  EnsureQueueState_();
}

void
RdmaPortEngine::SetTxNotifyCallback(TxNotifyCallback callback) {
  m_txNotify = callback;
}

bool
RdmaPortEngine::SubmitFromFlowIo(const Address& src,
                                 const Address& dst,
                                 uint16_t protocol,
                                 Ptr<Packet> packet) {
  if (!packet) {
    return false;
  }

  PortTxItem item;
  item.packet = packet->Copy();
  item.src = src;
  item.dst = dst;
  item.protocol = protocol;
  item.qid = ClassifyQueueId_(dst);
  RdmaDispatchTag tag;
  if (item.packet->PeekPacketTag(tag)) {
    item.controlClass = tag.GetTxClass() == RoceTxClass::Control || tag.IsTransportAck();
  }
  if (TrySendNow_(&item)) {
    return true;
  }
  EnqueuePending_(std::move(item));
  ScheduleDrain_(false);
  return true;
}

bool
RdmaPortEngine::SubmitFromBridgeEgress(const Address& src,
                                       const Address& dst,
                                       uint16_t protocol,
                                       Ptr<Packet> packet) {
  return SubmitFromFlowIo(src, dst, protocol, packet);
}

bool
RdmaPortEngine::HandleAck(const RoceSimHeader& roceHeader,
                          const ThreePHeader& tpHeader) {
  if (!m_cfg.enable || !m_cfg.enableTransportAck ||
      roceHeader.GetOpcode() != RoceOpcode::Ack) {
    return false;
  }

  const RoceFlowKey flowKey{tpHeader.GetTag(), tpHeader.GetSrc(), tpHeader.GetDst()};
  const RoceQpKey qpKey{flowKey.src, flowKey.dst, roceHeader.GetQpn()};
  auto mapIt = m_outstandingPackets.find(qpKey);
  if (mapIt == m_outstandingPackets.end()) {
    return false;
  }

  const uint32_t ackPsn = roceHeader.GetAckPsn();
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
  if (recoveryIt != m_recoveryTargetPsns.end() && ackPsn >= recoveryIt->second) {
    m_recoveryTargetPsns.erase(recoveryIt);
    m_pendingRetransmissions.erase(qpKey);
  }
  return true;
}

void
RdmaPortEngine::DoDispose() {
  Reset();
  m_device = nullptr;
  Object::DoDispose();
}

void
RdmaPortEngine::EnsureQueueState_() {
  const uint32_t numQueues = GetNumQueues_();
  if (m_controlPending.size() != numQueues) {
    m_controlPending.assign(numQueues, {});
  }
  if (m_dataPending.size() != numQueues) {
    m_dataPending.assign(numQueues, {});
  }
  if (m_inflightPackets.size() != numQueues) {
    m_inflightPackets.assign(numQueues, 0);
  }
  if (m_knownPaused.size() != numQueues) {
    m_knownPaused.assign(numQueues, false);
  }
  if (m_rrIndex >= numQueues) {
    m_rrIndex = 0;
  }
}

uint32_t
RdmaPortEngine::GetNumQueues_() const {
  if (!m_device) {
    return 1;
  }
  Ptr<Queue<Packet>> queue = m_device->GetQueue();
  Ptr<PppRoundRobinQueue<Packet>> rrQueue = DynamicCast<PppRoundRobinQueue<Packet>>(queue);
  if (!rrQueue || rrQueue->GetNumQueues() == 0) {
    return 1;
  }
  return rrQueue->GetNumQueues();
}

uint32_t
RdmaPortEngine::ClassifyQueueId_(const Address& dst) const {
  if (!m_device) {
    return 0;
  }
  Ptr<Queue<Packet>> queue = m_device->GetQueue();
  Ptr<PppRoundRobinQueue<Packet>> rrQueue = DynamicCast<PppRoundRobinQueue<Packet>>(queue);
  if (!rrQueue || !Mac48Address::IsMatchingType(dst)) {
    return 0;
  }
  return rrQueue->GetQueueIndexByDst(Mac48Address::ConvertFrom(dst)) %
         std::max<uint32_t>(1, rrQueue->GetNumQueues());
}

bool
RdmaPortEngine::IsQueuePaused_(uint32_t qid) const {
  if (!m_device) {
    return false;
  }
  Ptr<Queue<Packet>> queue = m_device->GetQueue();
  Ptr<PppRoundRobinQueue<Packet>> rrQueue = DynamicCast<PppRoundRobinQueue<Packet>>(queue);
  if (rrQueue) {
    return rrQueue->IsSubQueuePaused(qid % std::max<uint32_t>(1, rrQueue->GetNumQueues()));
  }
  if (qid < m_knownPaused.size()) {
    return m_knownPaused[qid];
  }
  return false;
}

uint32_t
RdmaPortEngine::GetInflightLimit_() const {
  return std::max<uint32_t>(1, m_cfg.transportWindowPackets);
}

bool
RdmaPortEngine::HasPendingWork_() const {
  for (const auto& q : m_controlPending) {
    if (!q.empty()) {
      return true;
    }
  }
  for (const auto& q : m_dataPending) {
    if (!q.empty()) {
      return true;
    }
  }
  return false;
}

bool
RdmaPortEngine::CanAdmit_(uint32_t qid) const {
  if (IsQueuePaused_(qid)) {
    return false;
  }
  if (qid >= m_inflightPackets.size()) {
    return false;
  }
  return m_inflightPackets[qid] < GetInflightLimit_();
}

void
RdmaPortEngine::EnqueuePending_(PortTxItem&& item) {
  EnsureQueueState_();
  const uint32_t qid = item.qid % std::max<uint32_t>(1, static_cast<uint32_t>(m_controlPending.size()));
  if (item.controlClass) {
    m_controlPending[qid].push_back(std::move(item));
  } else {
    m_dataPending[qid].push_back(std::move(item));
  }
}

bool
RdmaPortEngine::TrySendNow_(PortTxItem* item) {
  if (item == nullptr || !item->packet || !m_device) {
    return false;
  }

  EnsureQueueState_();
  const uint32_t qid = item->qid % std::max<uint32_t>(1, static_cast<uint32_t>(m_inflightPackets.size()));
  if (!CanAdmit_(qid)) {
    return false;
  }

  RdmaDispatchTag tag;
  if (!item->packet->PeekPacketTag(tag)) {
    return false;
  }
  if (tag.IsRetransmission()) {
    const RoceFlowKey flowKey = tag.GetFlowKey();
    const RoceQpKey qpKey{flowKey.src, flowKey.dst, tag.GetQpn()};
    auto mapIt = m_outstandingPackets.find(qpKey);
    if (mapIt == m_outstandingPackets.end() ||
        mapIt->second.find(tag.GetPsn()) == mapIt->second.end()) {
      m_pendingRetransmissions.erase(qpKey);
      m_recoveryTargetPsns.erase(qpKey);
      return true;
    }
  }
  tag.SetQueueId(qid);
  item->packet->ReplacePacketTag(tag);

  if (!m_device->SendFrom(item->packet->Copy(), item->src, item->dst, item->protocol)) {
    return false;
  }

  ++m_inflightPackets[qid];
  TrackSuccessfulSend_(*item);
  if (!m_txNotify.IsNull()) {
    m_txNotify(item->packet->Copy());
  }
  return true;
}

void
RdmaPortEngine::DrainPending_() {
  EnsureQueueState_();
  if (!HasPendingWork_()) {
    return;
  }

  const uint32_t numQueues = std::max<uint32_t>(1, static_cast<uint32_t>(m_controlPending.size()));
  uint32_t visited = 0;
  while (visited < numQueues) {
    const uint32_t qid = m_rrIndex % numQueues;
    m_rrIndex = (m_rrIndex + 1) % numQueues;
    ++visited;

    if (!CanAdmit_(qid)) {
      continue;
    }

    std::deque<PortTxItem>* queue = nullptr;
    if (!m_controlPending[qid].empty()) {
      queue = &m_controlPending[qid];
    } else if (!m_dataPending[qid].empty()) {
      queue = &m_dataPending[qid];
    } else {
      continue;
    }

    NS_ASSERT(queue != nullptr);
    const bool sent = TrySendNow_(&queue->front());
    if (!sent) {
      ScheduleDrain_(true);
      return;
    }
    queue->pop_front();
    visited = 0;
  }

  if (HasPendingWork_()) {
    ScheduleDrain_(true);
  }
}

void
RdmaPortEngine::ScheduleDrain_(bool delayed) {
  if (!HasPendingWork_() && m_pendingRetransmissions.empty()) {
    return;
  }
  if (m_drainEvent.IsPending()) {
    return;
  }
  if (delayed) {
    m_drainEvent = Simulator::Schedule(MicroSeconds(2), &RdmaPortEngine::DrainPending_, this);
  } else {
    m_drainEvent = Simulator::ScheduleNow(&RdmaPortEngine::DrainPending_, this);
  }
}

void
RdmaPortEngine::TrackSuccessfulSend_(const PortTxItem& item) {
  if (!m_cfg.enable || !m_cfg.enableTransportAck || item.packet == nullptr) {
    return;
  }

  RdmaDispatchTag tag;
  if (!item.packet->PeekPacketTag(tag) || tag.IsTransportAck()) {
    return;
  }

  const RoceFlowKey flowKey = tag.GetFlowKey();
  const RoceQpKey qpKey{flowKey.src, flowKey.dst, tag.GetQpn()};
  auto& packets = m_outstandingPackets[qpKey];
  auto packetIt = packets.find(tag.GetPsn());
  if (packetIt == packets.end()) {
    OutstandingPacket outstanding;
    outstanding.packet = item.packet->Copy();
    outstanding.src = item.src;
    outstanding.dst = item.dst;
    outstanding.protocol = item.protocol;
    outstanding.qid = item.qid;
    outstanding.controlClass = item.controlClass;
    outstanding.flowKey = flowKey;
    outstanding.qpKey = qpKey;
    outstanding.psn = tag.GetPsn();
    outstanding.retries = 0;
    packetIt = packets.emplace(tag.GetPsn(), std::move(outstanding)).first;
  }

  OutstandingPacket& outstanding = packetIt->second;
  if (tag.IsRetransmission()) {
    outstanding.retries++;
    m_pendingRetransmissions.erase(qpKey);
  }
  ArmRetransTimer_(qpKey);
}

void
RdmaPortEngine::HandleRetransTimeout_(const RoceQpKey& qpKey) {
  auto mapIt = m_outstandingPackets.find(qpKey);
  if (mapIt == m_outstandingPackets.end() || mapIt->second.empty()) {
    CancelRetransTimer_(qpKey);
    return;
  }

  OutstandingPacket& outstanding = mapIt->second.begin()->second;
  if (outstanding.retries >= m_cfg.maxRetransmissions) {
    mapIt->second.erase(mapIt->second.begin());
    if (mapIt->second.empty()) {
      CancelRetransTimer_(qpKey);
      m_outstandingPackets.erase(mapIt);
    } else {
      ArmRetransTimer_(qpKey);
    }
    m_pendingRetransmissions.erase(qpKey);
    m_recoveryTargetPsns.erase(qpKey);
    return;
  }

  OutstandingPacket pending = outstanding;
  RdmaDispatchTag tag;
  if (pending.packet != nullptr && pending.packet->PeekPacketTag(tag)) {
    tag.SetRetransmission(true);
    pending.packet->ReplacePacketTag(tag);
  }
  m_pendingRetransmissions[qpKey] = pending;
  m_recoveryTargetPsns[qpKey] = pending.psn;

  PortTxItem item;
  item.packet = pending.packet != nullptr ? pending.packet->Copy() : nullptr;
  item.src = pending.src;
  item.dst = pending.dst;
  item.protocol = pending.protocol;
  item.qid = pending.qid;
  item.controlClass = pending.controlClass;
  EnqueuePending_(std::move(item));
  ScheduleDrain_(false);
}

void
RdmaPortEngine::ArmRetransTimer_(const RoceQpKey& qpKey) {
  auto outstandingIt = m_outstandingPackets.find(qpKey);
  if (outstandingIt == m_outstandingPackets.end() || outstandingIt->second.empty()) {
    CancelRetransTimer_(qpKey);
    return;
  }

  const uint32_t oldestPsn = outstandingIt->second.begin()->first;
  auto& timerState = m_retransTimers[qpKey];
  if (timerState.event.IsPending()) {
    timerState.event.Cancel();
  }
  timerState.trackedPsn = oldestPsn;
  timerState.event = Simulator::Schedule(NanoSeconds(m_cfg.ackTimeoutNs),
                                         &RdmaPortEngine::HandleRetransTimeout_,
                                         this,
                                         qpKey);
}

void
RdmaPortEngine::CancelRetransTimer_(const RoceQpKey& qpKey) {
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
RdmaPortEngine::GetOutstandingCount_(const RoceQpKey& qpKey) const {
  auto it = m_outstandingPackets.find(qpKey);
  if (it == m_outstandingPackets.end()) {
    return 0;
  }
  return static_cast<uint32_t>(it->second.size());
}

uint32_t
RdmaPortEngine::GetOldestOutstandingPsn_(const RoceQpKey& qpKey) const {
  auto it = m_outstandingPackets.find(qpKey);
  if (it == m_outstandingPackets.end() || it->second.empty()) {
    return 0;
  }
  return it->second.begin()->first;
}

uint32_t
RdmaPortEngine::GetRecoveryTargetPsn_(const RoceQpKey& qpKey) const {
  auto it = m_recoveryTargetPsns.find(qpKey);
  if (it == m_recoveryTargetPsns.end()) {
    return 0;
  }
  return it->second;
}

void
RdmaPortEngine::OnQueuePauseState_(uint32_t qid, bool paused) {
  EnsureQueueState_();
  if (qid < m_knownPaused.size()) {
    m_knownPaused[qid] = paused;
  }
  if (!paused) {
    ScheduleDrain_(false);
  }
}

void
RdmaPortEngine::OnPhyTxEnd_(Ptr<const Packet> packet) {
  if (!packet) {
    return;
  }

  RdmaDispatchTag tag;
  Ptr<Packet> copy = packet->Copy();
  if (!copy->PeekPacketTag(tag)) {
    return;
  }

  EnsureQueueState_();
  const uint32_t qid = tag.GetQueueId() % std::max<uint32_t>(1, static_cast<uint32_t>(m_inflightPackets.size()));
  if (qid < m_inflightPackets.size() && m_inflightPackets[qid] > 0) {
    --m_inflightPackets[qid];
  }
  if (HasPendingWork_()) {
    ScheduleDrain_(false);
  }
}

} // namespace ns3
