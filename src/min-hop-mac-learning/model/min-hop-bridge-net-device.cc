#include "min-hop-bridge-net-device.h"
#include "flow-io.h"
#include "hop-count-header.h"
#include "pause-header.h"

#include "ns3/boolean.h"
#include "ns3/bridge-channel.h"
#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/queue.h"
#include "ns3/point-to-point-net-device.h"
#include "PppRoundRobinQueue.h"
#include <algorithm>
#include <sstream>
#include <chrono>
#include <mutex>
#include <iostream>
#include <iomanip>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("MinHopBridgeNetDevice");
NS_OBJECT_ENSURE_REGISTERED(MinHopBridgeNetDevice);

namespace
{
struct FloodWindowSummary
{
    bool seen = false;
    bool printed = false;
    Time startSim = Time(0);
    Time endSim = Time(0);
    std::chrono::steady_clock::time_point startWall;
    std::chrono::steady_clock::time_point endWall;
};

FloodWindowSummary g_floodWindowSummary;
std::mutex g_floodWindowSummaryMutex;

void
RecordFloodWindowTouch()
{
    std::lock_guard<std::mutex> guard(g_floodWindowSummaryMutex);
    Time nowSim = Simulator::Now();
    auto nowWall = std::chrono::steady_clock::now();
    if (!g_floodWindowSummary.seen)
    {
        g_floodWindowSummary.seen = true;
        g_floodWindowSummary.startSim = nowSim;
        g_floodWindowSummary.endSim = nowSim;
        g_floodWindowSummary.startWall = nowWall;
        g_floodWindowSummary.endWall = nowWall;
        return;
    }
    g_floodWindowSummary.endSim = nowSim;
    g_floodWindowSummary.endWall = nowWall;
}

void
TryPrintFloodWindowSummaryOnce(bool printToStdout)
{
    std::lock_guard<std::mutex> guard(g_floodWindowSummaryMutex);
    if (!g_floodWindowSummary.seen)
    {
        if (printToStdout)
        {
            std::cout << "[FLOOD][WINDOW][GLOBAL] status=no_data_yet (unit: sim_time=ns, wall_time=ns/ms)\n";
        }
        return;
    }
    if (g_floodWindowSummary.printed)
    {
        return;
    }
    g_floodWindowSummary.printed = true;
    int64_t simDurationNs = (g_floodWindowSummary.endSim - g_floodWindowSummary.startSim).GetNanoSeconds();
    int64_t wallDurationNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 g_floodWindowSummary.endWall - g_floodWindowSummary.startWall)
                                 .count();
    double wallDurationS = static_cast<double>(wallDurationNs) / 1e9;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3)
        << "[FLOOD][WINDOW][GLOBAL]"
        << " startSim=" << g_floodWindowSummary.startSim.GetNanoSeconds() << " ns"
        << " endSim=" << g_floodWindowSummary.endSim.GetNanoSeconds() << " ns"
        << " simDuration=" << simDurationNs << " ns"
        << " wallDuration=" << wallDurationNs << " ns"
        << " (" << wallDurationS << " s)";
    NS_LOG_UNCOND(oss.str());
    if (printToStdout)
    {
        std::cout << oss.str() << std::endl;
    }
}
} // namespace

void
MinHopBridgeNetDevice::PrintFloodWindowSummary(bool printToStdout)
{
    TryPrintFloodWindowSummaryOnce(printToStdout);
}

