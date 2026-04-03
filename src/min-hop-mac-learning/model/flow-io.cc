#include "flow-io.h"

#include "PppRoundRobinQueue.h"
#include "3PHeader.h"
#include "ns3/hop-count-header.h"
#include "pause-header.h"
#include "ns3/boolean.h"
#include "ns3/log.h"
#include "ns3/mac48-address.h"
#include "ns3/node.h"
#include "ns3/packet-socket-address.h"
#include "ns3/packet-socket-factory.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/ptr.h" // PeekPointer
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("FlowIo");
NS_OBJECT_ENSURE_REGISTERED(FlowIo);

namespace {
// Non-owning registry: avoid reference cycles/re-entrancy during global destruction.
std::unordered_map<uint32_t, FlowIo*> g_flowIoRegistryByNodeId;

struct FlowIoDropStats
{
  uint64_t bufferOverflowDrops = 0;
  uint64_t bufferOverflowDropBytes = 0;
  std::unordered_map<uint32_t, uint64_t> perNodeBufferOverflowDrops;
  bool summaryScheduled = false;
};

FlowIoDropStats g_flowIoDropStats;
constexpr uint64_t kApproxSchedulerEventBytes = 128;

void
PrintFlowIoDropSummary()
{
  NS_LOG_UNCOND("[DROP][SUMMARY][FLOWIO] bufOverflowPkts="
                << g_flowIoDropStats.bufferOverflowDrops
                << " bufOverflowBytes="
                << g_flowIoDropStats.bufferOverflowDropBytes
                << " nodesWithDrops="
                << g_flowIoDropStats.perNodeBufferOverflowDrops.size());
  for (const auto& entry : g_flowIoDropStats.perNodeBufferOverflowDrops)
  {
    NS_LOG_UNCOND("[DROP][SUMMARY][FLOWIO][NODE] node=" << entry.first
                  << " bufOverflowPkts=" << entry.second);
  }
}

void
EnsureFlowIoDropSummaryScheduled()
{
  if (!g_flowIoDropStats.summaryScheduled)
  {
    g_flowIoDropStats.summaryScheduled = true;
    Simulator::ScheduleDestroy(&PrintFlowIoDropSummary);
  }
}
}

TypeId
FlowIo::GetTypeId()
{
  static TypeId tid =
      TypeId("ns3::FlowIo")
          .SetParent<Object>()
          .SetGroupName("Applications")
          .AddAttribute("EnableTxBuffer",
                        "Enable FlowIo software tx buffering/retry when NetDevice send fails.",
                        BooleanValue(true),
                        MakeBooleanAccessor(&FlowIo::m_enableTxBuffer),
                        MakeBooleanChecker())
          .AddAttribute("TxRetryInterval",
                        "Retry interval for draining buffered packets.",
                        TimeValue(MicroSeconds(2)),
                        MakeTimeAccessor(&FlowIo::m_retryInterval),
                        MakeTimeChecker())
          .AddAttribute("MaxBufferedPackets",
                        "Maximum buffered packets in FlowIo before dropping new packets.",
                        UintegerValue(20000000),
                        MakeUintegerAccessor(&FlowIo::m_maxBufferedPackets),
                        MakeUintegerChecker<uint32_t>())
          .AddAttribute("NumTxQueues",
                        "Number of software queue ids used for local pause mapping.",
                        UintegerValue(16),
                        MakeUintegerAccessor(&FlowIo::m_numTxQueues),
                        MakeUintegerChecker<uint32_t>(1))
          .AddConstructor<FlowIo>();
  return tid;
}

void
FlowIo::PauseNodeQueue(Ptr<Node> node, uint32_t queueId)
{
  if (!node)
  {
    return;
  }
  auto it = g_flowIoRegistryByNodeId.find(node->GetId());
  if (it != g_flowIoRegistryByNodeId.end() && it->second)
  {
    it->second->PauseTxQueue(queueId);
  }
}

void
FlowIo::ResumeNodeQueue(Ptr<Node> node, uint32_t queueId)
{
  if (!node)
  {
    return;
  }
  auto it = g_flowIoRegistryByNodeId.find(node->GetId());
  if (it != g_flowIoRegistryByNodeId.end() && it->second)
  {
    it->second->ResumeTxQueue(queueId);
  }
}

FlowIo::FlowIo() = default;

FlowIo::~FlowIo()
{
  // 资源释放在 DoDispose
}

void
FlowIo::DoDispose()
{
  Stop();
  m_node = nullptr;
  m_bridge = nullptr;
  Object::DoDispose();
}

void
FlowIo::SetNode(Ptr<Node> node)
{
  m_node = node;
}

