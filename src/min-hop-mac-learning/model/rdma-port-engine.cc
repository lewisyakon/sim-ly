#include "rdma-port-engine.h"

#include "PppRoundRobinQueue.h"
#include "hop-count-header.h"
#include "ns3/log.h"
#include "ns3/mac48-address.h"
#include "ns3/nstime.h"
#include "ns3/packet-socket-address.h"
#include "ns3/queue.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "rdma-dispatch-tag.h"

#include <algorithm>
#include <utility>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RdmaPortEngine");
NS_OBJECT_ENSURE_REGISTERED(RdmaPortEngine);

namespace {

uint16_t
SafePayloadBytes(int64_t rawBytes) {
  return rawBytes > 0 ? static_cast<uint16_t>(rawBytes) : 0;
}

Address
ToWireAddress(const Address& addr) {
  if (PacketSocketAddress::IsMatchingType(addr)) {
    return PacketSocketAddress::ConvertFrom(addr).GetPhysicalAddress();
  }
  return addr;
}

} // namespace

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
  m_txScheduler.Configure(m_cfg);
  m_qpEngine.Configure(m_cfg);
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
  m_knownPaused.assign(m_knownPaused.size(), false);

  m_qpRuntimes.clear();
  m_bufferedUpperPackets.clear();
  m_outstandingPackets.clear();
  m_retransTimers.clear();
  m_pendingRetransmissions.clear();
  m_recoveryTargetPsns.clear();
  m_txScheduler.Reset();
  m_qpEngine.Reset();
  EnsureQueueState_();
}

void
RdmaPortEngine::SetTxNotifyCallback(TxNotifyCallback callback) {
  m_txNotify = callback;
}

void
RdmaPortEngine::SetNodeId(uint32_t nodeId) {
  m_nodeId = nodeId;
  m_hasNodeId = true;
}