TypeId
MinHopBridgeNetDevice::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::MinHopBridgeNetDevice")
            .SetParent<NetDevice>()
            .SetGroupName("Bridge")
            .AddConstructor<MinHopBridgeNetDevice>()
            .AddAttribute("Mtu",
                          "The MAC-level Maximum Transmission Unit",
                          UintegerValue(1500),
                          MakeUintegerAccessor(&MinHopBridgeNetDevice::SetMtu,
                                             &MinHopBridgeNetDevice::GetMtu),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("EnableLearning",
                          "Enable the learning mode of the Learning Bridge",
                          BooleanValue(true),
                          MakeBooleanAccessor(&MinHopBridgeNetDevice::m_enableLearning),
                          MakeBooleanChecker())
            .AddAttribute("PfcHighWatermark",
                          "Pause threshold ratio for ingress/egress queue occupancy.",
                          DoubleValue(0.8),
                          MakeDoubleAccessor(&MinHopBridgeNetDevice::m_highWatermark),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("PfcLowWatermark",
                          "Resume threshold ratio for ingress/egress queue occupancy.",
                          DoubleValue(0.2),
                          MakeDoubleAccessor(&MinHopBridgeNetDevice::m_lowWatermark),
                          MakeDoubleChecker<double>(0.0, 1.0))
            .AddAttribute("ExpirationTime",
                          "Time it takes for learned MAC state entry to expire.",
                          TimeValue(Seconds(99999)),
                          MakeTimeAccessor(&MinHopBridgeNetDevice::m_expirationTime),
                          MakeTimeChecker());
    return tid;
}

MinHopBridgeNetDevice::MinHopBridgeNetDevice()
    : m_node(nullptr),
      m_ifIndex(0),
      m_mtu(1500),
      m_expirationTime(Seconds(99999)),
      m_enableLearning(true),
      m_highWatermark(0.8),
      m_lowWatermark(0.2),
      m_ingressQueueDrops(0),
      m_ingressQueueDropBytes(0),
      m_forwardSendFailDrops(0),
      m_localSendFailDrops(0)
{
    NS_LOG_FUNCTION_NOARGS();
    m_channel = CreateObject<BridgeChannel>();
    is_set_address = false;
    // m_switchDelay = Time("10us");
    m_switchDelay = Time("10us");
}

MinHopBridgeNetDevice::~MinHopBridgeNetDevice()
{
    NS_LOG_FUNCTION_NOARGS();
}

void
MinHopBridgeNetDevice::DoDispose()
{
    NS_LOG_FUNCTION_NOARGS();
    uint64_t ingressRemain = 0;
    for (auto& entry : m_ingressQueues)
    {
        if (entry.second)
        {
            ingressRemain += entry.second->GetNPackets();
            entry.second = nullptr;
        }
    }
    if (m_ingressQueueDrops > 0 || ingressRemain > 0)
    {
        Mac48Address bridgeMac = Mac48Address::IsMatchingType(m_address)
                                     ? Mac48Address::ConvertFrom(m_address)
                                     : Mac48Address("00:00:00:00:00:00");
        NS_LOG_UNCOND("[DROP][SUMMARY][BRIDGE][INGRESS] bridge="
                      << bridgeMac
                      << " ingressDrops=" << m_ingressQueueDrops
                      << " ingressDropBytes=" << m_ingressQueueDropBytes
                      << " ingressRemain=" << ingressRemain);
    }
    m_ingressQueues.clear();
    for (auto& retryEvent : m_ingressRetryEvents)
    {
        if (retryEvent.second.IsPending())
        {
            Simulator::Cancel(retryEvent.second);
        }
    }
    m_ingressRetryEvents.clear();
    m_pausedIngressQueueIds.clear();
    m_ingressPauseUpstreamQueueIds.clear();
    m_egressQueueCongested.clear();
    if (m_forwardSendFailDrops > 0 || m_localSendFailDrops > 0)
    {
        Mac48Address bridgeMac = Mac48Address::IsMatchingType(m_address)
                                     ? Mac48Address::ConvertFrom(m_address)
                                     : Mac48Address("00:00:00:00:00:00");
        NS_LOG_UNCOND("[DROP][SUMMARY][BRIDGE] bridge="
                      << bridgeMac
                      << " fwdSendFailDrops=" << m_forwardSendFailDrops
                      << " localSendFailDrops=" << m_localSendFailDrops);
    }
    TryPrintFloodWindowSummaryOnce(false);
    for (auto iter = m_ports.begin(); iter != m_ports.end(); iter++)
    {
        *iter = nullptr;
    }
    m_ports.clear();
    m_channel = nullptr;
    m_node = nullptr;
    NetDevice::DoDispose();
}

void MinHopBridgeNetDevice::set_maxjump(int jump){
    maxjump=jump;
}
void MinHopBridgeNetDevice::hostsend(Ptr<NetDevice> incomingPort,Ptr<const Packet> packet,uint16_t protocol,
                    const Address& src,const Address& dst){
    EnqueueIngressPacket(incomingPort, packet, protocol, src, dst, PacketType::PACKET_OTHERHOST);
}
void
MinHopBridgeNetDevice::ReceiveFromDevice(Ptr<NetDevice> incomingPort,
                                         Ptr<const Packet> packet,
                                         uint16_t protocol,
                                         const Address& src,
                                         const Address& dst,
                                         PacketType packetType)
{
    EnqueueIngressPacket(incomingPort, packet, protocol, src, dst, packetType);
}

bool
MinHopBridgeNetDevice::EnqueueIngressPacket(Ptr<NetDevice> incomingPort,
                                            Ptr<const Packet> packet,
                                            uint16_t protocol,
                                            const Address& src,
                                            const Address& dst,
                                            PacketType packetType,
                                            bool addSwitchDelay)
{
    auto qIt = m_ingressQueues.find(incomingPort);
    if (qIt == m_ingressQueues.end() || !qIt->second)
    {
        ++m_ingressQueueDrops;
        m_ingressQueueDropBytes += packet ? packet->GetSize() : 0;
        static uint64_t sIngressNoQueueDropLogCount = 0;
        std::ostringstream ingressPortDesc;
        if (incomingPort)
        {
            ingressPortDesc << Mac48Address::ConvertFrom(incomingPort->GetAddress());
        }
        else
        {
            ingressPortDesc << "LOCAL";
        }
        if (sIngressNoQueueDropLogCount < 20 || (sIngressNoQueueDropLogCount % 1000) == 0)
        {
            NS_LOG_UNCOND("[DROP][BRIDGE][INGRESS_QUEUE_MISSING] bridge="
                          << Mac48Address::ConvertFrom(m_address)
                          << " inPort=" << ingressPortDesc.str()
                          << " src=" << Mac48Address::ConvertFrom(src)
                          << " dst=" << Mac48Address::ConvertFrom(dst)
                          << " proto=0x" << std::hex << protocol << std::dec
                          << " pktSize=" << (packet ? packet->GetSize() : 0)
                          << " totalDrops=" << m_ingressQueueDrops);
        }
        ++sIngressNoQueueDropLogCount;
        return false;
    }

    Ptr<Packet> ingressPacket = packet->Copy();
    PppHeader ingressHeader;
    ingressHeader.SetProtocol(protocol);
    ingressHeader.SetSrcMac(src);
    ingressHeader.SetDstMac(dst);
    ingressPacket->AddHeader(ingressHeader);

    if (!qIt->second->Enqueue(ingressPacket))
    {
        ++m_ingressQueueDrops;
        m_ingressQueueDropBytes += ingressPacket->GetSize();
        static uint64_t sIngressDropLogCount = 0;
        std::ostringstream ingressPortDesc;
        if (incomingPort)
        {
            ingressPortDesc << Mac48Address::ConvertFrom(incomingPort->GetAddress());
        }
        else
        {
            ingressPortDesc << "LOCAL";
        }
        if (sIngressDropLogCount < 20 || (sIngressDropLogCount % 1000) == 0)
        {
            NS_LOG_UNCOND("[DROP][BRIDGE][INGRESS_QUEUE_FULL] bridge="
                          << Mac48Address::ConvertFrom(m_address)
                          << " inPort=" << ingressPortDesc.str()
                          << " src=" << Mac48Address::ConvertFrom(src)
                          << " dst=" << Mac48Address::ConvertFrom(dst)
                          << " proto=0x" << std::hex << protocol << std::dec
                          << " pktSize=" << ingressPacket->GetSize()
                          << " totalDrops=" << m_ingressQueueDrops);
        }
        ++sIngressDropLogCount;
        return false;
    }

    if (Mac48Address::IsMatchingType(dst))
    {
        UpdateIngressPauseStateOnEnqueue(incomingPort, Mac48Address::ConvertFrom(dst));
    }

    auto& ev = m_ingressRetryEvents[incomingPort];
    if (!ev.IsPending())
    {
        if (addSwitchDelay && m_switchDelay.IsStrictlyPositive())
        {
            ev = Simulator::Schedule(m_switchDelay,
                                     &MinHopBridgeNetDevice::DrainIngressQueue,
                                     this,
                                     incomingPort);
        }
        else
        {
            ev = Simulator::ScheduleNow(&MinHopBridgeNetDevice::DrainIngressQueue,
                                        this,
                                        incomingPort);
        }
    }
    return true;
}

void
MinHopBridgeNetDevice::ScheduleIngressDrainRetry(Ptr<NetDevice> incomingPort)
{
    auto& ev = m_ingressRetryEvents[incomingPort];
    if (!ev.IsPending())
    {
        Time retryDelay = (m_switchDelay.IsStrictlyPositive() ? m_switchDelay : MicroSeconds(1));
        ev = Simulator::Schedule(retryDelay,
                                 &MinHopBridgeNetDevice::DrainIngressQueue,
                                 this,
                                 incomingPort);
    }
}

bool
MinHopBridgeNetDevice::RefreshEgressQueueCongestionStateByQid(Ptr<NetDevice> egressPort,
                                                               uint32_t queueId,
                                                               bool anticipateEnqueue)
{
    Ptr<PointToPointNetDevice> egressP2p = DynamicCast<PointToPointNetDevice>(egressPort);
    Ptr<PppRoundRobinQueue<Packet>> egressQueue =
        egressP2p ? DynamicCast<PppRoundRobinQueue<Packet>>(egressP2p->GetQueue()) : nullptr;
    if (!egressQueue)
    {
        return false;
    }

    const uint32_t maxPerQueue = egressQueue->GetMaxPacketsPerQueue();
    if (maxPerQueue == 0)
    {
        return false;
    }

    uint32_t qLen = egressQueue->GetSubQueueLength(queueId);
    if (anticipateEnqueue)
    {
        ++qLen;
    }
    double ratio = static_cast<double>(qLen) / static_cast<double>(maxPerQueue);

    bool& congested = m_egressQueueCongested[egressPort][queueId];
    if (!congested && ratio >= m_highWatermark)
    {
        congested = true;
    }
    else if (congested && ratio <= m_lowWatermark)
    {
        congested = false;
    }
    return congested;
}

bool
MinHopBridgeNetDevice::IsEgressQueueCongested(Ptr<NetDevice> egressPort,
                                              Mac48Address dst,
                                              bool anticipateEnqueue)
{
    Ptr<PointToPointNetDevice> egressP2p = DynamicCast<PointToPointNetDevice>(egressPort);
    Ptr<PppRoundRobinQueue<Packet>> egressQueue =
        egressP2p ? DynamicCast<PppRoundRobinQueue<Packet>>(egressP2p->GetQueue()) : nullptr;
    if (!egressQueue)
    {
        return false;
    }

    uint32_t queueId = egressQueue->GetQueueIndexByDst(dst);
    return RefreshEgressQueueCongestionStateByQid(egressPort, queueId, anticipateEnqueue);
}

void
MinHopBridgeNetDevice::UpdateIngressPauseStateOnEnqueue(Ptr<NetDevice> ingressPort, Mac48Address dst)
{
    auto qIt = m_ingressQueues.find(ingressPort);
    if (qIt == m_ingressQueues.end() || !qIt->second)
    {
        return;
    }

    const uint32_t ingressNumQueues = qIt->second->GetNumQueues();
    if (ingressNumQueues == 0)
    {
        return;
    }
    const uint32_t ingressMaxPerQueue = qIt->second->GetMaxPacketsPerQueue();
    if (ingressMaxPerQueue == 0)
    {
        return;
    }

    uint32_t ingressQueueId = qIt->second->GetQueueIndexByDst(dst) % ingressNumQueues;
    double ratio = static_cast<double>(qIt->second->GetSubQueueLength(ingressQueueId)) /
                   static_cast<double>(ingressMaxPerQueue);
    auto& pausedQids = m_pausedIngressQueueIds[ingressPort];
    if (ratio >= m_highWatermark && pausedQids.insert(ingressQueueId).second)
    {
        if (!ingressPort)
        {
            FlowIo::PauseNodeQueue(m_node, ingressQueueId);
            if (IsPfcFullLogEnabled())
            {
                NS_LOG_UNCOND("[PFC][THRESH][LOCAL][PAUSE] bridge="
                              << Mac48Address::ConvertFrom(GetAddress())
                              << " node=" << (m_node ? m_node->GetId() : 0)
                              << " qid=" << ingressQueueId
                              << " ratio=" << ratio
                              << " len=" << qIt->second->GetSubQueueLength(ingressQueueId)
                              << " maxPerQueue=" << ingressMaxPerQueue
                              << " dst=" << dst
                              << " t=" << Simulator::Now().GetSeconds() << "s");
            }
            return;
        }

        uint32_t upstreamQueueId = ingressQueueId;
        Ptr<PointToPointNetDevice> ingressP2p = DynamicCast<PointToPointNetDevice>(ingressPort);
        Ptr<PppRoundRobinQueue<Packet>> upstreamQueue =
            ingressP2p ? DynamicCast<PppRoundRobinQueue<Packet>>(ingressP2p->GetQueue()) : nullptr;
        if (upstreamQueue)
        {
            const uint32_t upstreamNumQueues = upstreamQueue->GetNumQueues();
            if (upstreamNumQueues > 0)
            {
                upstreamQueueId = upstreamQueue->GetQueueIndexByDst(dst) % upstreamNumQueues;
            }
        }
        m_ingressPauseUpstreamQueueIds[ingressPort][ingressQueueId] = upstreamQueueId;
        if (IsPfcFullLogEnabled())
        {
            NS_LOG_UNCOND("[PFC][THRESH][NONLOCAL][PAUSE] bridge="
                          << Mac48Address::ConvertFrom(GetAddress())
                          << " ingressPort=" << Mac48Address::ConvertFrom(ingressPort->GetAddress())
                          << " ingressQid=" << ingressQueueId
                          << " upstreamQid=" << upstreamQueueId
                          << " ratio=" << ratio
                          << " len=" << qIt->second->GetSubQueueLength(ingressQueueId)
                          << " maxPerQueue=" << ingressMaxPerQueue
                          << " dst=" << dst
                          << " t=" << Simulator::Now().GetSeconds() << "s");
        }
        sendPauseFrameToUpstream(ingressPort, upstreamQueueId, true);
    }
}

void
MinHopBridgeNetDevice::UpdateIngressPauseStateOnDequeue(Ptr<NetDevice> ingressPort, uint32_t queueId)
{
    auto qIt = m_ingressQueues.find(ingressPort);
    auto pausedIt = m_pausedIngressQueueIds.find(ingressPort);
    if (qIt == m_ingressQueues.end() || !qIt->second || pausedIt == m_pausedIngressQueueIds.end())
    {
        return;
    }

    const uint32_t ingressNumQueues = qIt->second->GetNumQueues();
    if (ingressNumQueues == 0)
    {
        return;
    }
    queueId %= ingressNumQueues;

    const uint32_t ingressMaxPerQueue = qIt->second->GetMaxPacketsPerQueue();
    if (ingressMaxPerQueue == 0)
    {
        return;
    }

    double ratio = static_cast<double>(qIt->second->GetSubQueueLength(queueId)) /
                   static_cast<double>(ingressMaxPerQueue);
    if (ratio <= m_lowWatermark && pausedIt->second.erase(queueId) > 0)
    {
        if (!ingressPort)
        {
            FlowIo::ResumeNodeQueue(m_node, queueId);
            if (IsPfcFullLogEnabled())
            {
                NS_LOG_UNCOND("[PFC][THRESH][LOCAL][RESUME] bridge="
                              << Mac48Address::ConvertFrom(GetAddress())
                              << " node=" << (m_node ? m_node->GetId() : 0)
                              << " qid=" << queueId
                              << " ratio=" << ratio
                              << " len=" << qIt->second->GetSubQueueLength(queueId)
                              << " maxPerQueue=" << ingressMaxPerQueue
                              << " t=" << Simulator::Now().GetSeconds() << "s");
            }
            if (pausedIt->second.empty())
            {
                m_pausedIngressQueueIds.erase(pausedIt);
                m_ingressPauseUpstreamQueueIds.erase(ingressPort);
            }
            return;
        }

        uint32_t upstreamQueueId = queueId;
        auto upstreamMapIt = m_ingressPauseUpstreamQueueIds.find(ingressPort);
        if (upstreamMapIt != m_ingressPauseUpstreamQueueIds.end())
        {
            auto qidMapIt = upstreamMapIt->second.find(queueId);
            if (qidMapIt != upstreamMapIt->second.end())
            {
                upstreamQueueId = qidMapIt->second;
                upstreamMapIt->second.erase(qidMapIt);
            }
            if (upstreamMapIt->second.empty())
            {
                m_ingressPauseUpstreamQueueIds.erase(upstreamMapIt);
            }
        }
        if (IsPfcFullLogEnabled())
        {
            NS_LOG_UNCOND("[PFC][THRESH][NONLOCAL][RESUME] bridge="
                          << Mac48Address::ConvertFrom(GetAddress())
                          << " ingressPort=" << Mac48Address::ConvertFrom(ingressPort->GetAddress())
                          << " ingressQid=" << queueId
                          << " upstreamQid=" << upstreamQueueId
                          << " ratio=" << ratio
                          << " len=" << qIt->second->GetSubQueueLength(queueId)
                          << " maxPerQueue=" << ingressMaxPerQueue
                          << " t=" << Simulator::Now().GetSeconds() << "s");
        }
        sendPauseFrameToUpstream(ingressPort, upstreamQueueId, false);
    }
    if (pausedIt->second.empty())
    {
        m_pausedIngressQueueIds.erase(pausedIt);
        m_ingressPauseUpstreamQueueIds.erase(ingressPort);
    }
}

bool
MinHopBridgeNetDevice::HasAvailableUnicastPath(Ptr<NetDevice> incomingPort, Mac48Address dst)
{
    std::vector<Ptr<NetDevice>> learnedPorts = GetLearnedState(dst);
    if (!learnedPorts.empty())
    {
        for (auto const& port : learnedPorts)
        {
            if (!port || port == incomingPort)
            {
                continue;
            }
            if (!IsEgressQueueCongested(port, dst, true))
            {
                return true;
            }
        }
        return false;
    }

    for (auto const& port : m_ports)
    {
        if (!port || port == incomingPort)
        {
            continue;
        }
        if (!IsEgressQueueCongested(port, dst, true))
        {
            return true;
        }
    }
    return false;
}

bool
MinHopBridgeNetDevice::HasAvailableFloodPath(Ptr<NetDevice> incomingPort, Mac48Address dst)
{
    for (auto const& port : m_ports)
    {
        if (!port || port == incomingPort)
        {
            continue;
        }
        if (!IsEgressQueueCongested(port, dst, true))
        {
            return true;
        }
    }
    return false;
}

void
MinHopBridgeNetDevice::DrainIngressQueue(Ptr<NetDevice> incomingPort)
{
    auto evIt = m_ingressRetryEvents.find(incomingPort);
    if (evIt != m_ingressRetryEvents.end() && !evIt->second.IsPending())
    {
        evIt->second = EventId();
    }

    auto qIt = m_ingressQueues.find(incomingPort);
    if (qIt == m_ingressQueues.end() || !qIt->second)
    {
        return;
    }

    Ptr<const Packet> head = qIt->second->Peek();
    if (!head)
    {
        return;
    }

    Ptr<Packet> headCopy = head->Copy();
    PppHeader headIngressHeader;
    if (headCopy->RemoveHeader(headIngressHeader) == 0)
    {
        NS_LOG_WARN("Ingress queue head missing synthetic PPP header; dropping head.");
        qIt->second->Dequeue();
        if (qIt->second->GetNPackets() > 0)
        {
            auto& ev = m_ingressRetryEvents[incomingPort];
            if (!ev.IsPending())
            {
                if (incomingPort && m_switchDelay.IsStrictlyPositive())
                {
                    ev = Simulator::Schedule(m_switchDelay,
                                             &MinHopBridgeNetDevice::DrainIngressQueue,
                                             this,
                                             incomingPort);
                }
                else
                {
                    ev = Simulator::ScheduleNow(&MinHopBridgeNetDevice::DrainIngressQueue,
                                                this,
                                                incomingPort);
                }
            }
        }
        return;
    }

    Address headDst = headIngressHeader.GetDstMac();
    PacketType headPacketType = InferPacketTypeFromDst(headDst, PacketType::PACKET_OTHERHOST);
    if (Mac48Address::IsMatchingType(headDst))
    {
        Mac48Address dst48 = Mac48Address::ConvertFrom(headDst);
        bool isFlood = false;
        HopCountHeader hopHeader;
        if (headCopy->PeekHeader(hopHeader) > 0)
        {
            isFlood = hopHeader.IsFlood();
        }

        bool shouldCheckAvailability = false;
        bool hasPath = true;
        if (headPacketType == PacketType::PACKET_OTHERHOST &&
            dst48 != Mac48Address::ConvertFrom(m_address))
        {
            shouldCheckAvailability = true;
            hasPath = isFlood ? HasAvailableFloodPath(incomingPort, dst48)
                              : HasAvailableUnicastPath(incomingPort, dst48);
        }
        else if (headPacketType == PacketType::PACKET_BROADCAST ||
                 headPacketType == PacketType::PACKET_MULTICAST)
        {
            shouldCheckAvailability = true;
            hasPath = HasAvailableFloodPath(incomingPort, dst48);
        }

        if (shouldCheckAvailability && !hasPath)
        {
            ScheduleIngressDrainRetry(incomingPort);
            return;
        }
    }

    Ptr<Packet> p = qIt->second->Dequeue();
    if (!p)
    {
        return;
    }

    uint32_t dequeuedQueueId = 0;
    if (qIt->second->GetLastDequeuedSubQueueId(dequeuedQueueId))
    {
        UpdateIngressPauseStateOnDequeue(incomingPort, dequeuedQueueId);
    }

    PppHeader ingressHeader;
    if (p->RemoveHeader(ingressHeader) == 0)
    {
        NS_LOG_WARN("Ingress packet missing synthetic PPP header; dropping.");
        return;
    }

    uint16_t protocol = ingressHeader.GetProtocol();
    Address src = ingressHeader.GetSrcMac();
    Address dst = ingressHeader.GetDstMac();
    PacketType packetType = InferPacketTypeFromDst(dst, PacketType::PACKET_OTHERHOST);
    ScheduleReceive(incomingPort, p, protocol, src, dst, packetType);

    if (qIt->second->GetNPackets() > 0)
    {
        auto& ev = m_ingressRetryEvents[incomingPort];
        if (!ev.IsPending())
        {
            if (incomingPort && m_switchDelay.IsStrictlyPositive())
            {
                ev = Simulator::Schedule(m_switchDelay,
                                         &MinHopBridgeNetDevice::DrainIngressQueue,
                                         this,
                                         incomingPort);
            }
            else
            {
                ev = Simulator::ScheduleNow(&MinHopBridgeNetDevice::DrainIngressQueue,
                                            this,
                                            incomingPort);
            }
        }
    }
}

NetDevice::PacketType
MinHopBridgeNetDevice::InferPacketTypeFromDst(const Address& dst, PacketType fallback) const
{
    if (!Mac48Address::IsMatchingType(dst))
    {
        return fallback;
    }

    Mac48Address dst48 = Mac48Address::ConvertFrom(dst);
    if (dst48 == Mac48Address::GetBroadcast())
    {
        return NetDevice::PACKET_BROADCAST;
    }
    if (dst48.IsGroup())
    {
        return NetDevice::PACKET_MULTICAST;
    }
    if (dst48 == Mac48Address::ConvertFrom(m_address))
    {
        return NetDevice::PACKET_HOST;
    }
    return NetDevice::PACKET_OTHERHOST;
}

void
MinHopBridgeNetDevice::ScheduleReceive(Ptr<NetDevice> incomingPort,
                                         Ptr<const Packet> packet,
                                         uint16_t protocol,
                                         const Address& src,
                                         const Address& dst,
                                         PacketType packetType)
{
    Ptr<Packet> p = packet->Copy();
    HopCountHeader hopHeader;
    uint8_t currentHops = 0, newHops = 0;
    bool allowFloodForwarding = true;

    Mac48Address src48 = Mac48Address::ConvertFrom(src);
    Mac48Address dst48 = Mac48Address::ConvertFrom(dst);

    if (p->RemoveHeader(hopHeader) > 0)
    {
        // NS_LOG_LOGIC("Is Flood: "<<hopHeader.IsFlood());
        if (hopHeader.IsFlood())
        {
            // Packet came from another bridge, use its hop count
            currentHops = hopHeader.GetHopCount();
            newHops = currentHops + 1;

            bool flagmin = Learn(src48, incomingPort, newHops);

            hopHeader.SetHopCount(newHops);      
            if (newHops > maxjump)
            {
                // NS_LOG_LOGIC("Packet " << p->GetUid() << " dropped, hop count limit reached ("
                //                     << (uint32_t)newHops << " >= 4)");
                return; // Stop processing this packet
            }
            if (flagmin == false)
            {
                // Non-minimal flood replicas should not keep propagating,
                // but they may still be destined to the local host.
                allowFloodForwarding = false;
            }
        }
        else{
            // Time now1=Simulator::Now();
            // NS_LOG_INFO("[SRX] UID=" << packet->GetUid()
            //                << " Received via the switch" 
            //                << " | Time=" << now1.GetFemtoSeconds() << "s");
        }
        p->AddHeader(hopHeader);  
    }

    if (!m_promiscRxCallback.IsNull())
    {
        m_promiscRxCallback(this, p, protocol, src, dst, packetType);
    }

    // Forwarding/Receiving logic
    switch (packetType)
    {
    case PACKET_HOST:
        // Packet is for us. Pass up to local applications.
        if (dst48 == m_address)
        {
            m_rxCallback(this, p, protocol, src);
        }
        break;

    case PACKET_BROADCAST:
    case PACKET_MULTICAST:
        // Pass up to local applications, then flood to other ports.
        // m_rxCallback(this, p, protocol, src);
        if (newHops == 0 || allowFloodForwarding)
        {
            ForwardBroadcast(incomingPort, p, protocol, src48, dst48);
        }
        break;

    case PACKET_OTHERHOST:
        // Packet is unicast, but not for us. Forward it.
        if (dst48 != m_address)
        {
            const bool localInjection = (incomingPort == nullptr);
            if (newHops > 0)
            {
                if (!allowFloodForwarding)
                {
                    break;
                }
                RecordFloodWindowTouch();
                for (auto iter = m_ports.begin(); iter != m_ports.end(); iter++)
                {
                    Ptr<NetDevice> port = *iter;
                    if (port != incomingPort)
                    {
                        if (IsEgressQueueCongested(port, dst48, true))
                        {
                            continue;
                        }
                        sendFromWithDropStats(port, p, src, dst, protocol, localInjection);
                    }
                }
            }
            else
            {
                ForwardUnicast(incomingPort, p, protocol, src48, dst48);                
            }
        }
        else // It's for us but was misidentified? Pass up.
        {
            m_rxCallback(this, p, protocol, src);
        }
        break;
    default:
        break;
    }  
}

void 
MinHopBridgeNetDevice::set_ecmp_model(int model){
    if (model>=0 and model<=2){
        ecmpmodel=model;
    }
}

void 
MinHopBridgeNetDevice::AddEntry(Mac48Address mac, const std::vector<uint32_t>& ports){
    std::vector<LearnedState> mstate;
    uint8_t hopCount = 0;
    for (size_t i=0;i<ports.size();i++){
        LearnedState state;
        state.associatedPort = m_ports[ports[i]];
        state.minHopCount = hopCount;
        state.expirationTime = Time::Max(); 
        mstate.push_back(state);
    }
    m_learnState[mac] = mstate;
}

Ptr<NetDevice>
MinHopBridgeNetDevice::ecmp(std::vector<Ptr<NetDevice> > outPorts, Mac48Address src,  Mac48Address dst, uint64_t uid){
    std::vector<Ptr<NetDevice>> candidatePorts;
    candidatePorts.reserve(outPorts.size());
    for (auto const& outPort : outPorts)
    {
        if (!outPort)
        {
            continue;
        }
        if (IsEgressQueueCongested(outPort, dst, true))
        {
            continue;
        }
        candidatePorts.push_back(outPort);
    }

    if (candidatePorts.empty())
    {
        return nullptr;
    }

    if (ecmpmodel==0){//随机转发
        auto nowsys = std::chrono::system_clock::now();
        auto duration = nowsys.time_since_epoch();
        uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        uint32_t seed = static_cast<uint32_t>(now);
        uint32_t index = seed % candidatePorts.size();
        Ptr<NetDevice> outPort = candidatePorts[index];

        return outPort;
    }
    //按包hash（src+dst+uid+m_address）/按流hash(流ID？)
    
    if (ecmpmodel==1){//HASH（按源地址和目的地址HASH）
        std::ostringstream oss;
        // 添加源和目的 MAC 地址
        oss << src << dst;
        // 添加本地地址
        oss << m_address;
        // 添加 packet UID
        oss << uid;
        std::string key = oss.str();
        // // 使用标准哈希函数
        std::hash<std::string> hasher;
        size_t hash = hasher(key);

        // 取模得到输出端口索引
        uint32_t index = hash % candidatePorts.size();
        // NS_LOG_INFO("m_address (converted to MAC): " << m_address);
        return candidatePorts[index];
    }
    if (ecmpmodel==2){//哈希测试
        // --- 全部内嵌的小工具 ---
        auto macToU64 = [](const Mac48Address& a)->uint64_t {
        uint8_t b[6]; a.CopyTo(b);
        return (uint64_t)b[0]<<40 | (uint64_t)b[1]<<32 | (uint64_t)b[2]<<24 |
                (uint64_t)b[3]<<16 | (uint64_t)b[4]<<8  | (uint64_t)b[5];
        };
        auto fmix64 = [](uint64_t k)->uint64_t {
        k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33; return k;
        };
        auto rotl64 = [](uint64_t x, int r)->uint64_t { return (x<<r) | (x>>(64-r)); };
        auto fastrange64 = [](uint64_t h, uint32_t n)->uint32_t {
        #if defined(__SIZEOF_INT128__)
        return (uint32_t)(((__uint128_t)h * n) >> 64); // 无偏
        #else
        return (uint32_t)(h % n); // 兜底
        #endif
        };

        Mac48Address selfMac = Mac48Address::ConvertFrom(m_address);
        // 组合键：(src, dst, 本端MAC, uid)
        // 如需“按流哈希”，把 k4 改成 flow_id（或 {tag,src,dst} 的稳定编码），别用 uid。
        uint64_t h  = 0x9e3779b97f4a7c15ULL;      // 种子
        uint64_t k1 = macToU64(src);
        uint64_t k2 = macToU64(dst);
        uint64_t k3 = macToU64(selfMac);
        uint64_t k4 = uid;                        // ← 按包；按流请改为 flow_id

        h ^= fmix64(k1); h = rotl64(h,27) * 0x9ddfea08eb382d69ULL;
        h ^= fmix64(k2); h = rotl64(h,27) * 0x9ddfea08eb382d69ULL;
        h ^= fmix64(k3); h = rotl64(h,27) * 0x9ddfea08eb382d69ULL;
        h ^= fmix64(k4); h = fmix64(h);

        uint32_t idx = fastrange64(h, (uint32_t)candidatePorts.size());
        return candidatePorts[idx];
    }
    return candidatePorts.front();
}

void
MinHopBridgeNetDevice::ForwardUnicast(Ptr<NetDevice> incomingPort,
                                Ptr<const Packet> packet,
                                uint16_t protocol,
                                Mac48Address src,
                                Mac48Address dst)
{
    // NS_LOG_FUNCTION_NOARGS();
    // NS_LOG_DEBUG("LearningBridgeForward (incomingPort="
    //              << incomingPort->GetInstanceTypeId().GetName() << ", packet=" << packet
    //              << ", protocol=" << protocol << ", src=" << src << ", dst=" << dst << ")");

    const bool localInjection = (incomingPort == nullptr);
    std::vector<Ptr<NetDevice> > outPort = GetLearnedState(dst);
    // for(auto p:outPort)
    // {
    //     in_to_out[p].insert(src);
    //     // NS_LOG_INFO("Insert src "<< src <<" to map of device"<< Mac48Address::ConvertFrom(p->GetAddress()));
    // }

    if (!outPort.empty())
    {
        std::vector<Ptr<NetDevice>> ecmpCandidates;
        ecmpCandidates.reserve(outPort.size());
        for (auto const& port : outPort)
        {
            if (!port || port == incomingPort)
            {
                continue;
            }
            if (IsEgressQueueCongested(port, dst, true))
            {
                continue;
            }
            ecmpCandidates.push_back(port);
        }
        Ptr<NetDevice> realoutport = ecmp(ecmpCandidates, src, dst, packet->GetUid());

        if (realoutport)
        {
            // NS_LOG_LOGIC("Learning bridge state says to use port `"
            //          << realoutport->GetInstanceTypeId().GetName() << "'");
            sendFromWithDropStats(realoutport, packet, src, dst, protocol, localInjection);
        }
        
        // for (int i=0;i<outPort.size();i++){
        //     NS_LOG_LOGIC("Learning bridge state says to use port `"
        //              << outPort[i]->GetInstanceTypeId().GetName() << "'");
        //     outPort[i]->SendFrom(packet->Copy(), src, dst, protocol);    
        // }
    }
    else
    {
        RecordFloodWindowTouch();
        // Unknown unicast: treat as flooded traffic so downstream bridges apply hop-limit
        // and learning logic consistently.
        Ptr<Packet> floodedPacket = packet->Copy();
        HopCountHeader floodHeader;
        if (floodedPacket->RemoveHeader(floodHeader) > 0)
        {
            floodHeader.SetIsFlood(true);
            floodedPacket->AddHeader(floodHeader);
        }
        Ptr<const Packet> txPacket = floodedPacket;

        // NS_LOG_LOGIC("At time:"<<Simulator::Now().GetSeconds()<< "s");
        // NS_LOG_INFO("No learned state: send through all ports");
        for (auto iter = m_ports.begin(); iter != m_ports.end(); iter++)
        {
            Ptr<NetDevice> port = *iter;
            if (port != incomingPort)
            {
                if (IsEgressQueueCongested(port, dst, true))
                {
                    continue;
                }
                // NS_LOG_LOGIC("LearningBridgeForward ("
                //              << src << " => " << dst
                //              << "): " << incomingPort->GetInstanceTypeId().GetName() << " --> "
                //              << port->GetInstanceTypeId().GetName() << " (UID " << packet->GetUid()
                //              << ").");
                sendFromWithDropStats(port, txPacket, src, dst, protocol, localInjection);
            }
        }
    }
}

void
MinHopBridgeNetDevice::ForwardBroadcast(Ptr<NetDevice> incomingPort,
                                        Ptr<const Packet> packet,
                                        uint16_t protocol,
                                        Mac48Address src,
                                        Mac48Address dst)
{
    NS_LOG_FUNCTION_NOARGS();
    const bool localInjection = (incomingPort == nullptr);
    RecordFloodWindowTouch();

    Ptr<Packet> floodedPacket = packet->Copy();
    HopCountHeader floodHeader;
    if (floodedPacket->RemoveHeader(floodHeader) > 0)
    {
        floodHeader.SetIsFlood(true);
        floodedPacket->AddHeader(floodHeader);
    }
    Ptr<const Packet> txPacket = floodedPacket;

    // --- FIX: Learning is now done in ReceiveFromDevice, no need to do it here ---

    for (auto iter = m_ports.begin(); iter != m_ports.end(); iter++)
    {
        Ptr<NetDevice> port = *iter;
        if (port != incomingPort)
        {
            if (IsEgressQueueCongested(port, dst, true))
            {
                continue;
            }
            // NS_LOG_LOGIC("Flooding (" << src << " => " << dst << "): "
            //                           << " --> port " << port->GetIfIndex()
            //                           << " (UID " << packet->GetUid() << ").");
            sendFromWithDropStats(port, txPacket, src, dst, protocol, localInjection);
        }
    }
}

bool
MinHopBridgeNetDevice::Learn(Mac48Address source, Ptr<NetDevice> port, uint8_t hopCount)
{
    // NS_LOG_INFO(this << source << " via port " << port->GetIfIndex() << " with hop count " << (uint32_t)hopCount);
    bool flagmin = true;

    if (!m_enableLearning)
    {
        // NS_LOG_LOGIC("Node " << GetNode()->GetId() << ": Learning is disabled.");
        return false;
    }

    Time now = Simulator::Now();
    std::vector<LearnedState>& states = m_learnState[source]; // Access or create vector for source MAC

    // Find the current overall minimum hop count from existing entries (without considering expiration here).
    uint8_t currentOverallMinHops = 255;
    if (!states.empty())
    {
        for (const auto& ls : states)
        {
            currentOverallMinHops = std::min(currentOverallMinHops, ls.minHopCount);
        }
    }

    // Check if an entry for the incoming port already exists in the current states.
    bool portEntryExists = false;
    for (auto& ls : states)
    {
        if (ls.associatedPort == port)
        {
            portEntryExists = true;
            // If the incoming hop count is better than the currently recorded for this specific port, update it.
            if (hopCount < ls.minHopCount)
            {
                // NS_LOG_LOGIC("Node " << GetNode()->GetId() << ": Updating existing entry for MAC "
                //                      << source << " on port " << port->GetIfIndex()
                //                      << " with better hop count: " << (uint32_t)hopCount
                //                      << " (old: " << (uint32_t)ls.minHopCount << ")");
                ls.minHopCount = hopCount;
            }
            ls.expirationTime = now + m_expirationTime; // Always refresh timer for existing path
            break;
        }
    }

    if (!portEntryExists)
    {
        // This is a new port for this MAC address.
        if (states.empty() || hopCount < currentOverallMinHops)
        {
            // If no entries, or this path is strictly better than any existing path,
            // clear all old entries and add this new best path.
            // NS_LOG_LOGIC("Node " << GetNode()->GetId() << ": Found new best path for MAC "
            //                      << source << " via port " << port->GetIfIndex()
            //                      << " with hop count " << (uint32_t)hopCount
            //                      << ". Clearing older, worse paths.");
            states.clear(); // Clear all previous entries
            LearnedState newLs;
            newLs.associatedPort = port;
            newLs.minHopCount = hopCount;
            newLs.expirationTime = now + m_expirationTime;
            states.push_back(newLs);
        }
        else if (hopCount == currentOverallMinHops)
        {
            // This is an equally good path, add it.
            // NS_LOG_LOGIC("Node " << GetNode()->GetId() << ": Found another equal-cost path for MAC "
            //                      << source << " via port " << port->GetIfIndex()
            //                      << " with hop count " << (uint32_t)hopCount);
            LearnedState newLs;
            newLs.associatedPort = port;
            newLs.minHopCount = hopCount;
            newLs.expirationTime = now + m_expirationTime;
            states.push_back(newLs);
        }
        else
        {
            flagmin = false;
            // This path is worse than existing minimum, ignore it.
            // NS_LOG_LOGIC("Node " << GetNode()->GetId() << ": Ignoring worse path for MAC "
            //                      << source << " via port " << port->GetIfIndex()
            //                      << " with hop count " << (uint32_t)hopCount
            //                      << " (current min: " << (uint32_t)currentOverallMinHops << ")");
        }
    }
    else
    {
        // If an existing port entry was found and potentially updated,
        // we need to re-evaluate if this update changed the overall minimum,
        // and if so, filter out paths that are no longer optimal.
        // This handles cases where an existing path for 'port' becomes the new best.
        uint8_t reEvaluatedMinHops = 255;
        for (const auto& ls : states)
        {
            reEvaluatedMinHops = std::min(reEvaluatedMinHops, ls.minHopCount);
        }

        if (reEvaluatedMinHops < currentOverallMinHops)
        {
            // NS_LOG_LOGIC("Node " << GetNode()->GetId() << ": Re-evaluating learned state for "
            //                      << source << ". New overall min hops: " << (uint32_t)reEvaluatedMinHops
            //                      << " due to update on port " << port->GetIfIndex());
            std::vector<LearnedState> filteredStates;
            for (const auto& ls : states)
            {
                if (ls.minHopCount == reEvaluatedMinHops)
                {
                    filteredStates.push_back(ls);
                }
            }
            states = filteredStates;
        }
    }

    return flagmin;
}

std::vector<Ptr<NetDevice>>
MinHopBridgeNetDevice::GetLearnedState(Mac48Address source)
{
    // NS_LOG_FUNCTION(this << source);
    std::vector<Ptr<NetDevice>> resultPorts;

    if (!m_enableLearning)
    {
        // NS_LOG_LOGIC("Node " << GetNode()->GetId() << ": Learning is disabled, no learned state.");
        return resultPorts;
    }

    Time now = Simulator::Now();
    auto it = m_learnState.find(source);

    if (it != m_learnState.end())
    {
        std::vector<LearnedState>& states = it->second;
        uint8_t overallMinHops = 255;
        std::vector<LearnedState> validStates; // To rebuild the vector without expired entries

        // First pass: Find the overall minimum hop count among valid entries
        for (const auto& ls : states)
        {
            if (ls.expirationTime > now)
            {
                validStates.push_back(ls); // Keep valid entries
                overallMinHops = std::min(overallMinHops, ls.minHopCount);
            }
            else
            {
                // NS_LOG_LOGIC("Node " << GetNode()->GetId() << ": Expired learned entry for MAC "
                //                      << source << " on port " << ls.associatedPort->GetIfIndex()
                //                      << " with hops " << (uint32_t)ls.minHopCount);
            }
        }
        states = validStates; // Update the map's vector, removing expired entries

        // Second pass: Collect all ports that match the overall minimum hop count
        for (const auto& ls : states)
        {
            if (ls.minHopCount == overallMinHops)
            {
                // Ensure no duplicate ports are added if somehow they exist
                bool alreadyAdded = false;
                for (const auto& existingPort : resultPorts)
                {
                    if (existingPort == ls.associatedPort)
                    {
                        alreadyAdded = true;
                        break;
                    }
                }
                if (!alreadyAdded)
                {
                    resultPorts.push_back(ls.associatedPort);
                }
            }
        }
    }
    return resultPorts;
}

uint32_t
MinHopBridgeNetDevice::GetNBridgePorts() const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_ports.size();
}