void
FlowIo::SetProtocol(uint16_t etherType)
{
  m_proto = etherType;
}

void
FlowIo::SetOutDeviceIndex(uint32_t outDevIndex)
{
  m_outDev = outDevIndex;
}

void
FlowIo::SetInnerDeviceIndex(uint32_t innerDevIndex)
{
  m_innerDev = innerDevIndex;
}

void
FlowIo::SetLocalBridge(Ptr<NetDevice> bridgeDev)
{
  m_bridge = bridgeDev;

  // 尝试在 node 已设置时，从指针推断 innerDev（更鲁棒）
  if (m_node && m_bridge)
  {
    for (uint32_t i = 0; i < m_node->GetNDevices(); ++i)
    {
      if (m_node->GetDevice(i) == m_bridge)
      {
        m_innerDev = i;
        break;
      }
    }
  }
}

void
FlowIo::SetRxBindMode(RxBindMode mode)
{
  m_bindMode = mode;
}

void
FlowIo::EnsureQueueState()
{
  if (m_numTxQueues == 0)
  {
    m_numTxQueues = 1;
  }
  if (m_pausedTxQueues.size() != m_numTxQueues)
  {
    m_pausedTxQueues.assign(m_numTxQueues, false);
  }
  if (m_externalIngressQueues.size() != m_numTxQueues)
  {
    m_externalIngressQueues.assign(m_numTxQueues, {});
  }
  if (m_externalHoldQueues.size() != m_numTxQueues)
  {
    m_externalHoldQueues.assign(m_numTxQueues, {});
  }
  if (m_externalIngressPaused.size() != m_numTxQueues)
  {
    m_externalIngressPaused.assign(m_numTxQueues, false);
  }
  if (m_externalIngressBackpressured.size() != m_numTxQueues)
  {
    m_externalIngressBackpressured.assign(m_numTxQueues, false);
  }
  if (m_externalIngressRrIndex >= m_numTxQueues)
  {
    m_externalIngressRrIndex = 0;
  }
}

uint32_t
FlowIo::ClassifyQueueId(const Address& dest) const
{
  if (m_numTxQueues == 0 || !PacketSocketAddress::IsMatchingType(dest))
  {
    return 0;
  }

  PacketSocketAddress ps = PacketSocketAddress::ConvertFrom(dest);
  Address physical = ps.GetPhysicalAddress();
  if (!Mac48Address::IsMatchingType(physical))
  {
    return 0;
  }

  Mac48Address mac = Mac48Address::ConvertFrom(physical);
  uint8_t buffer[6];
  mac.CopyTo(buffer);
  uint64_t macAsU64 = 0;
  for (int i = 0; i < 6; ++i)
  {
    macAsU64 = (macAsU64 << 8) | buffer[i];
  }
  std::hash<uint64_t> hasher;
  uint32_t finalHash = static_cast<uint32_t>(hasher(macAsU64));
  return finalHash % m_numTxQueues;
}

bool
FlowIo::IsTxQueuePaused(uint32_t queueId) const
{
  if (m_pausedTxQueues.empty())
  {
    return false;
  }
  return m_pausedTxQueues[queueId % m_pausedTxQueues.size()];
}

void
FlowIo::PauseTxQueue(uint32_t queueId)
{
  EnsureQueueState();
  uint32_t qid = queueId % m_numTxQueues;
  if (!m_pausedTxQueues[qid])
  {
    m_pausedTxQueues[qid] = true;
    NS_LOG_DEBUG("[FLOWIO][PAUSE] node=" << (m_node ? m_node->GetId() : 0)
                 << " qid=" << qid
                 << " t=" << Simulator::Now().GetSeconds() << "s");
  }
}

void
FlowIo::ResumeTxQueue(uint32_t queueId)
{
  EnsureQueueState();
  uint32_t qid = queueId % m_numTxQueues;
  if (m_pausedTxQueues[qid])
  {
    m_pausedTxQueues[qid] = false;
    NS_LOG_DEBUG("[FLOWIO][RESUME] node=" << (m_node ? m_node->GetId() : 0)
                 << " qid=" << qid
                 << " t=" << Simulator::Now().GetSeconds() << "s");
  }
  if (!m_txBuffer.empty())
  {
    ScheduleDrain();
  }
}