bool
RdmaPortEngine::PostSend(const RoceTxWorkItem& item) {
  if (!m_cfg.enable || !m_device) {
    return false;
  }
  if (item.remainingBytes == 0) {
    return false;
  }

  QpRuntime& runtime = EnsureQpRuntime_(item);
  if (runtime.sqEntries.size() >= runtime.sqDepth) {
    NS_LOG_WARN("[ROCE][SQ][FULL] qpn=" << item.qpKey.qpn
                << " src=" << item.qpKey.src
                << " dst=" << item.qpKey.dst
                << " sqDepth=" << runtime.sqDepth);
    return false;
  }

  SqEntry sqEntry;
  sqEntry.workId = item.workId;
  sqEntry.state = item.state;
  sqEntry.flowKey = item.flowKey;
  sqEntry.totalBytes = item.totalBytes;
  sqEntry.totalPackets = item.totalPackets;
  sqEntry.txClass = item.txClass;
  runtime.sqEntries.push_back(sqEntry);

  if (!m_txScheduler.Enqueue(item)) {
    runtime.sqEntries.pop_back();
    return false;
  }
  ScheduleDrain_(false);
  return true;
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

RdmaPortEngine::RxResult
RdmaPortEngine::HandleIncomingPacket(const RoceSimHeader& roceHeader,
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
  const RoceQpKey qpKey{src, dst, roceHeader.GetQpn()};

  if (roceHeader.GetOpcode() == RoceOpcode::Ack) {
    HandleAck(roceHeader, tpHeader);
    return result;
  }

  QpRuntime& runtime = EnsureQpRuntime_(qpKey);
  if (runtime.rqInflight.size() >= runtime.rqDepth &&
      runtime.rqInflight.find(roceHeader.GetPsn()) == runtime.rqInflight.end()) {
    result.accepted = false;
    NS_LOG_WARN("[ROCE][RQ][FULL] packet rejected before rx header tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << roceHeader.GetQpn()
                << " psn=" << roceHeader.GetPsn()
                << " rqDepth=" << runtime.rqDepth);
    return result;
  }
  EnsureQpReadyToSend_(qpKey);

  bool shouldDeliverUpper = true;
  result.accepted = m_qpEngine.OnRxHeader(qpKey,
                                          roceHeader,
                                          nullptr,
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

  if (m_cfg.enableTransportAck) {
    MaybeSendAck_(ToWireAddress(from),
                  tag,
                  src,
                  dst,
                  roceHeader.GetQpn(),
                  roceHeader.GetOpcode());
  }

  if (result.overflowedReorderWindow) {
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
    if (!AcquireRqEntry_(qpKey, roceHeader.GetPsn())) {
      result.accepted = false;
      NS_LOG_WARN("[ROCE][RQ][FULL] buffered packet rejected tag=" << tag
                  << " src=" << src
                  << " dst=" << dst
                  << " qpn=" << roceHeader.GetQpn()
                  << " psn=" << roceHeader.GetPsn()
                  << " rqDepth=" << EnsureQpRuntime_(qpKey).rqDepth);
      return result;
    }
    BufferedUpperPacket buffered;
    buffered.payload = payload != nullptr ? payload->Copy() : nullptr;
    buffered.tpHeader = tpHeader;
    buffered.from = from;
    buffered.formIndex = formIndex;
    buffered.hasTransportFragmentId = (tpHeader.GetState() == ThreePHeader::DATA);
    buffered.transportFragmentId = roceHeader.GetPsn();
    buffered.rqPsn = roceHeader.GetPsn();
    buffered.qpKey = qpKey;
    m_bufferedUpperPackets[qpKey][roceHeader.GetPsn()] = buffered;

    if (m_cfg.logLevel == RoceLogLevel::Full) {
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

  if (!shouldDeliverUpper) {
    return result;
  }

  if (!AcquireRqEntry_(qpKey, roceHeader.GetPsn())) {
    result.accepted = false;
    NS_LOG_WARN("[ROCE][RQ][FULL] packet rejected tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << roceHeader.GetQpn()
                << " psn=" << roceHeader.GetPsn()
                << " rqDepth=" << EnsureQpRuntime_(qpKey).rqDepth);
    return result;
  }

  BufferedUpperPacket current;
  current.payload = payload != nullptr ? payload->Copy() : Create<Packet>(0);
  current.tpHeader = tpHeader;
  current.from = from;
  current.formIndex = formIndex;
  current.hasTransportFragmentId = (tpHeader.GetState() == ThreePHeader::DATA);
  current.transportFragmentId = roceHeader.GetPsn();
  current.rqPsn = roceHeader.GetPsn();
  current.qpKey = qpKey;
  result.readyPackets.push_back(current);
  PushCompletion_(qpKey,
                  roceHeader.GetPsn(),
                  roceHeader.GetPsn(),
                  true,
                  RoceCompletionKind::Recv);
  ReleaseRqEntry_(qpKey, roceHeader.GetPsn());
  CollectReadyBufferedPackets_(qpKey, &result.readyPackets);
  return result;
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

  MaybeReleaseIdleTxQp_(qpKey);
  ScheduleDrain_(false);
  return true;
}

void
RdmaPortEngine::ReleaseQpState(const RoceQpKey& qpKey) {
  ReleaseQpState_(qpKey, true);
}

uint32_t
RdmaPortEngine::GetCompletionCount(const RoceQpKey& qpKey,
                                   RoceCompletionKind kind) const {
  auto it = m_qpRuntimes.find(qpKey);
  if (it == m_qpRuntimes.end()) {
    return 0;
  }
  if (kind == RoceCompletionKind::Any) {
    return static_cast<uint32_t>(it->second.completions.size());
  }
  if (kind == RoceCompletionKind::Send) {
    return it->second.sendCompletionCount;
  }
  if (kind == RoceCompletionKind::Recv) {
    return it->second.recvCompletionCount;
  }
  return static_cast<uint32_t>(it->second.completions.size());
}

bool
RdmaPortEngine::PopCompletion(const RoceQpKey& qpKey,
                              RoceCompletion* completion,
                              RoceCompletionKind kind) {
  if (completion == nullptr) {
    return false;
  }
  auto it = m_qpRuntimes.find(qpKey);
  if (it == m_qpRuntimes.end() || it->second.completions.empty()) {
    return false;
  }

  auto eraseCompletion = [&](std::deque<RoceCompletion>::iterator target) {
    if (target->kind == RoceCompletionKind::Send && it->second.sendCompletionCount != 0) {
      --it->second.sendCompletionCount;
    } else if (target->kind == RoceCompletionKind::Recv &&
               it->second.recvCompletionCount != 0) {
      --it->second.recvCompletionCount;
    }
    *completion = *target;
    it->second.completions.erase(target);
    return true;
  };

  if (kind == RoceCompletionKind::Any) {
    return eraseCompletion(it->second.completions.begin());
  }

  auto target = std::find_if(it->second.completions.begin(),
                             it->second.completions.end(),
                             [kind](const RoceCompletion& c) { return c.kind == kind; });
  if (target == it->second.completions.end()) {
    return false;
  }
  return eraseCompletion(target);
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
RdmaPortEngine::GetSubQueueCapacity_() const {
  if (!m_device) {
    return 0;
  }

  Ptr<Queue<Packet>> queue = m_device->GetQueue();
  Ptr<PppRoundRobinQueue<Packet>> rrQueue = DynamicCast<PppRoundRobinQueue<Packet>>(queue);
  if (!rrQueue) {
    return 0;
  }
  return rrQueue->GetMaxPacketsPerQueue();
}

uint32_t
RdmaPortEngine::GetSubQueueOccupancy_(uint32_t qid) const {
  if (!m_device) {
    return 0;
  }

  Ptr<Queue<Packet>> queue = m_device->GetQueue();
  Ptr<PppRoundRobinQueue<Packet>> rrQueue = DynamicCast<PppRoundRobinQueue<Packet>>(queue);
  if (!rrQueue || rrQueue->GetNumQueues() == 0) {
    return 0;
  }
  return rrQueue->GetSubQueueLength(qid % std::max<uint32_t>(1, rrQueue->GetNumQueues()));
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
  return m_txScheduler.HasPendingWork();
}

bool
RdmaPortEngine::CanAdmit_(uint32_t qid, bool controlClass) const {
  if (controlClass) {
    return true;
  }
  if (IsQueuePaused_(qid)) {
    return false;
  }
  const uint32_t capacity = GetSubQueueCapacity_();
  if (capacity == 0) {
    return true;
  }
  return GetSubQueueOccupancy_(qid) < capacity;
}

RdmaPortEngine::QpRuntime&
RdmaPortEngine::EnsureQpRuntime_(const RoceTxWorkItem& item) {
  QpRuntime& runtime = EnsureQpRuntime_(item.qpKey);
  runtime.dest = item.dest;
  runtime.hasDest = true;
  runtime.txClass = item.txClass;
  return runtime;
}

RdmaPortEngine::QpRuntime&
RdmaPortEngine::EnsureQpRuntime_(const RoceQpKey& key) {
  QpRuntime& runtime = m_qpRuntimes[key];
  if (runtime.sqDepth == 0) {
    runtime.sqDepth = std::max<uint32_t>(1, m_cfg.defaultSqDepth);
  }
  if (runtime.rqDepth == 0) {
    runtime.rqDepth = std::max<uint32_t>(1, m_cfg.defaultRqDepth);
  }
  if (runtime.cqDepth == 0) {
    runtime.cqDepth = std::max<uint32_t>(1, runtime.sqDepth + runtime.rqDepth);
  }
  return runtime;
}

bool
RdmaPortEngine::EnsureQpReadyToSend_(const RoceQpKey& key) {
  EnsureQpRuntime_(key);
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

std::set<RoceQpKey>
RdmaPortEngine::CollectBlockedQps_() const {
  std::set<RoceQpKey> blocked;
  const uint32_t windowPackets = std::max<uint32_t>(1, m_cfg.transportWindowPackets);

  for (const auto& [qpKey, packets] : m_outstandingPackets) {
    if (packets.size() >= windowPackets) {
      blocked.insert(qpKey);
    }
  }
  for (const auto& [qpKey, _] : m_recoveryTargetPsns) {
    blocked.insert(qpKey);
  }
  for (const auto& [qpKey, runtime] : m_qpRuntimes) {
    if (!runtime.hasDest) {
      continue;
    }
    if (runtime.txClass == RoceTxClass::Control) {
      continue;
    }
    const uint32_t qid = ClassifyQueueId_(runtime.dest);
    if (!CanAdmit_(qid, false)) {
      blocked.insert(qpKey);
    }
  }
  return blocked;
}

void
RdmaPortEngine::EnqueuePending_(PortTxItem&& item) {
  EnsureQueueState_();
  const uint32_t qid =
      item.qid % std::max<uint32_t>(1, static_cast<uint32_t>(m_controlPending.size()));
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
  const uint32_t qid =
      item->qid % std::max<uint32_t>(1, static_cast<uint32_t>(m_controlPending.size()));
  if (!CanAdmit_(qid, item->controlClass)) {
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

  TrackSuccessfulSend_(*item);
  if (!m_txNotify.IsNull()) {
    m_txNotify(item->packet->Copy());
  }
  return true;
}

bool
RdmaPortEngine::BuildPacketForSlice_(const RoceTxSlice& slice,
                                     Ptr<Packet>* packetOut,
                                     RoceSimHeader* headerOut) {
  if (packetOut == nullptr || !m_cfg.enable) {
    return false;
  }
  if (!EnsureQpReadyToSend_(slice.qpKey) || !m_qpEngine.CanPostSend(slice.qpKey)) {
    return false;
  }

  HopCountHeader hopHeader;
  hopHeader.SetHopCount(0);
  hopHeader.SetIsFlood(false);

  ThreePHeader tpHeader;
  tpHeader.SetState(slice.state);
  tpHeader.SetTag(slice.flowKey.tag);
  tpHeader.SetSrc(slice.flowKey.src);
  tpHeader.SetDst(slice.flowKey.dst);
  tpHeader.SetDataSize(slice.totalBytes);

  Ptr<Packet> packet;
  uint16_t realPacketPayload = 0;
  const uint16_t roceWireBytes =
      static_cast<uint16_t>(RoceSimHeader::GetVisibleWireBytes(slice.opcode));

  if (slice.state == ThreePHeader::REQUEST || slice.state == ThreePHeader::ACT) {
    realPacketPayload = SafePayloadBytes(
        static_cast<int64_t>(slice.controlPacketBytes) - tpHeader.GetSerializedSize() -
        hopHeader.GetSerializedSize() - roceWireBytes);
    packet = Create<Packet>(realPacketPayload);
  } else if (slice.state == ThreePHeader::DATA) {
    if (!slice.hasFlowTagHeader) {
      return false;
    }
    realPacketPayload = SafePayloadBytes(
        static_cast<int64_t>(slice.sliceBytes) - tpHeader.GetSerializedSize() -
        hopHeader.GetSerializedSize() - slice.flowTagHeader.GetSerializedSize());
    packet = Create<Packet>(realPacketPayload);
    packet->AddHeader(slice.flowTagHeader);
  } else {
    return false;
  }

  RoceSimHeader roceHeader;
  const uint32_t encodedBytes =
      realPacketPayload > 0
          ? static_cast<uint32_t>(realPacketPayload)
          : static_cast<uint32_t>(std::min<uint64_t>(slice.totalBytes, 0xffffffffULL));
  if (!m_qpEngine.BuildSendHeader(slice.qpKey,
                                  slice.opcode,
                                  slice.userPhase,
                                  encodedBytes,
                                  slice.flowKey.tag,
                                  &roceHeader)) {
    return false;
  }

  const uint64_t remoteAddr =
      (static_cast<uint64_t>(slice.flowKey.dst) << 32) |
      static_cast<uint64_t>(slice.flowKey.tag);
  const uint32_t rkey =
      (slice.flowKey.tag * 2654435761u) ^ (slice.flowKey.src << 16) ^
      (slice.flowKey.dst & 0xffffu);
  uint8_t flags = 0;
  if (slice.opcode == RoceOpcode::RdmaWrite || slice.opcode == RoceOpcode::RdmaReadReq ||
      slice.opcode == RoceOpcode::RdmaReadResp) {
    flags |= 0x1;
  }
  if (slice.opcode == RoceOpcode::RdmaReadResp) {
    flags |= 0x2;
  }
  if (slice.opcode == RoceOpcode::Ack) {
    flags |= 0x4;
  }
  roceHeader.SetFlags(flags);
  roceHeader.SetRemoteAddr(remoteAddr);
  roceHeader.SetRkey(rkey);
  roceHeader.SetImmData(
      static_cast<uint32_t>(std::min<uint64_t>(slice.totalBytes, 0xffffffffULL)));
  if (slice.opcode == RoceOpcode::RdmaReadReq) {
    roceHeader.SetLength(
        static_cast<uint32_t>(std::min<uint64_t>(slice.totalBytes, 0xffffffffULL)));
  }

  packet->AddHeader(roceHeader);
  packet->AddHeader(tpHeader);
  packet->AddHeader(hopHeader);

  RdmaDispatchTag tag;
  tag.SetWorkId(slice.workId);
  tag.SetFlowKey(slice.flowKey);
  tag.SetState(slice.state);
  tag.SetQpn(slice.qpKey.qpn);
  tag.SetPsn(roceHeader.GetPsn());
  tag.SetOpcode(roceHeader.GetOpcode());
  tag.SetTxClass(slice.txClass);
  tag.SetTotalBytes(slice.totalBytes);
  tag.SetSliceBytes(slice.sliceBytes);
  tag.SetPacketIndex(slice.packetIndex);
  tag.SetTotalPackets(slice.totalPackets);
  tag.SetLastSlice(slice.isLastSlice);
  tag.SetRetransmission(false);
  tag.SetTransportAck(false);
  packet->AddPacketTag(tag);

  if (headerOut != nullptr) {
    *headerOut = roceHeader;
  }
  *packetOut = packet;
  return true;
}

bool
RdmaPortEngine::DispatchFromScheduler_() {
  if (!m_device || !m_txScheduler.HasPendingWork()) {
    return false;
  }

  bool sentAny = false;
  while (m_txScheduler.HasPendingWork()) {
    const std::set<RoceQpKey> blockedQps = CollectBlockedQps_();
    RoceTxSlice slice;
    if (!m_txScheduler.PickNextSlice(m_device->GetMtu(), &blockedQps, &slice)) {
      break;
    }

    Ptr<Packet> packet;
    RoceSimHeader roceHeader;
    if (!BuildPacketForSlice_(slice, &packet, &roceHeader)) {
      NS_LOG_WARN("[ROCE][PORT][BUILD_FAIL] qpn=" << slice.qpKey.qpn
                  << " tag=" << slice.flowKey.tag
                  << " src=" << slice.flowKey.src
                  << " dst=" << slice.flowKey.dst);
      continue;
    }

    PortTxItem item;
    item.packet = packet;
    item.src = m_device->GetAddress();
    item.dst = slice.dest;
    item.protocol = m_cfg.etherType;
    item.qid = ClassifyQueueId_(item.dst);
    item.controlClass = (slice.txClass == RoceTxClass::Control);

    if (!TrySendNow_(&item)) {
      EnqueuePending_(std::move(item));
      ScheduleDrain_(true);
      break;
    }
    sentAny = true;
  }
  return sentAny;
}

bool
RdmaPortEngine::BuildAckPacket_(uint32_t tag,
                                uint32_t src,
                                uint32_t dst,
                                uint32_t qpn,
                                Ptr<Packet>* packetOut,
                                RoceSimHeader* headerOut) {
  if (packetOut == nullptr || !m_cfg.enable) {
    return false;
  }

  const RoceQpKey qpKey{src, dst, qpn};
  if (!EnsureQpReadyToSend_(qpKey) || !m_qpEngine.CanPostSend(qpKey)) {
    return false;
  }

  HopCountHeader hopHeader;
  hopHeader.SetHopCount(0);
  hopHeader.SetIsFlood(false);

  ThreePHeader tpHeader;
  tpHeader.SetState(ThreePHeader::IDLE);
  tpHeader.SetTag(tag);
  tpHeader.SetSrc(src);
  tpHeader.SetDst(dst);
  tpHeader.SetDataSize(0);

  RoceSimHeader ackHeader;
  if (!m_qpEngine.BuildSendHeader(
          qpKey, RoceOpcode::Ack, RoceUserPhase::None, 0, tag, &ackHeader)) {
    return false;
  }
  ackHeader.SetFlags(0x4);
  ackHeader.SetRemoteAddr(0);
  ackHeader.SetRkey(0);
  ackHeader.SetImmData(0);
  ackHeader.SetLength(0);

  Ptr<Packet> packet = Create<Packet>(0);
  packet->AddHeader(ackHeader);
  packet->AddHeader(tpHeader);
  packet->AddHeader(hopHeader);

  RdmaDispatchTag dispatchTag;
  dispatchTag.SetFlowKey(RoceFlowKey{tag, src, dst});
  dispatchTag.SetState(ThreePHeader::IDLE);
  dispatchTag.SetQpn(qpn);
  dispatchTag.SetPsn(ackHeader.GetPsn());
  dispatchTag.SetOpcode(ackHeader.GetOpcode());
  dispatchTag.SetTxClass(RoceTxClass::Control);
  dispatchTag.SetTotalBytes(0);
  dispatchTag.SetSliceBytes(0);
  dispatchTag.SetPacketIndex(0);
  dispatchTag.SetTotalPackets(1);
  dispatchTag.SetLastSlice(true);
  dispatchTag.SetRetransmission(false);
  dispatchTag.SetTransportAck(true);
  packet->AddPacketTag(dispatchTag);

  if (headerOut != nullptr) {
    *headerOut = ackHeader;
  }
  *packetOut = packet;
  return true;
}

void
RdmaPortEngine::MaybeSendAck_(const Address& dest,
                              uint32_t tag,
                              uint32_t src,
                              uint32_t dst,
                              uint32_t qpn,
                              RoceOpcode rxOpcode) {
  if (!m_cfg.enable || !m_cfg.enableTransportAck || rxOpcode == RoceOpcode::Ack) {
    return;
  }

  Ptr<Packet> ackPacket;
  RoceSimHeader ackHeader;
  if (!BuildAckPacket_(tag, src, dst, qpn, &ackPacket, &ackHeader)) {
    NS_LOG_WARN("[ROCE][ACK][TX] failed to build ack"
                << " node=" << (m_hasNodeId ? m_nodeId : 0)
                << " tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << qpn);
    return;
  }

  const RoceQpKey qpKey{src, dst, qpn};
  if (m_cfg.logLevel == RoceLogLevel::Full) {
    NS_LOG_INFO("[ROCE][ACK][TX] tag=" << tag
                << " node=" << (m_hasNodeId ? m_nodeId : 0)
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << qpn
                << " psn=" << ackHeader.GetPsn()
                << " ackPsn=" << ackHeader.GetAckPsn()
                << " expectedRxPsn=" << m_qpEngine.GetExpectedRxPsn(qpKey)
                << " pendingRxCount=" << m_qpEngine.GetPendingRxCount(qpKey)
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }

  const Address wireDest = ToWireAddress(dest);
  PortTxItem item;
  item.packet = ackPacket;
  item.src = m_device != nullptr ? m_device->GetAddress() : Address();
  item.dst = wireDest;
  item.protocol = m_cfg.etherType;
  item.qid = ClassifyQueueId_(wireDest);
  item.controlClass = true;
  if (!TrySendNow_(&item)) {
    EnqueuePending_(std::move(item));
    ScheduleDrain_(false);
  }
}

void
RdmaPortEngine::PushCompletion_(const RoceQpKey& qpKey,
                                uint32_t psn,
                                uint32_t wrId,
                                bool success,
                                RoceCompletionKind kind) {
  QpRuntime& runtime = EnsureQpRuntime_(qpKey);
  if (runtime.completions.size() >= runtime.cqDepth && !runtime.completions.empty()) {
    const RoceCompletion& dropped = runtime.completions.front();
    if (dropped.kind == RoceCompletionKind::Send && runtime.sendCompletionCount != 0) {
      --runtime.sendCompletionCount;
    } else if (dropped.kind == RoceCompletionKind::Recv &&
               runtime.recvCompletionCount != 0) {
      --runtime.recvCompletionCount;
    }
    runtime.completions.pop_front();
    ++runtime.cqOverflowCount;
    NS_LOG_WARN("[ROCE][CQ][OVERFLOW] qpn=" << qpKey.qpn
                << " src=" << qpKey.src
                << " dst=" << qpKey.dst
                << " cqDepth=" << runtime.cqDepth
                << " overflowCount=" << runtime.cqOverflowCount);
  }

  RoceCompletion completion;
  completion.src = qpKey.src;
  completion.dst = qpKey.dst;
  completion.qpn = qpKey.qpn;
  completion.psn = psn;
  completion.wrId = wrId;
  completion.success = success;
  completion.kind = kind;
  runtime.completions.push_back(completion);
  if (kind == RoceCompletionKind::Send) {
    ++runtime.sendCompletionCount;
  } else if (kind == RoceCompletionKind::Recv) {
    ++runtime.recvCompletionCount;
  }
}

void
RdmaPortEngine::CompleteSqEntry_(const RoceQpKey& qpKey,
                                 uint64_t workId,
                                 uint32_t psn) {
  QpRuntime& runtime = EnsureQpRuntime_(qpKey);
  if (runtime.sqEntries.empty()) {
    NS_LOG_WARN("[ROCE][SQ][COMPLETE_MISS] qpn=" << qpKey.qpn
                << " src=" << qpKey.src
                << " dst=" << qpKey.dst
                << " workId=" << workId);
    return;
  }

  auto it = std::find_if(runtime.sqEntries.begin(),
                         runtime.sqEntries.end(),
                         [workId](const SqEntry& entry) { return entry.workId == workId; });
  if (it == runtime.sqEntries.end()) {
    NS_LOG_WARN("[ROCE][SQ][COMPLETE_UNKNOWN] qpn=" << qpKey.qpn
                << " src=" << qpKey.src
                << " dst=" << qpKey.dst
                << " workId=" << workId);
    return;
  }

  PushCompletion_(qpKey,
                  psn,
                  static_cast<uint32_t>(std::min<uint64_t>(workId, 0xffffffffULL)),
                  true,
                  RoceCompletionKind::Send);
  runtime.sqEntries.erase(it);
}

bool
RdmaPortEngine::AcquireRqEntry_(const RoceQpKey& qpKey,
                                uint32_t psn) {
  QpRuntime& runtime = EnsureQpRuntime_(qpKey);
  if (runtime.rqInflight.find(psn) != runtime.rqInflight.end()) {
    return true;
  }
  if (runtime.rqInflight.size() >= runtime.rqDepth) {
    return false;
  }

  RqEntry entry;
  entry.psn = psn;
  entry.wrId = psn;
  runtime.rqInflight.emplace(psn, entry);
  return true;
}

void
RdmaPortEngine::ReleaseRqEntry_(const RoceQpKey& qpKey,
                                uint32_t psn) {
  auto qpIt = m_qpRuntimes.find(qpKey);
  if (qpIt == m_qpRuntimes.end()) {
    return;
  }
  qpIt->second.rqInflight.erase(psn);
}

void
RdmaPortEngine::ReleaseQpState_(const RoceQpKey& qpKey, bool dropCompletions) {
  CancelRetransTimer_(qpKey);
  m_bufferedUpperPackets.erase(qpKey);
  m_outstandingPackets.erase(qpKey);
  m_pendingRetransmissions.erase(qpKey);
  m_recoveryTargetPsns.erase(qpKey);

  auto runtimeIt = m_qpRuntimes.find(qpKey);
  if (runtimeIt != m_qpRuntimes.end()) {
    runtimeIt->second.sqEntries.clear();
    runtimeIt->second.rqInflight.clear();
    if (dropCompletions) {
      runtimeIt->second.completions.clear();
      runtimeIt->second.sendCompletionCount = 0;
      runtimeIt->second.recvCompletionCount = 0;
    }
    m_qpRuntimes.erase(runtimeIt);
  }

  m_qpEngine.ReleaseQp(qpKey);
}

void
RdmaPortEngine::MaybeReleaseIdleTxQp_(const RoceQpKey& qpKey) {
  auto runtimeIt = m_qpRuntimes.find(qpKey);
  if (runtimeIt == m_qpRuntimes.end()) {
    return;
  }
  if (!runtimeIt->second.sqEntries.empty() || !runtimeIt->second.rqInflight.empty()) {
    return;
  }

  auto bufferedIt = m_bufferedUpperPackets.find(qpKey);
  if (bufferedIt != m_bufferedUpperPackets.end() && !bufferedIt->second.empty()) {
    return;
  }
  auto outstandingIt = m_outstandingPackets.find(qpKey);
  if (outstandingIt != m_outstandingPackets.end() && !outstandingIt->second.empty()) {
    return;
  }
  if (m_pendingRetransmissions.find(qpKey) != m_pendingRetransmissions.end() ||
      m_recoveryTargetPsns.find(qpKey) != m_recoveryTargetPsns.end() ||
      m_retransTimers.find(qpKey) != m_retransTimers.end()) {
    return;
  }

  ReleaseQpState_(qpKey, true);
}

void
RdmaPortEngine::CollectReadyBufferedPackets_(
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
    if (m_cfg.logLevel == RoceLogLevel::Full) {
      NS_LOG_INFO("[ROCE][RX][REORDER_RELEASE] node=" << (m_hasNodeId ? m_nodeId : 0)
                  << " src=" << qpKey.src
                  << " dst=" << qpKey.dst
                  << " qpn=" << qpKey.qpn
                  << " psn=" << it->first
                  << " deliverUpToPsn=" << deliverUpToPsn
                  << " remainingBuffered=" << packets.size());
    }
    PushCompletion_(qpKey,
                    it->second.transportFragmentId,
                    it->second.transportFragmentId,
                    true,
                    RoceCompletionKind::Recv);
    ReleaseRqEntry_(qpKey, it->second.rqPsn);
    out->push_back(it->second);
    packets.erase(it);
  }
  if (packets.empty()) {
    m_bufferedUpperPackets.erase(bufferedIt);
  }
}

void
RdmaPortEngine::DrainPending_() {
  EnsureQueueState_();

  const uint32_t numQueues =
      std::max<uint32_t>(1, static_cast<uint32_t>(m_controlPending.size()));
  uint32_t visited = 0;
  while (visited < numQueues) {
    const uint32_t qid = m_rrIndex % numQueues;
    m_rrIndex = (m_rrIndex + 1) % numQueues;
    ++visited;

    std::deque<PortTxItem>* queue = nullptr;
    if (!m_controlPending[qid].empty()) {
      queue = &m_controlPending[qid];
    } else if (!m_dataPending[qid].empty()) {
      queue = &m_dataPending[qid];
    } else {
      continue;
    }

    NS_ASSERT(queue != nullptr);
    if (!CanAdmit_(qid, queue->front().controlClass)) {
      continue;
    }
    const bool sent = TrySendNow_(&queue->front());
    if (!sent) {
      ScheduleDrain_(true);
      return;
    }
    queue->pop_front();
    visited = 0;
  }

  (void)DispatchFromScheduler_();

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
    if (m_cfg.logLevel != RoceLogLevel::Off) {
      NS_LOG_INFO("[ROCE][RTX][SEND] tag=" << flowKey.tag
                  << " src=" << flowKey.src
                  << " dst=" << flowKey.dst
                  << " qpn=" << qpKey.qpn
                  << " psn=" << tag.GetPsn()
                  << " retryIndex=" << (outstanding.retries + 1)
                  << " outstanding=" << packets.size()
                  << " TIME=" << Simulator::Now().GetFemtoSeconds());
    }
    outstanding.retries++;
    m_pendingRetransmissions.erase(qpKey);
  }
  if (tag.IsLastSlice() && !tag.IsRetransmission()) {
    CompleteSqEntry_(qpKey, tag.GetWorkId(), tag.GetPsn());
    if (!m_cfg.enableTransportAck) {
      MaybeReleaseIdleTxQp_(qpKey);
    }
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
  if (m_cfg.logLevel != RoceLogLevel::Off) {
    NS_LOG_INFO("[ROCE][RTO] tag=" << outstanding.flowKey.tag
                << " src=" << outstanding.flowKey.src
                << " dst=" << outstanding.flowKey.dst
                << " qpn=" << qpKey.qpn
                << " psn=" << outstanding.psn
                << " retries=" << outstanding.retries
                << " outstanding=" << mapIt->second.size()
                << " ackTimeoutNs=" << m_cfg.ackTimeoutNs
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }
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
    MaybeReleaseIdleTxQp_(qpKey);
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
  (void)packet;
  if (HasPendingWork_()) {
    ScheduleDrain_(false);
  }
}

} // namespace ns3