Ptr<NetDevice>
MinHopBridgeNetDevice::GetBridgePort(uint32_t n) const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_ports[n];
}

std::vector<MinHopBridgeNetDevice::IngressQueueSnapshot>
MinHopBridgeNetDevice::GetIngressQueueSnapshots() const
{
    std::vector<IngressQueueSnapshot> snapshots;
    snapshots.reserve(m_ports.size());

    for (uint32_t portIndex = 0; portIndex < m_ports.size(); ++portIndex)
    {
        Ptr<NetDevice> port = m_ports[portIndex];
        auto qIt = m_ingressQueues.find(port);
        if (qIt == m_ingressQueues.end() || !qIt->second)
        {
            continue;
        }

        IngressQueueSnapshot snapshot;
        snapshot.portIndex = portIndex;
        snapshot.portMac = Mac48Address::ConvertFrom(port->GetAddress());
        snapshot.maxPacketsPerQueue = qIt->second->GetMaxPacketsPerQueue();
        const uint32_t numQueues = qIt->second->GetNumQueues();
        snapshot.subQueueLengths.reserve(numQueues);
        snapshot.subQueueDropPackets.reserve(numQueues);
        for (uint32_t qid = 0; qid < numQueues; ++qid)
        {
            snapshot.subQueueLengths.push_back(qIt->second->GetSubQueueLength(qid));
            snapshot.subQueueDropPackets.push_back(qIt->second->GetSubQueueDropPackets(qid));
        }

        snapshots.push_back(snapshot);
    }

    return snapshots;
}