bool
FlowIo::IsSendPathPaused(const Address& dest, FlowSendMode mode) const
{
  if (m_numTxQueues == 0)
  {
    return false;
  }
  const uint32_t qid = ClassifyQueueId(dest) % m_numTxQueues;

  // External 路径的 pause 由 externalIngressPaused/hold 队列体现。
  if (mode == FlowSendMode::External && m_bindMode != RxBindMode::BridgeOnly)
  {
    const bool ingressPaused =
        qid < m_externalIngressPaused.size() && m_externalIngressPaused[qid];
    const bool hasHeldPackets =
        qid < m_externalHoldQueues.size() && !m_externalHoldQueues[qid].empty();
    const bool backpressured =
        qid < m_externalIngressBackpressured.size() && m_externalIngressBackpressured[qid];
    return ingressPaused || hasHeldPackets || backpressured;
  }

  return qid < m_pausedTxQueues.size() && m_pausedTxQueues[qid];
}

void
FlowIo::Start(const SocketRxCallback& cb)
{
  NS_ASSERT_MSG(m_node != nullptr, "FlowIo: node not set");
  NS_ASSERT_MSG(m_proto != 0, "FlowIo: protocol not set");
  NS_ASSERT_MSG(m_node->GetNDevices() >= 1, "FlowIo: node has no devices");
  NS_ASSERT_MSG(m_innerDev < m_node->GetNDevices(), "FlowIo: innerDevIndex out of range");

  // 常规拓扑才要求 outDev 合法；BridgeOnly 可以不设置 outDev
  if (m_bindMode == RxBindMode::BridgeAndExternal)
  {
    NS_ASSERT_MSG(m_outDev < m_node->GetNDevices(), "FlowIo: outDevIndex out of range");
  }

  m_sockRxCb = cb;
  EnsureQueueState();

  // 若未设置 bridge 指针，则尝试用 innerDevIndex 推断
  if (!m_bridge)
  {
    m_bridge = m_node->GetDevice(m_innerDev);
  }

  // 防止重复 Start
  Stop();

  std::vector<uint32_t> bindDevs;
  bindDevs.push_back(m_innerDev);

  if (m_bindMode == RxBindMode::BridgeAndExternal)
  {
    bindDevs.push_back(m_outDev);
  }

  std::sort(bindDevs.begin(), bindDevs.end());
  bindDevs.erase(std::unique(bindDevs.begin(), bindDevs.end()), bindDevs.end());

  for (uint32_t devIdx : bindDevs)
  {
    NS_ASSERT_MSG(devIdx < m_node->GetNDevices(), "FlowIo: bind devIdx out of range");

    Ptr<Socket> sock = Socket::CreateSocket(m_node, PacketSocketFactory::GetTypeId());

    PacketSocketAddress local;
    local.SetSingleDevice(devIdx);
    local.SetPhysicalAddress(m_node->GetDevice(devIdx)->GetAddress());
    local.SetProtocol(m_proto);

    sock->Bind(local);
    sock->BindToNetDevice(m_node->GetDevice(devIdx));

    // 关键：直接绑定用户原来的 HandleRead(Ptr<Socket>)
    sock->SetRecvCallback(m_sockRxCb);

    m_socks.push_back(sock);
    m_sock2dev[PeekPointer(sock)] = devIdx;
  }

  g_flowIoRegistryByNodeId[m_node->GetId()] = this;
}

void
FlowIo::Stop()
{
  if (m_drainEvent.IsRunning())
  {
    Simulator::Cancel(m_drainEvent);
  }
  if (m_externalIngressDrainEvent.IsRunning())
  {
    Simulator::Cancel(m_externalIngressDrainEvent);
  }
  m_txBuffer.clear();
  m_externalIngressQueues.clear();
  m_externalHoldQueues.clear();
  m_externalIngressPaused.clear();
  m_externalIngressBackpressured.clear();

  if (m_node)
  {
    auto it = g_flowIoRegistryByNodeId.find(m_node->GetId());
    if (it != g_flowIoRegistryByNodeId.end() && it->second == this)
    {
      g_flowIoRegistryByNodeId.erase(it);
    }
  }

  for (auto& s : m_socks)
  {
    if (s)
    {
      s->Close();
    }
  }
  m_socks.clear();
  m_sock2dev.clear();
}

FlowIo::DebugBufferStats
FlowIo::GetDebugBufferStats() const
{
  DebugBufferStats stats;
  for (const auto& item : m_txBuffer)
  {
    stats.txBufferedPackets++;
    stats.txBufferedApproxBytes += sizeof(BufferedTx);
    if (item.pkt)
    {
      const uint64_t payloadBytes = item.pkt->GetSize();
      stats.txBufferedBytes += payloadBytes;
      stats.txBufferedApproxBytes += payloadBytes;
    }
  }
  for (const auto& q : m_externalIngressQueues)
  {
    for (const auto& item : q)
    {
      stats.externalIngressPackets++;
      stats.externalIngressApproxBytes += sizeof(ExternalBufferedTx);
      if (item.pkt)
      {
        const uint64_t payloadBytes = item.pkt->GetSize();
        stats.externalIngressBytes += payloadBytes;
        stats.externalIngressApproxBytes += payloadBytes;
      }
    }
  }
  for (const auto& q : m_externalHoldQueues)
  {
    for (const auto& item : q)
    {
      stats.externalHoldPackets++;
      stats.externalHoldApproxBytes += sizeof(ExternalBufferedTx);
      if (item.pkt)
      {
        const uint64_t payloadBytes = item.pkt->GetSize();
        stats.externalHoldBytes += payloadBytes;
        stats.externalHoldApproxBytes += payloadBytes;
      }
    }
  }
  stats.totalBufferedPackets = stats.txBufferedPackets +
                               stats.externalIngressPackets +
                               stats.externalHoldPackets;
  stats.totalBufferedBytes = stats.txBufferedBytes +
                             stats.externalIngressBytes +
                             stats.externalHoldBytes;
  stats.totalBufferedApproxBytes = stats.txBufferedApproxBytes +
                                   stats.externalIngressApproxBytes +
                                   stats.externalHoldApproxBytes;
  stats.drainEventPending = m_drainEvent.IsPending() ? 1 : 0;
  stats.externalIngressDrainEventPending = m_externalIngressDrainEvent.IsPending() ? 1 : 0;
  stats.pendingEvents = stats.drainEventPending + stats.externalIngressDrainEventPending;
  stats.pendingEventApproxBytes = stats.pendingEvents * kApproxSchedulerEventBytes;
  return stats;
}

bool
FlowIo::SendOnDevice(uint32_t devIndex, const Address& dest, Ptr<Packet> pkt)
{
  NS_ASSERT_MSG(m_node != nullptr, "FlowIo: node not set");
  NS_ASSERT_MSG(pkt != nullptr, "FlowIo: null packet");
  NS_ASSERT_MSG(devIndex < m_node->GetNDevices(), "FlowIo: devIndex out of range");
  NS_ASSERT_MSG(PacketSocketAddress::IsMatchingType(dest), "FlowIo: destination address type mismatch");

  Ptr<Socket> sock = nullptr;
  for (auto& s : m_socks)
  {
    auto it = m_sock2dev.find(PeekPointer(s));
    if (it != m_sock2dev.end() && it->second == devIndex)
    {
      sock = s;
      break;
    }
  }
  NS_ASSERT_MSG(sock != nullptr, "FlowIo: no socket bound to requested devIndex");

  PacketSocketAddress ps = PacketSocketAddress::ConvertFrom(dest);
  ps.SetSingleDevice(devIndex);
  ps.SetProtocol(m_proto);

  // Keep caller packet immutable across retries (NetDevice may mutate headers).
  int ret = sock->SendTo(pkt->Copy(), 0, ps);
  return (ret >= 0);
}

bool
FlowIo::IsHighPriorityControlPacket(Ptr<Packet> pkt) const
{
  if (pkt == nullptr)
  {
    return false;
  }

  Ptr<Packet> copy = pkt->Copy();
  HopCountHeader hopHeader;
  ThreePHeader tpHeader;
  if (copy->GetSize() < hopHeader.GetSerializedSize() + tpHeader.GetSerializedSize())
  {
    return false;
  }

  copy->RemoveHeader(hopHeader);
  copy->RemoveHeader(tpHeader);
  return tpHeader.GetState() == ThreePHeader::REQUEST ||
         tpHeader.GetState() == ThreePHeader::ACT ||
         tpHeader.GetState() == ThreePHeader::IDLE;
}