void
MinHopBridgeNetDevice::AddBridgePort(Ptr<NetDevice> bridgePort)
{
    NS_LOG_FUNCTION_NOARGS();
    NS_ASSERT(bridgePort != this);
    if (!Mac48Address::IsMatchingType(bridgePort->GetAddress()))
    {
        NS_FATAL_ERROR("Device does not support eui 48 addresses: cannot be added to bridge.");
    }
    if (!bridgePort->SupportsSendFrom())
    {
        NS_FATAL_ERROR("Device does not support SendFrom: cannot be added to bridge.");
    }
    if (m_address == Mac48Address() or is_set_address == false)
    {
        m_address = Mac48Address::ConvertFrom(bridgePort->GetAddress());
        is_set_address = true;
    }

    Ptr<PointToPointNetDevice> bridgeP2p = DynamicCast<PointToPointNetDevice>(bridgePort);
    if (!bridgeP2p)
    {
        NS_FATAL_ERROR("MinHopBridgeNetDevice only supports PointToPointNetDevice bridge ports when RRQ "
                       "is enabled.");
    }

    Ptr<PppRoundRobinQueue<Packet>> egressQueue =
        DynamicCast<PppRoundRobinQueue<Packet>>(bridgeP2p->GetQueue());
    if (!egressQueue)
    {
        NS_FATAL_ERROR("Bridge expects P2P egress queue to be pre-configured as "
                       "ns3::PppRoundRobinQueue in topology setup. port="
                       << Mac48Address::ConvertFrom(bridgePort->GetAddress()));
    }
    const uint32_t ingressNumQueues = egressQueue->GetNumQueues();
    const uint32_t ingressMaxPacketsPerQueue = egressQueue->GetMaxPacketsPerQueue();
    if (ingressNumQueues == 0 || ingressMaxPacketsPerQueue == 0)
    {
        NS_FATAL_ERROR("Bridge found invalid RRQ egress config (NumQueues/MaxPacketsPerQueue == 0). "
                       "port=" << Mac48Address::ConvertFrom(bridgePort->GetAddress()));
    }

    Ptr<PppRoundRobinQueue<Packet>> ingressQueue = CreateObject<PppRoundRobinQueue<Packet>>();
    ingressQueue->SetAttribute("NumQueues", UintegerValue(ingressNumQueues));
    ingressQueue->SetAttribute("MaxPacketsPerQueue", UintegerValue(ingressMaxPacketsPerQueue));
    ingressQueue->SyncQueueBaseMaxSize();
    m_ingressQueues[bridgePort] = ingressQueue;

    auto localIngressIt = m_ingressQueues.find(nullptr);
    if (localIngressIt == m_ingressQueues.end() || !localIngressIt->second)
    {
        // nullptr key is a sentinel for locally injected packets via bridge SendFrom().
        Ptr<PppRoundRobinQueue<Packet>> localIngressQueue = CreateObject<PppRoundRobinQueue<Packet>>();
        m_ingressQueues[nullptr] = localIngressQueue;
        localIngressIt = m_ingressQueues.find(nullptr);
    }
    if (localIngressIt != m_ingressQueues.end() && localIngressIt->second)
    {
        // Keep local ingress queue config aligned with bridge egress queue config.
        localIngressIt->second->SetAttribute("NumQueues", UintegerValue(ingressNumQueues));
        localIngressIt->second->SetAttribute("MaxPacketsPerQueue",
                                             UintegerValue(ingressMaxPacketsPerQueue));
        localIngressIt->second->SyncQueueBaseMaxSize();
    }

    NS_LOG_DEBUG("RegisterProtocolHandler for " << bridgePort->GetInstanceTypeId().GetName());
    m_node->RegisterProtocolHandler(MakeCallback(&MinHopBridgeNetDevice::ReceiveFromDevice, this),
                                    0,
                                    bridgePort,
                                    true);
    m_ports.push_back(bridgePort);
    m_channel->AddChannel(bridgePort->GetChannel());
}