void
FlowIo::EnqueueBuffered(uint32_t devIndex, uint32_t queueId, const Address& dest, Ptr<Packet> pkt)
{
  EnsureQueueState();
  uint32_t qid = queueId % m_numTxQueues;
  const bool highPriority = IsHighPriorityControlPacket(pkt);
  if (m_txBuffer.size() >= m_maxBufferedPackets)
  {
    EnsureFlowIoDropSummaryScheduled();
    ++g_flowIoDropStats.bufferOverflowDrops;
    g_flowIoDropStats.bufferOverflowDropBytes += pkt->GetSize();
    uint32_t nodeId = m_node ? m_node->GetId() : UINT32_MAX;
    uint64_t nodeDrops = ++g_flowIoDropStats.perNodeBufferOverflowDrops[nodeId];

    static uint64_t sOverflowLogCount = 0;
    if (sOverflowLogCount < 20 || (sOverflowLogCount % 1000) == 0)
    {
      NS_LOG_UNCOND("[DROP][FLOWIO][BUF_OVERFLOW] node=" << nodeId
                    << " dev=" << devIndex
                    << " qid=" << qid
                    << " totalBuf=" << m_txBuffer.size()
                    << " maxBuf=" << m_maxBufferedPackets
                    << " pktSize=" << pkt->GetSize()
                    << " totalDropPkts=" << g_flowIoDropStats.bufferOverflowDrops
                    << " totalDropBytes=" << g_flowIoDropStats.bufferOverflowDropBytes
                    << " nodeDropPkts=" << nodeDrops);
    }
    ++sOverflowLogCount;
    return;
  }

  BufferedTx item{devIndex, qid, dest, pkt->Copy(), highPriority};
  if (highPriority)
  {
    m_txBuffer.push_front(std::move(item));
  }
  else
  {
    m_txBuffer.push_back(std::move(item));
  }

  static uint64_t sEnqueueLogCount = 0;
  if (sEnqueueLogCount < 200 || (sEnqueueLogCount % 10000) == 0)
  {
    NS_LOG_INFO("[FLOWIO][BUF][ENQ] dev=" << devIndex
                << " qid=" << qid
                << " size=" << pkt->GetSize()
                << " totalBuf=" << m_txBuffer.size());
  }
  ++sEnqueueLogCount;
}

uint32_t
FlowIo::GetExternalIngressMaxPacketsPerQueue() const
{
  if (!m_node || m_outDev >= m_node->GetNDevices())
  {
    return 100;
  }

  Ptr<PointToPointNetDevice> p2p = DynamicCast<PointToPointNetDevice>(m_node->GetDevice(m_outDev));
  if (!p2p)
  {
    return 100;
  }

  Ptr<Queue<Packet>> queue = p2p->GetQueue();
  if (!queue)
  {
    return 100;
  }

  Ptr<PppRoundRobinQueue<Packet>> rrQueue = DynamicCast<PppRoundRobinQueue<Packet>>(queue);
  if (rrQueue && rrQueue->GetMaxPacketsPerQueue() > 0)
  {
    return rrQueue->GetMaxPacketsPerQueue();
  }

  return 100;
}

bool
FlowIo::GetExternalEgressQueueState(uint32_t queueId,
                                    uint32_t& queueLen,
                                    uint32_t& maxPerQueue) const
{
  queueLen = 0;
  maxPerQueue = 0;

  if (!m_node || m_outDev >= m_node->GetNDevices())
  {
    return false;
  }

  Ptr<PointToPointNetDevice> p2p = DynamicCast<PointToPointNetDevice>(m_node->GetDevice(m_outDev));
  if (!p2p)
  {
    return false;
  }

  Ptr<Queue<Packet>> queue = p2p->GetQueue();
  if (!queue)
  {
    return false;
  }

  Ptr<PppRoundRobinQueue<Packet>> rrQueue = DynamicCast<PppRoundRobinQueue<Packet>>(queue);
  if (!rrQueue)
  {
    return false;
  }

  maxPerQueue = rrQueue->GetMaxPacketsPerQueue();
  if (maxPerQueue == 0)
  {
    return false;
  }

  const uint32_t numQueues = rrQueue->GetNumQueues();
  if (numQueues == 0)
  {
    return false;
  }

  queueLen = rrQueue->GetSubQueueLength(queueId % numQueues);
  return true;
}

uint32_t
FlowIo::GetExternalHoldPacketCount() const
{
  uint32_t total = 0;
  for (const auto& q : m_externalHoldQueues)
  {
    total += static_cast<uint32_t>(q.size());
  }
  return total;
}

bool
FlowIo::HasPendingExternalIngressWork() const
{
  for (const auto& q : m_externalIngressQueues)
  {
    if (!q.empty())
    {
      return true;
    }
  }
  for (const auto& q : m_externalHoldQueues)
  {
    if (!q.empty())
    {
      return true;
    }
  }
  return false;
}

bool
FlowIo::RefreshExternalIngressPauseState(uint32_t queueId, bool anticipateEnqueue)
{
  EnsureQueueState();
  if (queueId >= m_externalIngressPaused.size())
  {
    return false;
  }

  uint32_t queueLen = 0;
  uint32_t maxPerQueue = 0;
  if (!GetExternalEgressQueueState(queueId, queueLen, maxPerQueue))
  {
    return m_externalIngressPaused[queueId];
  }

  if (anticipateEnqueue)
  {
    ++queueLen;
  }

  const double ratio = static_cast<double>(queueLen) / static_cast<double>(maxPerQueue);
  if (!m_externalIngressPaused[queueId] && ratio >= g_pfcHighWatermark)
  {
    m_externalIngressPaused[queueId] = true;
    if (queueId < m_externalIngressBackpressured.size())
    {
      m_externalIngressBackpressured[queueId] = true;
    }
    if (IsPfcFullLogEnabled())
    {
      NS_LOG_UNCOND("[PFC][THRESH][EXTERNAL][PAUSE] node="
                    << (m_node ? m_node->GetId() : 0)
                    << " qid=" << queueId
                    << " rrqLen=" << queueLen
                    << " maxPerQueue=" << maxPerQueue
                    << " ratio=" << ratio);
    }
  }
  else if (m_externalIngressPaused[queueId] && ratio <= g_pfcLowWatermark)
  {
    m_externalIngressPaused[queueId] = false;
    if (queueId < m_externalIngressBackpressured.size() &&
        m_externalIngressQueues[queueId].empty() &&
        m_externalHoldQueues[queueId].empty())
    {
      m_externalIngressBackpressured[queueId] = false;
    }
    if (IsPfcFullLogEnabled())
    {
      NS_LOG_UNCOND("[PFC][THRESH][EXTERNAL][RESUME] node="
                    << (m_node ? m_node->GetId() : 0)
                    << " qid=" << queueId
                    << " rrqLen=" << queueLen
                    << " maxPerQueue=" << maxPerQueue
                    << " ratio=" << ratio);
    }
  }

  return m_externalIngressPaused[queueId];
}

void
FlowIo::EnqueueExternalIngress(uint32_t queueId, const Address& dest, Ptr<Packet> pkt)
{
  EnsureQueueState();
  const uint32_t qid = queueId % m_numTxQueues;
  const uint32_t maxPerQueue = GetExternalIngressMaxPacketsPerQueue();

  if (m_externalIngressQueues[qid].size() >= maxPerQueue)
  {
    if (qid < m_externalIngressBackpressured.size())
    {
      m_externalIngressBackpressured[qid] = true;
    }
    ++m_externalIngressDrops;
    m_externalIngressDropBytes += pkt->GetSize();
    static uint64_t sExternalIngressDropLogCount = 0;
    if (sExternalIngressDropLogCount < 20 || (sExternalIngressDropLogCount % 1000) == 0)
    {
      NS_LOG_UNCOND("[DROP][FLOWIO][EXT_INGRESS_FULL] node="
                    << (m_node ? m_node->GetId() : 0)
                    << " dev=" << m_outDev
                    << " qid=" << qid
                    << " len=" << m_externalIngressQueues[qid].size()
                    << " maxPerQueue=" << maxPerQueue
                    << " pktSize=" << pkt->GetSize()
                    << " totalDrops=" << m_externalIngressDrops
                    << " totalDropBytes=" << m_externalIngressDropBytes);
    }
    ++sExternalIngressDropLogCount;
    return;
  }

  m_externalIngressQueues[qid].push_back({qid, dest, pkt->Copy()});
  const double backlogRatio =
      maxPerQueue > 0
          ? static_cast<double>(m_externalIngressQueues[qid].size()) /
                static_cast<double>(maxPerQueue)
          : 0.0;
  if (qid < m_externalIngressBackpressured.size() && backlogRatio >= g_pfcHighWatermark)
  {
    m_externalIngressBackpressured[qid] = true;
  }
  (void)RefreshExternalIngressPauseState(qid, false);
}

void
FlowIo::EnqueueExternalHold(uint32_t queueId, const Address& dest, Ptr<Packet> pkt)
{
  EnsureQueueState();
  const uint32_t qid = queueId % m_numTxQueues;

  if (GetExternalHoldPacketCount() >= m_maxBufferedPackets)
  {
    EnsureFlowIoDropSummaryScheduled();
    ++g_flowIoDropStats.bufferOverflowDrops;
    g_flowIoDropStats.bufferOverflowDropBytes += pkt->GetSize();
    ++m_externalHoldDrops;
    m_externalHoldDropBytes += pkt->GetSize();
    static uint64_t sExternalHoldDropLogCount = 0;
    if (sExternalHoldDropLogCount < 20 || (sExternalHoldDropLogCount % 1000) == 0)
    {
      NS_LOG_UNCOND("[DROP][FLOWIO][EXT_HOLD_OVERFLOW] node="
                    << (m_node ? m_node->GetId() : 0)
                    << " dev=" << m_outDev
                    << " qid=" << qid
                    << " totalHold=" << GetExternalHoldPacketCount()
                    << " maxHold=" << m_maxBufferedPackets
                    << " pktSize=" << pkt->GetSize()
                    << " totalDrops=" << m_externalHoldDrops
                    << " totalDropBytes=" << m_externalHoldDropBytes);
    }
    ++sExternalHoldDropLogCount;
    return;
  }

  m_externalHoldQueues[qid].push_back({qid, dest, pkt->Copy()});
}