void
MinHopBridgeNetDevice::Set_switchDelay(Time switchDelay)
{
    m_switchDelay = switchDelay;
}

void
MinHopBridgeNetDevice::SetIfIndex(const uint32_t index)
{
    NS_LOG_FUNCTION_NOARGS();
    m_ifIndex = index;
}

uint32_t
MinHopBridgeNetDevice::GetIfIndex() const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_ifIndex;
}

Ptr<Channel>
MinHopBridgeNetDevice::GetChannel() const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_channel;
}

void
MinHopBridgeNetDevice::SetAddress(Address address)
{
    NS_LOG_FUNCTION_NOARGS();
    m_address = Mac48Address::ConvertFrom(address);
}

Address
MinHopBridgeNetDevice::GetAddress() const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_address;
}

bool
MinHopBridgeNetDevice::SetMtu(const uint16_t mtu)
{
    NS_LOG_FUNCTION_NOARGS();
    m_mtu = mtu;
    return true;
}

uint16_t
MinHopBridgeNetDevice::GetMtu() const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_mtu;
}

bool
MinHopBridgeNetDevice::IsLinkUp() const
{
    NS_LOG_FUNCTION_NOARGS();
    return true;
}

void
MinHopBridgeNetDevice::AddLinkChangeCallback(Callback<void> callback)
{
}