void
FlowIo::TryRefillExternalIngress(uint32_t queueId)
{
  EnsureQueueState();
  const uint32_t qid = queueId % m_numTxQueues;
  if (RefreshExternalIngressPauseState(qid, false) || m_externalHoldQueues[qid].empty())
  {
    return;
  }

  const uint32_t maxPerQueue = GetExternalIngressMaxPacketsPerQueue();
  if (m_externalIngressQueues[qid].size() >= maxPerQueue)
  {
    return;
  }

  ExternalBufferedTx item = std::move(m_externalHoldQueues[qid].front());
  m_externalHoldQueues[qid].pop_front();
  m_externalIngressQueues[qid].push_back(std::move(item));
  (void)RefreshExternalIngressPauseState(qid, false);
}

void
FlowIo::ScheduleDrain()
{
  if (!m_drainEvent.IsRunning())
  {
    m_drainEvent = Simulator::Schedule(m_retryInterval, &FlowIo::DrainBuffered, this);
  }
}

void
FlowIo::ScheduleExternalIngressDrain(bool delayed)
{
  if (m_externalIngressDrainEvent.IsRunning())
  {
    if (!delayed)
    {
      Simulator::Cancel(m_externalIngressDrainEvent);
      m_externalIngressDrainEvent = Simulator::ScheduleNow(&FlowIo::DrainExternalIngress, this);
    }
    return;
  }

  if (delayed)
  {
    m_externalIngressDrainEvent = Simulator::Schedule(m_retryInterval, &FlowIo::DrainExternalIngress, this);
  }
  else
  {
    m_externalIngressDrainEvent = Simulator::ScheduleNow(&FlowIo::DrainExternalIngress, this);
  }
}

void
FlowIo::DrainExternalIngress()
{
  EnsureQueueState();
  if (!HasPendingExternalIngressWork() || m_numTxQueues == 0)
  {
    return;
  }

  bool blocked = false;
  uint32_t budget = 4096;
  uint32_t visited = 0;
  uint32_t sent = 0;

  while (budget-- > 0 && visited < m_numTxQueues)
  {
    const uint32_t qid = m_externalIngressRrIndex % m_numTxQueues;
    m_externalIngressRrIndex = (m_externalIngressRrIndex + 1) % m_numTxQueues;
    ++visited;

    TryRefillExternalIngress(qid);
    if (m_externalIngressQueues[qid].empty())
    {
      continue;
    }

    if (RefreshExternalIngressPauseState(qid, true))
    {
      blocked = true;
      continue;
    }

    ExternalBufferedTx item = m_externalIngressQueues[qid].front();
    if (!SendOnDevice(m_outDev, item.dest, item.pkt))
    {
      blocked = true;
      if (qid < m_externalIngressBackpressured.size())
      {
        m_externalIngressBackpressured[qid] = true;
      }
      static uint64_t sExtBlockedLogCount = 0;
      if (sExtBlockedLogCount < 200 || (sExtBlockedLogCount % 10000) == 0)
      {
        NS_LOG_INFO("[FLOWIO][EXTQ][BLOCKED] node=" << (m_node ? m_node->GetId() : 0)
                    << " dev=" << m_outDev
                    << " qid=" << qid
                    << " ingressLen=" << m_externalIngressQueues[qid].size()
                    << " holdLen=" << m_externalHoldQueues[qid].size()
                    << " retryInUs=" << m_retryInterval.GetMicroSeconds());
      }
      ++sExtBlockedLogCount;
      break;
    }

    m_externalIngressQueues[qid].pop_front();
    ++sent;
    (void)RefreshExternalIngressPauseState(qid, false);
    TryRefillExternalIngress(qid);
    if (qid < m_externalIngressBackpressured.size() &&
        !m_externalIngressPaused[qid] &&
        m_externalIngressQueues[qid].empty() &&
        m_externalHoldQueues[qid].empty())
    {
      m_externalIngressBackpressured[qid] = false;
    }
    visited = 0;
  }

  if (sent > 0)
  {
    static uint64_t sExtDrainLogCount = 0;
    if (sExtDrainLogCount < 200 || (sExtDrainLogCount % 10000) == 0)
    {
      NS_LOG_INFO("[FLOWIO][EXTQ][DRAIN] sent=" << sent
                  << " pending=" << HasPendingExternalIngressWork());
    }
    ++sExtDrainLogCount;
  }

  if (HasPendingExternalIngressWork())
  {
    ScheduleExternalIngressDrain(blocked);
  }
}