bool
MinHopBridgeNetDevice::IsBroadcast() const
{
    NS_LOG_FUNCTION_NOARGS();
    return true;
}

Address
MinHopBridgeNetDevice::GetBroadcast() const
{
    NS_LOG_FUNCTION_NOARGS();
    return Mac48Address::GetBroadcast();
}

bool
MinHopBridgeNetDevice::IsMulticast() const
{
    NS_LOG_FUNCTION_NOARGS();
    return true;
}

Address
MinHopBridgeNetDevice::GetMulticast(Ipv4Address multicastGroup) const
{
    NS_LOG_FUNCTION(this << multicastGroup);
    Mac48Address multicast = Mac48Address::GetMulticast(multicastGroup);
    return multicast;
}

bool
MinHopBridgeNetDevice::IsPointToPoint() const
{
    NS_LOG_FUNCTION_NOARGS();
    return false;
}

bool
MinHopBridgeNetDevice::IsBridge() const
{
    NS_LOG_FUNCTION_NOARGS();
    return true;
}

bool
MinHopBridgeNetDevice::Send(Ptr<Packet> packet, const Address& dest, uint16_t protocolNumber)
{
    NS_LOG_FUNCTION_NOARGS();
    return SendFrom(packet, m_address, dest, protocolNumber);
}

bool
MinHopBridgeNetDevice::SendFrom(Ptr<Packet> packet,
                                const Address& src,
                                const Address& dest,
                                uint16_t protocolNumber)
{
    NS_LOG_FUNCTION_NOARGS();
    PacketType packetType = InferPacketTypeFromDst(dest, PacketType::PACKET_OTHERHOST);
    return EnqueueIngressPacket(nullptr, packet, protocolNumber, src, dest, packetType, false);
}

bool
MinHopBridgeNetDevice::sendFromWithDropStats(Ptr<NetDevice> outPort,
                                             Ptr<const Packet> packet,
                                             const Address& src,
                                             const Address& dst,
                                             uint16_t protocol,
                                             bool localInjection)
{
    if (!outPort || !packet)
    {
        return false;
    }

    bool ok = outPort->SendFrom(packet->Copy(), src, dst, protocol);
    if (ok)
    {
        return true;
    }

    uint64_t& dropCounter = localInjection ? m_localSendFailDrops : m_forwardSendFailDrops;
    ++dropCounter;
    static uint64_t sDropLogCount = 0;
    if (sDropLogCount < 20 || (sDropLogCount % 1000) == 0)
    {
        NS_LOG_UNCOND("[DROP][BRIDGE][SEND_FAIL] mode=" << (localInjection ? "LOCAL_INJECT" : "FORWARD")
                      << " bridge=" << Mac48Address::ConvertFrom(m_address)
                      << " outPort=" << Mac48Address::ConvertFrom(outPort->GetAddress())
                      << " src=" << Mac48Address::ConvertFrom(src)
                      << " dst=" << Mac48Address::ConvertFrom(dst)
                      << " proto=0x" << std::hex << protocol << std::dec
                      << " uid=" << packet->GetUid()
                      << " modeDropPkts=" << dropCounter);
    }
    ++sDropLogCount;
    return false;
}

Ptr<Node>
MinHopBridgeNetDevice::GetNode() const
{
    NS_LOG_FUNCTION_NOARGS();
    return m_node;
}

void
MinHopBridgeNetDevice::SetNode(Ptr<Node> node)
{
    NS_LOG_FUNCTION_NOARGS();
    m_node = node;
}

bool
MinHopBridgeNetDevice::NeedsArp() const
{
    NS_LOG_FUNCTION_NOARGS();
    return true;
}

void
MinHopBridgeNetDevice::SetReceiveCallback(NetDevice::ReceiveCallback cb)
{
    NS_LOG_FUNCTION_NOARGS();
    m_rxCallback = cb;
}

void
MinHopBridgeNetDevice::SetPromiscReceiveCallback(NetDevice::PromiscReceiveCallback cb)
{
    NS_LOG_FUNCTION_NOARGS();
    m_promiscRxCallback = cb;
}

bool
MinHopBridgeNetDevice::SupportsSendFrom() const
{
    NS_LOG_FUNCTION_NOARGS();
    return true;
}