void
FlowIo::DrainBuffered()
{
  EnsureQueueState();
  if (m_txBuffer.empty() || m_numTxQueues == 0)
  {
    return;
  }

  uint32_t budget = 4096;
  uint32_t sent = 0;
  uint32_t rotateBudget = static_cast<uint32_t>(m_txBuffer.size());

  while (!m_txBuffer.empty() && budget-- > 0 && rotateBudget > 0)
  {
    BufferedTx item = m_txBuffer.front();
    m_txBuffer.pop_front();

    if (IsTxQueuePaused(item.queueId))
    {
      m_txBuffer.push_back(std::move(item));
      --rotateBudget;
      continue;
    }

    if (!SendOnDevice(item.devIndex, item.dest, item.pkt))
    {
      static uint64_t sDrainBlockedLogCount = 0;
      if (sDrainBlockedLogCount < 200 || (sDrainBlockedLogCount % 10000) == 0)
      {
        NS_LOG_INFO("[FLOWIO][BUF][BLOCKED] dev=" << item.devIndex
                    << " qid=" << item.queueId
                    << " totalBuf=" << m_txBuffer.size() + 1
                    << " retryInUs=" << m_retryInterval.GetMicroSeconds());
      }
      ++sDrainBlockedLogCount;
      m_txBuffer.push_front(std::move(item));
      break;
    }

    ++sent;
    rotateBudget = static_cast<uint32_t>(m_txBuffer.size());
  }

  if (sent > 0)
  {
    static uint64_t sDrainLogCount = 0;
    if (sDrainLogCount < 200 || (sDrainLogCount % 10000) == 0)
    {
      NS_LOG_INFO("[FLOWIO][BUF][DRAIN] sent=" << sent
                  << " remain=" << m_txBuffer.size());
    }
    ++sDrainLogCount;
  }

  if (!m_txBuffer.empty())
  {
    ScheduleDrain();
  }
}

void
FlowIo::Send(const Address& dest,
             Ptr<Packet> pkt,
             FlowSendMode mode,
             uint32_t rxInDevIndexForLoopback)
{
  NS_ASSERT_MSG(m_node != nullptr, "FlowIo: node not set");
  NS_ASSERT_MSG(pkt != nullptr, "FlowIo: null packet");

  uint32_t dev = m_innerDev;

  // BridgeOnly：没有独立外部口，External 不应被调用（否则说明上层逻辑配置错）
  if (m_bindMode == RxBindMode::BridgeOnly)
  {
    if (mode == FlowSendMode::External)
    {
      NS_ASSERT_MSG(false, "FlowIo: External send requested but RxBindMode=BridgeOnly (no external port)");
    }
    dev = m_innerDev;
  }
  else
  {
    // BridgeAndExternal：正常三模式
    switch (mode)
    {
    case FlowSendMode::External:
      dev = m_outDev;
      break;
    case FlowSendMode::Internal:
      dev = m_innerDev;
      break;
    case FlowSendMode::Loopback:
      NS_ASSERT_MSG(rxInDevIndexForLoopback != UINT32_MAX,
                    "FlowIo: Loopback requires rxInDevIndexForLoopback");
      dev = (rxInDevIndexForLoopback == m_outDev) ? m_outDev : m_innerDev;
      break;
    default:
      NS_ASSERT_MSG(false, "FlowIo: unknown FlowSendMode");
    }
  }

  EnsureQueueState();
  uint32_t qid = ClassifyQueueId(dest);

  if (mode == FlowSendMode::External && m_bindMode != RxBindMode::BridgeOnly)
  {
    if (m_externalIngressPaused[qid] || !m_externalHoldQueues[qid].empty())
    {
      EnqueueExternalHold(qid, dest, pkt);
    }
    else
    {
      EnqueueExternalIngress(qid, dest, pkt);
    }
    ScheduleExternalIngressDrain(false);
    return;
  }

  if (!m_enableTxBuffer)
  {
    (void)SendOnDevice(dev, dest, pkt);
    return;
  }

  if (IsTxQueuePaused(qid) || !m_txBuffer.empty())
  {
    EnqueueBuffered(dev, qid, dest, pkt);
    ScheduleDrain();
    return;
  }

  if (!SendOnDevice(dev, dest, pkt))
  {
    NS_LOG_INFO("[FLOWIO][TXFAIL] dev=" << dev
                << " qid=" << qid
                << " size=" << pkt->GetSize()
                << " action=buffer");
    EnqueueBuffered(dev, qid, dest, pkt);
    ScheduleDrain();
  }
}

} // namespace ns3