Address
MinHopBridgeNetDevice::GetMulticast(Ipv6Address addr) const
{
    NS_LOG_FUNCTION(this << addr);
    return Mac48Address::GetMulticast(addr);
}
void
MinHopBridgeNetDevice::Print_LocalMacAddress() const
{
    NS_LOG_INFO("Learning State for Host on Node " << GetNode()->GetId() 
    << " (Device MAC: " << GetAddress());
}
void
MinHopBridgeNetDevice::PrintLearnedState() const
{
    NS_LOG_INFO("Learning State for Bridge on Node " << GetNode()->GetId() 
    << " (Device MAC: " << GetAddress() << ")  Size= "<<m_learnState.size());
    if (m_learnState.empty())
    {
        NS_LOG_INFO("  No learned entries.");
        return;
    }

    Time now = Simulator::Now();
    for (auto const& pair : m_learnState)
    {
        Mac48Address learnedMac = pair.first;
        const std::vector<LearnedState>& states = pair.second; // This is now a vector

        if (states.empty())
        {
            NS_LOG_INFO("  MAC: " << learnedMac << " (No valid entries)");
            continue;
        }

        // Find the overall minimum hop count for this MAC among non-expired entries
        uint8_t currentOverallMinHops = 255;
        bool hasValidEntry = false;
        for (const auto& ls : states)
        {
            if (ls.expirationTime > now)
            {
                currentOverallMinHops = std::min(currentOverallMinHops, ls.minHopCount);
                hasValidEntry = true;
            }
        }

        std::string portsStr = "";
        if (hasValidEntry)
        {
            for (const auto& ls : states)
            {
                std::ostringstream oss;
                oss << Mac48Address::ConvertFrom(ls.associatedPort->GetAddress());
                portsStr += "port " + std::to_string(ls.associatedPort->GetIfIndex()) +
                            " (MAC: " + oss.str() + ", Hops: " + std::to_string((uint32_t)ls.minHopCount) +
                            ", Expires: " + std::to_string((ls.expirationTime - now).GetSeconds()) + "s" +
                            (ls.minHopCount == currentOverallMinHops && ls.expirationTime > now ? " - MIN & VALID" : "") +
                            (ls.expirationTime <= now ? " - EXPIRED" : "") + ") ";
            }
            NS_LOG_INFO("  MAC: " << learnedMac << ", Overall Min Hops (Valid): " << (uint32_t)currentOverallMinHops
                                 << ", Paths: [" << portsStr << "]");
        }
        else
        {
            NS_LOG_INFO("  MAC: " << learnedMac << " (All entries expired or invalid)");
        }
    }
}

void
MinHopBridgeNetDevice::sendPauseFrameToUpstream(Ptr<NetDevice> ingressPort, uint32_t queueId, bool pause)
{
    Ptr<PointToPointNetDevice> ingressP2p = DynamicCast<PointToPointNetDevice>(ingressPort);
    if (!ingressP2p)
    {
        return;
    }

    Address upstreamPeer = ingressP2p->GetPeerAddress();
    Mac48Address upstreamPeerMac = Mac48Address::ConvertFrom(upstreamPeer);
    Ptr<Packet> p = Create<Packet>(0);
    PauseHeader header(pause ? PauseHeader::PAUSE : PauseHeader::RESUME,
                       upstreamPeerMac,
                       static_cast<uint16_t>(queueId));
    p->AddHeader(header);
    (void) ingressPort->SendFrom(p, ingressPort->GetAddress(), upstreamPeer, 0x8808);
}

bool
MinHopBridgeNetDevice::writemactable(std::string filepath){
    std::ofstream fout(filepath, std::ios::out | std::ios::trunc);
    if (!fout)
    {
        NS_LOG_WARN("Failed to open mactable file for writing: " << filepath);
        return false;
    }

    Time now = Simulator::Now();
    uint32_t validMacCount = 0;
    for (const auto& [mac, states] : m_learnState)
    {
        (void)mac;
        bool hasValidPath = false;
        for (const auto& st : states)
        {
            if (st.expirationTime <= now || !st.associatedPort)
            {
                continue;
            }
            for (const auto& p : m_ports)
            {
                if (p == st.associatedPort)
                {
                    hasValidPath = true;
                    break;
                }
            }
            if (hasValidPath)
            {
                break;
            }
        }
        if (hasValidPath)
        {
            ++validMacCount;
        }
    }

    fout << "MINHOP_MACTABLE_V1\n";
    fout << "bridge_mac " << Mac48Address::ConvertFrom(GetAddress()) << "\n";
    fout << "entries " << validMacCount << "\n";

    for (const auto& [mac, states] : m_learnState)
    {
        std::map<uint32_t, uint8_t> portHopMap;
        for (const auto& st : states)
        {
            if (st.expirationTime <= now || !st.associatedPort)
            {
                continue;
            }

            int32_t portIndex = -1;
            for (uint32_t i = 0; i < m_ports.size(); ++i)
            {
                if (m_ports[i] == st.associatedPort)
                {
                    portIndex = static_cast<int32_t>(i);
                    break;
                }
            }
            if (portIndex < 0)
            {
                continue;
            }

            auto it = portHopMap.find(static_cast<uint32_t>(portIndex));
            if (it == portHopMap.end() || st.minHopCount < it->second)
            {
                portHopMap[static_cast<uint32_t>(portIndex)] = st.minHopCount;
            }
        }

        if (portHopMap.empty())
        {
            continue;
        }

        fout << "mac " << mac << " paths " << portHopMap.size() << "\n";
        for (const auto& [portIndex, hopCount] : portHopMap)
        {
            fout << "port " << portIndex << " hop " << static_cast<uint32_t>(hopCount) << "\n";
        }
    }

    return true;
}

bool
MinHopBridgeNetDevice::readmactable(std::string filepath){
    std::ifstream fin(filepath);
    if (!fin)
    {
        NS_LOG_WARN("Failed to open mactable file for reading: " << filepath);
        return false;
    }

    std::string token;
    fin >> token;
    if (!fin || token != "MINHOP_MACTABLE_V1")
    {
        NS_LOG_WARN("Invalid mactable header in file: " << filepath);
        return false;
    }

    std::string bridgeMacToken;
    std::string bridgeMacStr;
    fin >> bridgeMacToken >> bridgeMacStr;
    if (!fin || bridgeMacToken != "bridge_mac")
    {
        NS_LOG_WARN("Invalid bridge_mac section in mactable file: " << filepath);
        return false;
    }

    std::string entriesToken;
    uint32_t entryCount = 0;
    fin >> entriesToken >> entryCount;
    if (!fin || entriesToken != "entries")
    {
        NS_LOG_WARN("Invalid entries section in mactable file: " << filepath);
        return false;
    }

    std::map<Mac48Address, std::vector<LearnedState>> loadedState;
    for (uint32_t i = 0; i < entryCount; ++i)
    {
        std::string macToken;
        std::string macStr;
        std::string pathsToken;
        uint32_t pathCount = 0;
        fin >> macToken >> macStr >> pathsToken >> pathCount;
        if (!fin || macToken != "mac" || pathsToken != "paths")
        {
            NS_LOG_WARN("Malformed entry header while reading mactable: " << filepath);
            return false;
        }

        Mac48Address learnedMac(macStr.c_str());
        std::map<uint32_t, uint8_t> portHopMap;
        for (uint32_t p = 0; p < pathCount; ++p)
        {
            std::string portToken;
            uint32_t portIndex = 0;
            std::string hopToken;
            uint32_t hopCount = 0;
            fin >> portToken >> portIndex >> hopToken >> hopCount;
            if (!fin || portToken != "port" || hopToken != "hop")
            {
                NS_LOG_WARN("Malformed path entry while reading mactable: " << filepath);
                return false;
            }

            if (portIndex >= m_ports.size())
            {
                continue;
            }
            if (hopCount > 255)
            {
                hopCount = 255;
            }

            auto it = portHopMap.find(portIndex);
            if (it == portHopMap.end() || hopCount < it->second)
            {
                portHopMap[portIndex] = static_cast<uint8_t>(hopCount);
            }
        }

        if (portHopMap.empty())
        {
            continue;
        }

        std::vector<LearnedState> states;
        states.reserve(portHopMap.size());
        for (const auto& [portIndex, hopCount] : portHopMap)
        {
            LearnedState st;
            st.associatedPort = m_ports[portIndex];
            st.minHopCount = hopCount;
            // Keep imported entries alive for the whole run unless re-learned.
            st.expirationTime = Time::Max();
            states.push_back(st);
        }
        loadedState[learnedMac] = states;
    }

    if (!fin.good() && !fin.eof())
    {
        NS_LOG_WARN("Error while reading mactable file: " << filepath);
        return false;
    }

    m_learnState.swap(loadedState);
    return true;
}

} // namespace ns3