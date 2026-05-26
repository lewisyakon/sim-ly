#include "min-hop-bridge-net-device.h"
#include "flow-io.h"
#include "hop-count-header.h"
#include "pause-header.h"
#include "rdma-dispatch-tag.h"
#include "rdma-ingress-port-tag.h"
#include "rdma-port-engine.h"

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
#include <limits>

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

struct ComputedMacTableDebugCounters
{
    uint64_t computedRuleForwardHits = 0;
    uint64_t learnedTableForwardHits = 0;
    uint64_t unknownUnicastFloodFallbacks = 0;
    uint64_t explicitBroadcastFloods = 0;
    uint64_t threePhaseUnsafeFloodDrops = 0;
    uint64_t computedRuleLookupHits = 0;
    uint64_t computedRuleLookupMisses = 0;
};

ComputedMacTableDebugCounters g_computedMacTableDebugCounters;
std::mutex g_computedMacTableDebugMutex;

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

void
PrintComputedMacTableDebugSummaryImpl(bool printToStdout)
{
    ComputedMacTableDebugCounters snapshot;
    {
        std::lock_guard<std::mutex> guard(g_computedMacTableDebugMutex);
        snapshot = g_computedMacTableDebugCounters;
    }

    std::ostringstream oss;
    oss << "[MACTABLE][COMPUTED][DEBUG]"
        << " computed_rule_forward_hits=" << snapshot.computedRuleForwardHits
        << " learned_table_forward_hits=" << snapshot.learnedTableForwardHits
        << " unknown_unicast_flood_fallbacks=" << snapshot.unknownUnicastFloodFallbacks
        << " explicit_broadcast_floods=" << snapshot.explicitBroadcastFloods
        << " three_phase_unsafe_flood_drops=" << snapshot.threePhaseUnsafeFloodDrops
        << " computed_rule_lookup_hits=" << snapshot.computedRuleLookupHits
        << " computed_rule_lookup_misses=" << snapshot.computedRuleLookupMisses;
    NS_LOG_UNCOND(oss.str());
    if (printToStdout)
    {
        std::cout << oss.str() << std::endl;
    }
}

void
RecordComputedMacTableCounter(void (*increment)(ComputedMacTableDebugCounters&))
{
    std::lock_guard<std::mutex> guard(g_computedMacTableDebugMutex);
    increment(g_computedMacTableDebugCounters);
}

void
RecordComputedRuleForwardHit()
{
    RecordComputedMacTableCounter(
        [](ComputedMacTableDebugCounters& counters) { ++counters.computedRuleForwardHits; });
}

void
RecordLearnedTableForwardHit()
{
    RecordComputedMacTableCounter(
        [](ComputedMacTableDebugCounters& counters) { ++counters.learnedTableForwardHits; });
}

void
RecordUnknownUnicastFloodFallback()
{
    RecordComputedMacTableCounter(
        [](ComputedMacTableDebugCounters& counters) { ++counters.unknownUnicastFloodFallbacks; });
}

void
RecordExplicitBroadcastFlood()
{
    RecordComputedMacTableCounter(
        [](ComputedMacTableDebugCounters& counters) { ++counters.explicitBroadcastFloods; });
}

void
RecordThreePhaseUnsafeFloodDrop()
{
    RecordComputedMacTableCounter(
        [](ComputedMacTableDebugCounters& counters) { ++counters.threePhaseUnsafeFloodDrops; });
}

void
RecordComputedRuleLookupHit()
{
    RecordComputedMacTableCounter(
        [](ComputedMacTableDebugCounters& counters) { ++counters.computedRuleLookupHits; });
}

void
RecordComputedRuleLookupMiss()
{
    RecordComputedMacTableCounter(
        [](ComputedMacTableDebugCounters& counters) { ++counters.computedRuleLookupMisses; });
}

uint32_t
StableMix32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}
} // namespace

void
MinHopBridgeNetDevice::PrintFloodWindowSummary(bool printToStdout)
{
    TryPrintFloodWindowSummaryOnce(printToStdout);
}

void
MinHopBridgeNetDevice::PrintComputedMacTableDebugSummary(bool printToStdout)
{
    PrintComputedMacTableDebugSummaryImpl(printToStdout);
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
            .AddAttribute("TerminalLearningOnly",
                          "When true, packets arriving from bridge ports are only used for "
                          "local learning/receive and will not be forwarded onward.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&MinHopBridgeNetDevice::m_terminalLearningOnly),
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
            .AddAttribute("NotHeadRetryDelay",
                          "Compatibility no-op after ingress buffering switched to exact-token aggregate queue.",
                          TimeValue(NanoSeconds(1)),
                          MakeTimeAccessor(&MinHopBridgeNetDevice::m_notHeadRetryDelay),
                          MakeTimeChecker())
            .AddAttribute("BridgeDiagEnable",
                          "Compatibility no-op; bridge packet residence diagnostics are not emitted.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&MinHopBridgeNetDevice::m_bridgeDiagEnabled),
                          MakeBooleanChecker())
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
      m_terminalLearningOnly(false),
      m_highWatermark(0.8),
      m_lowWatermark(0.2),
      m_notHeadRetryDelay(NanoSeconds(1)),
      m_bridgeDiagEnabled(false),
      m_nextIngressPacketToken(0),
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
    for (auto& entry : m_ingressPackets)
    {
        ingressRemain += entry.second.size();
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
    m_ingressPackets.clear();
    m_ingressQueueStates.clear();
    for (auto& portEvents : m_ingressRetryEvents)
    {
        for (auto& retryEvent : portEvents.second)
        {
            if (retryEvent.second.IsPending())
            {
                Simulator::Cancel(retryEvent.second);
            }
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
    auto stateIt = m_ingressQueueStates.find(incomingPort);
    if (stateIt == m_ingressQueueStates.end() ||
        stateIt->second.numQueues == 0 ||
        stateIt->second.maxPacketsPerQueue == 0 ||
        stateIt->second.subQueueLengths.empty())
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
    BridgeIngressQueueState& state = stateIt->second;
    const uint32_t ingressQueueId = 0;

    if (state.subQueueLengths[ingressQueueId] >= state.maxPacketsPerQueue)
    {
        ++m_ingressQueueDrops;
        m_ingressQueueDropBytes += ingressPacket->GetSize();
        ++state.subQueueDropPackets[ingressQueueId];
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
                          << " qid=" << ingressQueueId
                          << " totalDrops=" << m_ingressQueueDrops);
        }
        ++sIngressDropLogCount;
        return false;
    }

    ++state.subQueueLengths[ingressQueueId];
    if (Mac48Address::IsMatchingType(dst))
    {
        UpdateIngressPauseStateOnEnqueue(incomingPort, Mac48Address::ConvertFrom(dst));
    }

    Time drainDelay = Seconds(0);
    if (addSwitchDelay && m_switchDelay.IsStrictlyPositive())
    {
        drainDelay = m_switchDelay;
    }
    uint64_t packetToken = ++m_nextIngressPacketToken;
    BridgeIngressPacketRecord record;
    record.packet = ingressPacket;
    record.protocol = protocol;
    record.src = src;
    record.dst = dst;
    record.packetType = InferPacketTypeFromDst(dst, packetType);
    record.queueId = ingressQueueId;
    m_ingressPackets[incomingPort][packetToken] = record;

    ScheduleIngressDrainEvent(incomingPort, packetToken, drainDelay);
    return true;
}

void
MinHopBridgeNetDevice::ScheduleIngressDrainEvent(Ptr<NetDevice> incomingPort,
                                                 uint64_t packetToken,
                                                 Time delay)
{
    auto& portEvents = m_ingressRetryEvents[incomingPort];
    auto& ev = portEvents[packetToken];
    if (!ev.IsPending())
    {
        if (delay.IsStrictlyPositive())
        {
            ev = Simulator::Schedule(delay,
                                     &MinHopBridgeNetDevice::DrainIngressPacket,
                                     this,
                                     incomingPort,
                                     packetToken);
        }
        else
        {
            ev = Simulator::ScheduleNow(&MinHopBridgeNetDevice::DrainIngressPacket,
                                        this,
                                        incomingPort,
                                        packetToken);
        }
    }
}

void
MinHopBridgeNetDevice::ScheduleIngressDrainRetry(Ptr<NetDevice> incomingPort,
                                                 uint64_t packetToken)
{
    Time retryDelay = (m_switchDelay.IsStrictlyPositive() ? m_switchDelay : MicroSeconds(1));
    ScheduleIngressDrainEvent(incomingPort, packetToken, retryDelay);
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

uint32_t
MinHopBridgeNetDevice::GetIngressQueueId(Ptr<NetDevice> ingressPort, Mac48Address dst)
{
    return 0;
}

void
MinHopBridgeNetDevice::UpdateIngressPauseStateOnEnqueue(Ptr<NetDevice> ingressPort, Mac48Address dst)
{
    auto stateIt = m_ingressQueueStates.find(ingressPort);
    if (stateIt == m_ingressQueueStates.end())
    {
        return;
    }

    BridgeIngressQueueState& state = stateIt->second;
    const uint32_t ingressNumQueues = state.numQueues;
    if (ingressNumQueues == 0)
    {
        return;
    }
    const uint32_t ingressMaxPerQueue = state.maxPacketsPerQueue;
    if (ingressMaxPerQueue == 0)
    {
        return;
    }

    const uint32_t ingressQueueId = 0;
    double ratio = static_cast<double>(state.subQueueLengths[ingressQueueId]) /
                   static_cast<double>(ingressMaxPerQueue);
    if (ratio >= m_highWatermark)
    {
        auto& pausedQids = m_pausedIngressQueueIds[ingressPort];
        if (!pausedQids.insert(ingressQueueId).second)
        {
            return;
        }

        const uint32_t pauseQueueCount = std::max(1u, state.pauseQueueCount);
        if (!ingressPort)
        {
            for (uint32_t qid = 0; qid < pauseQueueCount; ++qid)
            {
                FlowIo::PauseNodeQueue(m_node, qid);
            }
            if (IsPfcFullLogEnabled())
            {
                NS_LOG_UNCOND("[PFC][THRESH][LOCAL][PAUSE] bridge="
                              << Mac48Address::ConvertFrom(GetAddress())
                              << " node=" << (m_node ? m_node->GetId() : 0)
                              << " aggregateQid=" << ingressQueueId
                              << " pausedQids=" << pauseQueueCount
                              << " ratio=" << ratio
                              << " len=" << state.subQueueLengths[ingressQueueId]
                              << " maxPerQueue=" << ingressMaxPerQueue
                              << " dst=" << dst
                              << " t=" << Simulator::Now().GetSeconds() << "s");
            }
            return;
        }

        Ptr<PointToPointNetDevice> ingressP2p = DynamicCast<PointToPointNetDevice>(ingressPort);
        Ptr<PppRoundRobinQueue<Packet>> upstreamQueue =
            ingressP2p ? DynamicCast<PppRoundRobinQueue<Packet>>(ingressP2p->GetQueue()) : nullptr;
        uint32_t upstreamNumQueues = pauseQueueCount;
        if (upstreamQueue)
        {
            upstreamNumQueues = std::max(1u, upstreamQueue->GetNumQueues());
        }
        for (uint32_t qid = 0; qid < upstreamNumQueues; ++qid)
        {
            m_ingressPauseUpstreamQueueIds[ingressPort][qid] = qid;
            sendPauseFrameToUpstream(ingressPort, qid, true);
        }
        if (IsPfcFullLogEnabled())
        {
            NS_LOG_UNCOND("[PFC][THRESH][NONLOCAL][PAUSE] bridge="
                          << Mac48Address::ConvertFrom(GetAddress())
                          << " ingressPort=" << Mac48Address::ConvertFrom(ingressPort->GetAddress())
                          << " aggregateQid=" << ingressQueueId
                          << " pausedQids=" << upstreamNumQueues
                          << " ratio=" << ratio
                          << " len=" << state.subQueueLengths[ingressQueueId]
                          << " maxPerQueue=" << ingressMaxPerQueue
                          << " dst=" << dst
                          << " t=" << Simulator::Now().GetSeconds() << "s");
        }
    }
}

void
MinHopBridgeNetDevice::UpdateIngressPauseStateOnDequeue(Ptr<NetDevice> ingressPort, uint32_t queueId)
{
    auto stateIt = m_ingressQueueStates.find(ingressPort);
    auto pausedIt = m_pausedIngressQueueIds.find(ingressPort);
    if (stateIt == m_ingressQueueStates.end() || pausedIt == m_pausedIngressQueueIds.end())
    {
        return;
    }

    BridgeIngressQueueState& state = stateIt->second;
    const uint32_t ingressNumQueues = state.numQueues;
    if (ingressNumQueues == 0)
    {
        return;
    }
    queueId %= ingressNumQueues;

    const uint32_t ingressMaxPerQueue = state.maxPacketsPerQueue;
    if (ingressMaxPerQueue == 0)
    {
        return;
    }

    double ratio = static_cast<double>(state.subQueueLengths[queueId]) /
                   static_cast<double>(ingressMaxPerQueue);
    if (ratio <= m_lowWatermark && pausedIt->second.erase(queueId) > 0)
    {
        const uint32_t pauseQueueCount = std::max(1u, state.pauseQueueCount);
        if (!ingressPort)
        {
            for (uint32_t qid = 0; qid < pauseQueueCount; ++qid)
            {
                FlowIo::ResumeNodeQueue(m_node, qid);
            }
            if (IsPfcFullLogEnabled())
            {
                NS_LOG_UNCOND("[PFC][THRESH][LOCAL][RESUME] bridge="
                              << Mac48Address::ConvertFrom(GetAddress())
                              << " node=" << (m_node ? m_node->GetId() : 0)
                              << " aggregateQid=" << queueId
                              << " resumedQids=" << pauseQueueCount
                              << " ratio=" << ratio
                              << " len=" << state.subQueueLengths[queueId]
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

        auto upstreamMapIt = m_ingressPauseUpstreamQueueIds.find(ingressPort);
        if (upstreamMapIt != m_ingressPauseUpstreamQueueIds.end())
        {
            for (const auto& pausedQid : upstreamMapIt->second)
            {
                sendPauseFrameToUpstream(ingressPort, pausedQid.second, false);
            }
            m_ingressPauseUpstreamQueueIds.erase(upstreamMapIt);
        }
        if (IsPfcFullLogEnabled())
        {
            NS_LOG_UNCOND("[PFC][THRESH][NONLOCAL][RESUME] bridge="
                          << Mac48Address::ConvertFrom(GetAddress())
                          << " ingressPort=" << Mac48Address::ConvertFrom(ingressPort->GetAddress())
                          << " aggregateQid=" << queueId
                          << " resumedQids=" << pauseQueueCount
                          << " ratio=" << ratio
                          << " len=" << state.subQueueLengths[queueId]
                          << " maxPerQueue=" << ingressMaxPerQueue
                          << " t=" << Simulator::Now().GetSeconds() << "s");
        }
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
        bool hasCandidate = false;
        for (auto const& port : learnedPorts)
        {
            if (!port || port == incomingPort)
            {
                continue;
            }
            hasCandidate = true;
            if (!IsEgressQueueCongested(port, dst, true))
            {
                return true;
            }
        }
        return !hasCandidate;
    }

    bool hasCandidate = false;
    for (auto const& port : m_ports)
    {
        if (!port || port == incomingPort)
        {
            continue;
        }
        hasCandidate = true;
        if (!IsEgressQueueCongested(port, dst, true))
        {
            return true;
        }
    }
    return !hasCandidate;
}

bool
MinHopBridgeNetDevice::HasAvailableFloodPath(Ptr<NetDevice> incomingPort, Mac48Address dst)
{
    bool hasCandidate = false;
    for (auto const& port : m_ports)
    {
        if (!port || port == incomingPort)
        {
            continue;
        }
        hasCandidate = true;
        if (!IsEgressQueueCongested(port, dst, true))
        {
            return true;
        }
    }
    // No alternate flood egress is a terminal condition, not congestion.
    // Let the packet drain so the bridge can learn/receive it and stop forwarding.
    return !hasCandidate;
}

void
MinHopBridgeNetDevice::DrainIngressPacket(Ptr<NetDevice> incomingPort,
                                          uint64_t packetToken)
{
    auto portEventsIt = m_ingressRetryEvents.find(incomingPort);
    if (portEventsIt != m_ingressRetryEvents.end())
    {
        auto eventIt = portEventsIt->second.find(packetToken);
        if (eventIt != portEventsIt->second.end())
        {
            portEventsIt->second.erase(eventIt);
        }
        if (portEventsIt->second.empty())
        {
            m_ingressRetryEvents.erase(portEventsIt);
        }
    }

    auto portPacketsIt = m_ingressPackets.find(incomingPort);
    if (portPacketsIt == m_ingressPackets.end())
    {
        return;
    }

    auto packetIt = portPacketsIt->second.find(packetToken);
    if (packetIt == portPacketsIt->second.end())
    {
        return;
    }

    BridgeIngressPacketRecord record = packetIt->second;
    if (!record.packet)
    {
        portPacketsIt->second.erase(packetIt);
        if (portPacketsIt->second.empty())
        {
            m_ingressPackets.erase(portPacketsIt);
        }
        return;
    }

    Ptr<Packet> packetCopy = record.packet->Copy();
    if (Mac48Address::IsMatchingType(record.dst))
    {
        Mac48Address dst48 = Mac48Address::ConvertFrom(record.dst);
        bool isFlood = false;
        HopCountHeader hopHeader;
        if (packetCopy->PeekHeader(hopHeader) > 0)
        {
            isFlood = hopHeader.IsFlood();
        }

        bool shouldCheckAvailability = false;
        bool hasPath = true;
        const bool terminalTransitPacket =
            m_terminalLearningOnly &&
            incomingPort != nullptr &&
            ((record.packetType == PacketType::PACKET_OTHERHOST &&
              dst48 != Mac48Address::ConvertFrom(m_address)) ||
             record.packetType == PacketType::PACKET_BROADCAST ||
             record.packetType == PacketType::PACKET_MULTICAST);

        if (!terminalTransitPacket &&
            record.packetType == PacketType::PACKET_OTHERHOST &&
            dst48 != Mac48Address::ConvertFrom(m_address))
        {
            shouldCheckAvailability = true;
            hasPath = isFlood ? HasAvailableFloodPath(incomingPort, dst48)
                              : HasAvailableUnicastPath(incomingPort, dst48);
        }
        else if (!terminalTransitPacket &&
                 (record.packetType == PacketType::PACKET_BROADCAST ||
                  record.packetType == PacketType::PACKET_MULTICAST))
        {
            shouldCheckAvailability = true;
            hasPath = HasAvailableFloodPath(incomingPort, dst48);
        }

        if (shouldCheckAvailability && !hasPath)
        {
            static uint64_t sNoPathRetryLogCount = 0;
            if (sNoPathRetryLogCount < 5000 || (sNoPathRetryLogCount % 10000) == 0)
            {
                HopCountHeader retryHopHeader;
                ThreePHeader retryTpHeader;
                Ptr<Packet> retryCopy = record.packet->Copy();
                bool parsedThreePhase = false;
                if (retryCopy->PeekHeader(retryHopHeader) > 0)
                {
                    retryCopy->RemoveHeader(retryHopHeader);
                    parsedThreePhase = (retryCopy->PeekHeader(retryTpHeader) > 0);
                }

                std::ostringstream ingressDesc;
                if (incomingPort)
                {
                    ingressDesc << Mac48Address::ConvertFrom(incomingPort->GetAddress());
                }
                else
                {
                    ingressDesc << "LOCAL";
                }

                NS_LOG_INFO("[BRIDGE][RETRY][NO_PATH] bridge="
                            << Mac48Address::ConvertFrom(GetAddress())
                            << " ingressPort=" << ingressDesc.str()
                            << " dstMac=" << dst48
                            << " packetType=" << static_cast<uint32_t>(record.packetType)
                            << " isFlood=" << isFlood
                            << " parsed3P=" << parsedThreePhase
                            << " state="
                            << (parsedThreePhase ? static_cast<uint32_t>(retryTpHeader.GetState())
                                                 : 0xffffffffu)
                            << " tag="
                            << (parsedThreePhase ? retryTpHeader.GetTag() : 0xffffffffu)
                            << " src="
                            << (parsedThreePhase ? retryTpHeader.GetSrc() : 0xffffffffu)
                            << " dst="
                            << (parsedThreePhase ? retryTpHeader.GetDst() : 0xffffffffu)
                            << " retryDelayNs="
                            << (m_switchDelay.IsStrictlyPositive()
                                    ? m_switchDelay.GetNanoSeconds()
                                    : MicroSeconds(1).GetNanoSeconds())
                            << " TIME=" << Simulator::Now().GetFemtoSeconds());
            }
            ++sNoPathRetryLogCount;
            ScheduleIngressDrainRetry(incomingPort, packetToken);
            return;
        }
    }

    auto stateIt = m_ingressQueueStates.find(incomingPort);
    if (stateIt != m_ingressQueueStates.end() &&
        record.queueId < stateIt->second.subQueueLengths.size() &&
        stateIt->second.subQueueLengths[record.queueId] > 0)
    {
        --stateIt->second.subQueueLengths[record.queueId];
    }
    UpdateIngressPauseStateOnDequeue(incomingPort, record.queueId);

    portPacketsIt->second.erase(packetIt);
    if (portPacketsIt->second.empty())
    {
        m_ingressPackets.erase(portPacketsIt);
    }

    ScheduleReceive(incomingPort,
                    record.packet,
                    record.protocol,
                    record.src,
                    record.dst,
                    record.packetType);
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

    if (m_terminalLearningOnly &&
        incomingPort != nullptr &&
        ((packetType == PACKET_OTHERHOST &&
          dst48 != Mac48Address::ConvertFrom(m_address)) ||
         packetType == PACKET_BROADCAST ||
         packetType == PACKET_MULTICAST))
    {
        static uint64_t sTerminalDropLogCount = 0;
        if (sTerminalDropLogCount < 200 || (sTerminalDropLogCount % 10000) == 0)
        {
            // NS_LOG_INFO("[BRIDGE][TERMINAL][DROP] bridge="
            //             << Mac48Address::ConvertFrom(GetAddress())
            //             << " ingressPort="
            //             << Mac48Address::ConvertFrom(incomingPort->GetAddress())
            //             << " src=" << src48
            //             << " dst=" << dst48
            //             << " packetType=" << static_cast<uint32_t>(packetType)
            //             << " floodForward=" << (newHops > 0)
            //             << " learnedPaths=" << GetLearnedState(src48).size());
        }
        ++sTerminalDropLogCount;
        return;
    }

    // Forwarding/Receiving logic
    switch (packetType)
    {
    case PACKET_HOST:
        // Packet is for us. Pass up to local applications.
        if (dst48 == m_address)
        {
            Ptr<Packet> rxPacket = p->Copy();
            if (incomingPort)
            {
                RdmaIngressPortTag ingressTag;
                ingressTag.SetDeviceIndex(incomingPort->GetIfIndex());
                rxPacket->AddPacketTag(ingressTag);
            }
            m_rxCallback(this, rxPacket, protocol, src);
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
            Ptr<Packet> rxPacket = p->Copy();
            if (incomingPort)
            {
                RdmaIngressPortTag ingressTag;
                ingressTag.SetDeviceIndex(incomingPort->GetIfIndex());
                rxPacket->AddPacketTag(ingressTag);
            }
            m_rxCallback(this, rxPacket, protocol, src);
        }
        break;
    default:
        break;
    }  
}

void 
MinHopBridgeNetDevice::set_ecmp_model(int model){
    if (model>=0 and model<=4){
        ecmpmodel=model;
    }
}

void
MinHopBridgeNetDevice::SetFatTreeEcmpContext(uint32_t k,
                                             uint32_t gpusPerServer,
                                             EcmpTopologyLayer layer,
                                             uint32_t layerIndex)
{
    m_hasFatTreeEcmpContext = (k >= 2 && gpusPerServer > 0);
    m_fatTreeK = k;
    m_fatTreeGpusPerServer = gpusPerServer;
    m_ecmpTopologyLayer = layer;
    m_fatTreeLayerIndex = layerIndex;
}

void
MinHopBridgeNetDevice::SetComputedFatTreeMacTableContext(uint32_t k,
                                                         uint32_t gpusPerServer,
                                                         ComputedMacTableKind kind,
                                                         uint32_t index)
{
    m_computedMacTableEnabled =
        (k >= 2 && (k % 2) == 0 && gpusPerServer > 0 &&
         kind != ComputedMacTableKind::Disabled);
    m_computedFatTreeK = k;
    m_computedGpusPerServer = gpusPerServer;
    m_computedMacTableKind = kind;
    m_computedBridgeIndex = index;
}

void
MinHopBridgeNetDevice::SetComputedMacToGpuIndexTable(
    std::shared_ptr<const std::map<Mac48Address, uint32_t>> table)
{
    m_computedMacToGpuIndex = table;
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
MinHopBridgeNetDevice::ecmp(std::vector<Ptr<NetDevice> > outPorts,
                            Mac48Address src,
                            Mac48Address dst,
                            uint64_t uid,
                            uint32_t flowTag,
                            bool hasFlowTag,
                            uint32_t flowSrc,
                            uint32_t flowDst,
                            bool hasFlowTuple,
                            uint32_t flowState){
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
    if (ecmpmodel==2){//HASH（按流优先；无tag时回退到uid）
        std::ostringstream oss;
        // 组合键：(src, dst, 本地地址, flowTag/uid)
        oss << src << dst;
        oss << m_address;
        if (hasFlowTag)
        {
            oss << "tag:" << flowTag;
        }
        else
        {
            oss << "uid:" << uid;
        }
        std::string key = oss.str();
        std::hash<std::string> hasher;
        size_t hash = hasher(key);

        uint32_t index = hash % candidatePorts.size();
        return candidatePorts[index];
    }
    if (ecmpmodel==3){//DATA 按流喷洒；非 DATA 按 ecmpmodel=0 随机处理
        std::ostringstream candidateOss;
        std::ostringstream candidateSetOss;
        std::vector<Mac48Address> candidateSetMacs;
        candidateOss << "[";
        for (size_t i = 0; i < candidatePorts.size(); ++i)
        {
            Mac48Address candidateMac = Mac48Address::ConvertFrom(candidatePorts[i]->GetAddress());
            candidateSetMacs.push_back(candidateMac);
            if (i > 0)
            {
                candidateOss << ",";
            }
            candidateOss << "{idx=" << i
                         << ",if=" << candidatePorts[i]->GetIfIndex()
                         << ",mac=" << candidateMac
                         << "}";
        }
        candidateOss << "]";
        std::sort(candidateSetMacs.begin(), candidateSetMacs.end());
        for (size_t i = 0; i < candidateSetMacs.size(); ++i)
        {
            if (i > 0)
            {
                candidateSetOss << "|";
            }
            candidateSetOss << candidateSetMacs[i];
        }
        std::string candidateSetKey = candidateSetOss.str();

        if (hasFlowTuple && flowState == static_cast<uint32_t>(ThreePHeader::DATA))
        {
            std::string balanceScope = "candidateSet";
            std::string balanceDomainKey = candidateSetKey;
            uint32_t dstPod = 0xffffffffu;
            uint32_t dstEdge = 0xffffffffu;
            if (m_hasFatTreeEcmpContext && m_fatTreeK >= 2 && m_fatTreeGpusPerServer > 0)
            {
                uint32_t serversPerPod = m_fatTreeK * m_fatTreeK / 4;
                uint32_t edgesPerPod = m_fatTreeK / 2;
                uint32_t serversPerEdge = m_fatTreeK / 2;
                uint32_t dstServer = flowDst / m_fatTreeGpusPerServer;
                if (serversPerPod > 0 && serversPerEdge > 0)
                {
                    dstPod = dstServer / serversPerPod;
                    dstEdge = (dstServer % serversPerPod) / serversPerEdge;
                    if (dstPod < m_fatTreeK && dstEdge < edgesPerPod)
                    {
                        std::ostringstream domainOss;
                        domainOss << candidateSetKey;
                        if (m_ecmpTopologyLayer == EcmpTopologyLayer::Edge)
                        {
                            balanceScope = "candidateSet+dstPod+dstEdge";
                            domainOss << "|dstPod=" << dstPod << "|dstEdge=" << dstEdge;
                        }
                        else if (m_ecmpTopologyLayer == EcmpTopologyLayer::Aggregation)
                        {
                            balanceScope = "candidateSet+dstPod";
                            domainOss << "|dstPod=" << dstPod;
                        }
                        balanceDomainKey = domainOss.str();
                    }
                }
            }
            FlowSprayKey key{balanceDomainKey, flowState, flowTag, flowSrc, flowDst};
            auto learnedPort = m_flowSprayPorts.find(key);
            auto candidateIt =
                learnedPort == m_flowSprayPorts.end()
                    ? candidatePorts.end()
                    : std::find(candidatePorts.begin(), candidatePorts.end(), learnedPort->second);
            if (candidateIt != candidatePorts.end())
            {
                uint32_t index =
                    static_cast<uint32_t>(std::distance(candidatePorts.begin(), candidateIt));
                NS_LOG_INFO("[ECMP][FLOW_SPRAY] bridge=" << m_address
                            << " srcMac=" << src
                            << " dstMac=" << dst
                            << " flowState=" << flowState
                            << " flowTuple=" << flowTag << ":" << flowSrc << ":" << flowDst
                            << " action=data_reuse"
                            << " selected={idx=" << index
                            << ",if=" << learnedPort->second->GetIfIndex()
                            << ",mac=" << Mac48Address::ConvertFrom(learnedPort->second->GetAddress())
                            << "}"
                            << " candidateCount=" << candidatePorts.size()
                            << " candidates=" << candidateOss.str()
                            << " candidateSet=" << candidateSetKey
                            << " balanceScope=" << balanceScope
                            << " balanceDomain=" << balanceDomainKey
                            << " dstPod=" << dstPod
                            << " dstEdge=" << dstEdge
                            << " uid=" << uid
                            << " timeFs=" << Simulator::Now().GetFemtoSeconds());
                return learnedPort->second;
            }

            uint32_t& nextIndex = m_flowSprayNextPortByBalanceDomain[balanceDomainKey];
            std::map<Mac48Address, uint32_t>& portLoads =
                m_flowSprayPortLoadsByBalanceDomain[balanceDomainKey];
            uint32_t tieOffset = nextIndex % candidatePorts.size();
            if (m_hasFatTreeEcmpContext)
            {
                tieOffset = StableMix32(flowSrc ^ (flowDst << 8) ^ (flowTag << 16) ^
                                        (m_fatTreeLayerIndex << 24)) %
                            candidatePorts.size();
            }
            uint32_t index = 0;
            uint32_t minLoad = std::numeric_limits<uint32_t>::max();
            for (size_t n = 0; n < candidatePorts.size(); ++n)
            {
                uint32_t candidateIndex = (tieOffset + n) % candidatePorts.size();
                Mac48Address candidateMac =
                    Mac48Address::ConvertFrom(candidatePorts[candidateIndex]->GetAddress());
                uint32_t load = 0;
                auto loadIt = portLoads.find(candidateMac);
                if (loadIt != portLoads.end())
                {
                    load = loadIt->second;
                }
                if (load < minLoad)
                {
                    minLoad = load;
                    index = candidateIndex;
                }
            }
            Ptr<NetDevice> outPort = candidatePorts[index];
            Mac48Address selectedMac = Mac48Address::ConvertFrom(outPort->GetAddress());
            uint32_t selectedLoadBefore = portLoads[selectedMac];
            portLoads[selectedMac] = selectedLoadBefore + 1;
            nextIndex = (index + 1) % candidatePorts.size();
            std::ostringstream oldPortOss;
            if (learnedPort == m_flowSprayPorts.end())
            {
                oldPortOss << "none";
            }
            else
            {
                oldPortOss << "if=" << learnedPort->second->GetIfIndex()
                           << ",mac="
                           << Mac48Address::ConvertFrom(learnedPort->second->GetAddress());
            }
            NS_LOG_INFO("[ECMP][FLOW_SPRAY] bridge=" << m_address
                        << " srcMac=" << src
                        << " dstMac=" << dst
                        << " flowState=" << flowState
                        << " flowTuple=" << flowTag << ":" << flowSrc << ":" << flowDst
                        << " action=" << (learnedPort == m_flowSprayPorts.end() ? "data_new"
                                                                                 : "data_reassign")
                        << " oldPort=" << oldPortOss.str()
                        << " selected={idx=" << index
                        << ",if=" << outPort->GetIfIndex()
                        << ",mac=" << selectedMac
                        << "}"
                        << " nextIndexForBalanceDomain=" << nextIndex
                        << " tieOffset=" << tieOffset
                        << " selectedLoadBefore=" << selectedLoadBefore
                        << " selectedLoadAfter=" << portLoads[selectedMac]
                        << " candidateCount=" << candidatePorts.size()
                        << " candidates=" << candidateOss.str()
                        << " candidateSet=" << candidateSetKey
                        << " balanceScope=" << balanceScope
                        << " balanceDomain=" << balanceDomainKey
                        << " dstPod=" << dstPod
                        << " dstEdge=" << dstEdge
                        << " uid=" << uid
                        << " timeFs=" << Simulator::Now().GetFemtoSeconds());
            m_flowSprayPorts[key] = outPort;
            return outPort;
        }

        auto nowsys = std::chrono::system_clock::now();
        auto duration = nowsys.time_since_epoch();
        uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        uint32_t seed = static_cast<uint32_t>(now);
        uint32_t index = seed % candidatePorts.size();
        Ptr<NetDevice> outPort = candidatePorts[index];
        NS_LOG_INFO("[ECMP][FLOW_SPRAY] bridge=" << m_address
                    << " srcMac=" << src
                    << " dstMac=" << dst
                    << " flowState=" << flowState
                    << " flowTuple="
                    << (hasFlowTuple ? (std::to_string(flowTag) + ":" +
                                         std::to_string(flowSrc) + ":" +
                                         std::to_string(flowDst))
                                      : std::string("none"))
                    << " action=non_data_model0"
                    << " randomSeed=" << seed
                    << " selected={idx=" << index
                    << ",if=" << outPort->GetIfIndex()
                    << ",mac=" << Mac48Address::ConvertFrom(outPort->GetAddress())
                    << "}"
                    << " candidateCount=" << candidatePorts.size()
                    << " candidates=" << candidateOss.str()
                    << " candidateSet=" << candidateSetKey
                    << " uid=" << uid
                    << " timeFs=" << Simulator::Now().GetFemtoSeconds());
        return outPort;
    }
    if (ecmpmodel==4){//All-to-all 特化：按物理源目在 fat-tree 中固定拆分
        std::ostringstream candidateOss;
        candidateOss << "[";
        for (size_t i = 0; i < candidatePorts.size(); ++i)
        {
            if (i > 0)
            {
                candidateOss << ",";
            }
            candidateOss << "{idx=" << i
                         << ",if=" << candidatePorts[i]->GetIfIndex()
                         << ",mac=" << Mac48Address::ConvertFrom(candidatePorts[i]->GetAddress())
                         << "}";
        }
        candidateOss << "]";

        uint32_t index = 0;
        std::string action = "fallback_front";
        uint32_t physicalSrc = flowSrc;
        uint32_t physicalDst = flowDst;
        if (hasFlowTuple &&
            (flowState == static_cast<uint32_t>(ThreePHeader::ACT) ||
             flowState == static_cast<uint32_t>(ThreePHeader::IDLE)))
        {
            physicalSrc = flowDst;
            physicalDst = flowSrc;
        }

        uint32_t srcPod = 0xffffffffu;
        uint32_t dstPod = 0xffffffffu;
        uint32_t srcEdge = 0xffffffffu;
        uint32_t dstEdge = 0xffffffffu;
        uint32_t srcHostInEdge = 0xffffffffu;
        uint32_t dstHostInEdge = 0xffffffffu;
        uint32_t switchPod = 0xffffffffu;
        uint32_t switchLocalIndex = 0xffffffffu;

        if (candidatePorts.size() > 1)
        {
            bool usedFatTreeMapping = false;
            if (hasFlowTuple && m_hasFatTreeEcmpContext && m_fatTreeK >= 2 &&
                m_fatTreeGpusPerServer > 0)
            {
                uint32_t hostsPerEdge = m_fatTreeK / 2;
                uint32_t edgesPerPod = m_fatTreeK / 2;
                uint32_t serversPerPod = m_fatTreeK * m_fatTreeK / 4;
                uint32_t srcServer = physicalSrc / m_fatTreeGpusPerServer;
                uint32_t dstServer = physicalDst / m_fatTreeGpusPerServer;

                if (hostsPerEdge > 0 && edgesPerPod > 0 && serversPerPod > 0)
                {
                    srcPod = srcServer / serversPerPod;
                    dstPod = dstServer / serversPerPod;
                    srcEdge = (srcServer % serversPerPod) / hostsPerEdge;
                    dstEdge = (dstServer % serversPerPod) / hostsPerEdge;
                    srcHostInEdge = srcServer % hostsPerEdge;
                    dstHostInEdge = dstServer % hostsPerEdge;

                    switch (m_ecmpTopologyLayer)
                    {
                    case EcmpTopologyLayer::Edge:
                    {
                        switchPod = m_fatTreeLayerIndex / edgesPerPod;
                        switchLocalIndex = m_fatTreeLayerIndex % edgesPerPod;
                        if (switchPod == srcPod && switchLocalIndex == srcEdge)
                        {
                            index = (srcPod + srcHostInEdge + dstPod) % candidatePorts.size();
                            action = "edge_src_pod_host_dstpod";
                        }
                        else
                        {
                            index = dstHostInEdge % candidatePorts.size();
                            action = "edge_dst_host";
                        }
                        usedFatTreeMapping = true;
                        break;
                    }
                    case EcmpTopologyLayer::Aggregation:
                    {
                        switchPod = m_fatTreeLayerIndex / edgesPerPod;
                        switchLocalIndex = m_fatTreeLayerIndex % edgesPerPod;
                        if (switchPod == srcPod && switchPod != dstPod)
                        {
                            index = dstEdge % candidatePorts.size();
                            action = "agg_up_dst_edge";
                        }
                        else if (switchPod == dstPod)
                        {
                            index = dstEdge % candidatePorts.size();
                            action = "agg_down_dst_edge";
                        }
                        else
                        {
                            uint32_t h = StableMix32(physicalSrc ^ (physicalDst << 8) ^
                                                     (flowTag << 16) ^
                                                     (m_fatTreeLayerIndex << 24));
                            index = h % candidatePorts.size();
                            action = "agg_fallback_hash";
                        }
                        usedFatTreeMapping = true;
                        break;
                    }
                    case EcmpTopologyLayer::Core:
                    {
                        index = dstPod % candidatePorts.size();
                        action = "core_dst_pod";
                        usedFatTreeMapping = true;
                        break;
                    }
                    default:
                        break;
                    }
                }
            }

            if (!usedFatTreeMapping)
            {
                uint32_t h = hasFlowTuple
                                 ? StableMix32(physicalSrc ^ (physicalDst << 8) ^
                                               (flowTag << 16) ^ (flowState << 24))
                                 : StableMix32(static_cast<uint32_t>(uid) ^
                                               static_cast<uint32_t>(uid >> 32));
                index = h % candidatePorts.size();
                action = hasFlowTuple ? "tuple_hash" : "uid_hash";
            }
        }

        Ptr<NetDevice> outPort = candidatePorts[index];
        NS_LOG_INFO("[ECMP][MODEL4]"
                    << " bridge=" << m_address
                    << " layer=" << static_cast<uint32_t>(m_ecmpTopologyLayer)
                    << " layerIndex=" << m_fatTreeLayerIndex
                    << " flowState=" << flowState
                    << " flowTuple="
                    << (hasFlowTuple ? (std::to_string(flowTag) + ":" +
                                         std::to_string(flowSrc) + ":" +
                                         std::to_string(flowDst))
                                      : std::string("none"))
                    << " physicalSrc=" << physicalSrc
                    << " physicalDst=" << physicalDst
                    << " srcPod=" << srcPod
                    << " srcEdge=" << srcEdge
                    << " srcHostInEdge=" << srcHostInEdge
                    << " dstPod=" << dstPod
                    << " dstEdge=" << dstEdge
                    << " dstHostInEdge=" << dstHostInEdge
                    << " switchPod=" << switchPod
                    << " switchLocalIndex=" << switchLocalIndex
                    << " action=" << action
                    << " selected={idx=" << index
                    << ",if=" << outPort->GetIfIndex()
                    << ",mac=" << Mac48Address::ConvertFrom(outPort->GetAddress())
                    << "}"
                    << " candidateCount=" << candidatePorts.size()
                    << " candidates=" << candidateOss.str()
                    << " uid=" << uid
                    << " timeFs=" << Simulator::Now().GetFemtoSeconds());
        return outPort;
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
    std::vector<Ptr<NetDevice> > outPort = GetLearnedStateForForwarding(dst);
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
        uint32_t flowTag = 0;
        uint32_t flowSrc = 0;
        uint32_t flowDst = 0;
        uint32_t flowState = 0xffffffffu;
        bool hasFlowTag = false;
        bool hasFlowTuple = false;
        Ptr<Packet> flowProbe = packet->Copy();
        HopCountHeader flowProbeHopHeader;
        ThreePHeader flowProbeTpHeader;
        if (flowProbe->PeekHeader(flowProbeHopHeader) > 0)
        {
            flowProbe->RemoveHeader(flowProbeHopHeader);
        }
        if (flowProbe->PeekHeader(flowProbeTpHeader) > 0)
        {
            flowState = static_cast<uint32_t>(flowProbeTpHeader.GetState());
            flowTag = flowProbeTpHeader.GetTag();
            flowSrc = flowProbeTpHeader.GetSrc();
            flowDst = flowProbeTpHeader.GetDst();
            hasFlowTag = true;
            hasFlowTuple = true;
        }
        Ptr<NetDevice> realoutport =
            ecmp(ecmpCandidates,
                 src,
                 dst,
                 packet->GetUid(),
                 flowTag,
                 hasFlowTag,
                 flowSrc,
                 flowDst,
                 hasFlowTuple,
                 flowState);

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
        RecordUnknownUnicastFloodFallback();
        RecordFloodWindowTouch();

        Ptr<Packet> threePhaseProbe = packet->Copy();
        HopCountHeader unknownHopHeader;
        if (threePhaseProbe->PeekHeader(unknownHopHeader) > 0)
        {
            threePhaseProbe->RemoveHeader(unknownHopHeader);
        }
        ThreePHeader unknownTpHeader;
        const bool isThreePhaseUnknown = (threePhaseProbe->PeekHeader(unknownTpHeader) > 0);
        uint32_t unknownCandidatePorts = 0;
        for (auto const& port : m_ports)
        {
            if (port && port != incomingPort)
            {
                ++unknownCandidatePorts;
            }
        }
        if (isThreePhaseUnknown)
        {
            static uint64_t sUnknownThreePhaseFloodLogCount = 0;
            const bool mayReplicate = !localInjection || unknownCandidatePorts > 1;
            if (mayReplicate ||
                sUnknownThreePhaseFloodLogCount < 40 ||
                (sUnknownThreePhaseFloodLogCount % 100000) == 0)
            {
                std::ostringstream ingressDesc;
                if (incomingPort)
                {
                    ingressDesc << Mac48Address::ConvertFrom(incomingPort->GetAddress());
                }
                else
                {
                    ingressDesc << "LOCAL";
                }
                NS_LOG_UNCOND("[BRIDGE][UNKNOWN_UNICAST_FLOOD][3P] bridge="
                              << Mac48Address::ConvertFrom(GetAddress())
                              << " ingressPort=" << ingressDesc.str()
                              << " srcMac=" << src
                              << " dstMac=" << dst
                              << " state=" << static_cast<uint32_t>(unknownTpHeader.GetState())
                              << " tag=" << unknownTpHeader.GetTag()
                              << " flowSrc=" << unknownTpHeader.GetSrc()
                              << " flowDst=" << unknownTpHeader.GetDst()
                              << " candidatePorts=" << unknownCandidatePorts
                              << " localInjection=" << localInjection
                              << " uid=" << packet->GetUid()
                              << " TIME=" << Simulator::Now().GetFemtoSeconds());
            }
            ++sUnknownThreePhaseFloodLogCount;

            if (unknownCandidatePorts > 1)
            {
                static uint64_t sUnknownThreePhaseUnsafeDropLogCount = 0;
                if (sUnknownThreePhaseUnsafeDropLogCount < 200 ||
                    (sUnknownThreePhaseUnsafeDropLogCount % 10000) == 0)
                {
                    NS_LOG_UNCOND("[DROP][BRIDGE][UNKNOWN_UNICAST_3P_UNSAFE_FLOOD] bridge="
                                  << Mac48Address::ConvertFrom(GetAddress())
                                  << " srcMac=" << src
                                  << " dstMac=" << dst
                                  << " state=" << static_cast<uint32_t>(unknownTpHeader.GetState())
                                  << " tag=" << unknownTpHeader.GetTag()
                                  << " flowSrc=" << unknownTpHeader.GetSrc()
                                  << " flowDst=" << unknownTpHeader.GetDst()
                                  << " candidatePorts=" << unknownCandidatePorts
                                  << " localInjection=" << localInjection
                                  << " uid=" << packet->GetUid()
                                  << " TIME=" << Simulator::Now().GetFemtoSeconds());
                }
                ++sUnknownThreePhaseUnsafeDropLogCount;
                RecordThreePhaseUnsafeFloodDrop();
                ++m_forwardSendFailDrops;
                return;
            }
        }

        Ptr<const Packet> txPacket = packet;
        if (!isThreePhaseUnknown)
        {
            // Warmup/learning unknown unicast is propagated as flood traffic.
            Ptr<Packet> floodedPacket = packet->Copy();
            HopCountHeader floodHeader;
            if (floodedPacket->RemoveHeader(floodHeader) > 0)
            {
                floodHeader.SetIsFlood(true);
                floodedPacket->AddHeader(floodHeader);
            }
            txPacket = floodedPacket;
        }

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
    RecordExplicitBroadcastFlood();
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
MinHopBridgeNetDevice::GetComputedFatTreeLearnedState(Mac48Address dst) const
{
    std::vector<Ptr<NetDevice>> resultPorts;
    if (!m_computedMacTableEnabled || !m_computedMacToGpuIndex)
    {
        return resultPorts;
    }

    const auto gpuIt = m_computedMacToGpuIndex->find(dst);
    if (gpuIt == m_computedMacToGpuIndex->end())
    {
        return resultPorts;
    }

    const uint32_t k = m_computedFatTreeK;
    const uint32_t half = k / 2;
    const uint32_t gpusPerServer = m_computedGpusPerServer;
    if (half == 0 || gpusPerServer == 0)
    {
        return resultPorts;
    }

    const uint32_t gpu = gpuIt->second;
    const uint32_t server = gpu / gpusPerServer;
    const uint32_t localGpu = gpu % gpusPerServer;
    const uint32_t serversPerPod = half * half;
    if (serversPerPod == 0)
    {
        return resultPorts;
    }
    const uint32_t pod = server / serversPerPod;
    const uint32_t serverInPod = server % serversPerPod;
    const uint32_t edge = serverInPod / half;
    const uint32_t hostCol = serverInPod % half;

    auto addPort = [&](uint32_t portIndex) {
        if (portIndex < m_ports.size() && m_ports[portIndex])
        {
            resultPorts.push_back(m_ports[portIndex]);
        }
    };
    auto addRange = [&](uint32_t begin, uint32_t end) {
        for (uint32_t portIndex = begin; portIndex < end; ++portIndex)
        {
            addPort(portIndex);
        }
    };

    switch (m_computedMacTableKind)
    {
    case ComputedMacTableKind::FatTreeCore:
        addPort(pod);
        break;
    case ComputedMacTableKind::FatTreeAggregation:
    {
        const uint32_t aggPod = m_computedBridgeIndex / half;
        if (pod == aggPod)
        {
            addPort(half + edge);
        }
        else
        {
            addRange(0, half);
        }
        break;
    }
    case ComputedMacTableKind::FatTreeEdge:
    {
        const uint32_t edgePod = m_computedBridgeIndex / half;
        const uint32_t edgeInPod = m_computedBridgeIndex % half;
        if (pod == edgePod && edge == edgeInPod)
        {
            addPort(half + hostCol * gpusPerServer + localGpu);
        }
        else
        {
            addRange(0, half);
        }
        break;
    }
    case ComputedMacTableKind::FatTreeGpuInner:
    {
        const uint32_t ownerGpu = m_computedBridgeIndex;
        const uint32_t ownerServer = ownerGpu / gpusPerServer;
        const uint32_t ownerLocalGpu = ownerGpu % gpusPerServer;
        if (server == ownerServer && gpu != ownerGpu)
        {
            const uint32_t portIndex =
                (localGpu < ownerLocalGpu) ? localGpu : localGpu - 1;
            addPort(portIndex);
        }
        break;
    }
    case ComputedMacTableKind::Disabled:
    default:
        break;
    }

    return resultPorts;
}

std::vector<Ptr<NetDevice>>
MinHopBridgeNetDevice::GetLearnedState(Mac48Address source)
{
    // NS_LOG_FUNCTION(this << source);
    std::vector<Ptr<NetDevice>> resultPorts;

    std::vector<Ptr<NetDevice>> computedPorts = GetComputedFatTreeLearnedState(source);
    if (!computedPorts.empty())
    {
        return computedPorts;
    }

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

std::vector<Ptr<NetDevice>>
MinHopBridgeNetDevice::GetLearnedStateForForwarding(Mac48Address source)
{
    std::vector<Ptr<NetDevice>> computedPorts = GetComputedFatTreeLearnedState(source);
    if (!computedPorts.empty())
    {
        RecordComputedRuleForwardHit();
        RecordComputedRuleLookupHit();
        return computedPorts;
    }

    if (m_computedMacTableEnabled)
    {
        RecordComputedRuleLookupMiss();
    }

    std::vector<Ptr<NetDevice>> learnedPorts = GetLearnedState(source);
    if (!learnedPorts.empty())
    {
        RecordLearnedTableForwardHit();
    }
    return learnedPorts;
}

std::vector<Ptr<NetDevice>>
MinHopBridgeNetDevice::GetLocalInjectionEgressCandidates(const Address& dst)
{
    std::vector<Ptr<NetDevice>> candidates;
    if (!Mac48Address::IsMatchingType(dst))
    {
        return candidates;
    }

    Mac48Address dst48 = Mac48Address::ConvertFrom(dst);
    if (dst48 == Mac48Address::GetBroadcast() || dst48.IsGroup())
    {
        candidates.reserve(m_ports.size());
        for (auto const& port : m_ports)
        {
            if (!port)
            {
                continue;
            }
            if (IsEgressQueueCongested(port, dst48, true))
            {
                continue;
            }
            candidates.push_back(port);
        }
        return candidates;
    }

    std::vector<Ptr<NetDevice>> learnedPorts = GetLearnedState(dst48);
    if (!learnedPorts.empty())
    {
        candidates.reserve(learnedPorts.size());
        for (auto const& port : learnedPorts)
        {
            if (!port)
            {
                continue;
            }
            if (IsEgressQueueCongested(port, dst48, true))
            {
                continue;
            }
            candidates.push_back(port);
        }
        return candidates;
    }

    candidates.reserve(m_ports.size());
    for (auto const& port : m_ports)
    {
        if (!port)
        {
            continue;
        }
        if (IsEgressQueueCongested(port, dst48, true))
        {
            continue;
        }
        candidates.push_back(port);
    }
    return candidates;
}

Ptr<NetDevice>
MinHopBridgeNetDevice::ResolveLocalInjectionEgressPort(const Address& src,
                                                       const Address& dst,
                                                       uint64_t uidHint)
{
    std::vector<Ptr<NetDevice>> candidates = GetLocalInjectionEgressCandidates(dst);
    if (candidates.empty())
    {
        return nullptr;
    }
    if (candidates.size() == 1)
    {
        return candidates.front();
    }

    if (!Mac48Address::IsMatchingType(dst))
    {
        return candidates.front();
    }

    const Mac48Address src48 =
        Mac48Address::IsMatchingType(src) ? Mac48Address::ConvertFrom(src)
                                          : Mac48Address::ConvertFrom(m_address);
    const Mac48Address dst48 = Mac48Address::ConvertFrom(dst);
    return ecmp(candidates, src48, dst48, uidHint);
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
        auto stateIt = m_ingressQueueStates.find(port);
        if (stateIt == m_ingressQueueStates.end())
        {
            continue;
        }

        const BridgeIngressQueueState& state = stateIt->second;
        IngressQueueSnapshot snapshot;
        snapshot.portIndex = portIndex;
        snapshot.portMac = Mac48Address::ConvertFrom(port->GetAddress());
        snapshot.maxPacketsPerQueue = state.maxPacketsPerQueue;
        const uint32_t numQueues = state.numQueues;
        snapshot.subQueueLengths.reserve(numQueues);
        snapshot.subQueueDropPackets.reserve(numQueues);
        for (uint32_t qid = 0; qid < numQueues; ++qid)
        {
            snapshot.subQueueLengths.push_back(qid < state.subQueueLengths.size()
                                                   ? state.subQueueLengths[qid]
                                                   : 0);
            snapshot.subQueueDropPackets.push_back(qid < state.subQueueDropPackets.size()
                                                       ? state.subQueueDropPackets[qid]
                                                       : 0);
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
    const uint32_t egressNumQueues = egressQueue->GetNumQueues();
    const uint32_t egressMaxPacketsPerQueue = egressQueue->GetMaxPacketsPerQueue();
    if (egressNumQueues == 0 || egressMaxPacketsPerQueue == 0)
    {
        NS_FATAL_ERROR("Bridge found invalid RRQ egress config (NumQueues/MaxPacketsPerQueue == 0). "
                       "port=" << Mac48Address::ConvertFrom(bridgePort->GetAddress()));
    }

    BridgeIngressQueueState ingressState;
    ingressState.numQueues = 1;
    ingressState.maxPacketsPerQueue = egressNumQueues * egressMaxPacketsPerQueue;
    ingressState.pauseQueueCount = egressNumQueues;
    ingressState.subQueueLengths.assign(1, 0);
    ingressState.subQueueDropPackets.assign(1, 0);
    m_ingressQueueStates[bridgePort] = ingressState;

    auto localIngressIt = m_ingressQueueStates.find(nullptr);
    if (localIngressIt == m_ingressQueueStates.end())
    {
        // nullptr key is a sentinel for locally injected packets via bridge SendFrom().
        BridgeIngressQueueState localIngressState;
        m_ingressQueueStates[nullptr] = localIngressState;
        localIngressIt = m_ingressQueueStates.find(nullptr);
    }
    if (localIngressIt != m_ingressQueueStates.end())
    {
        // Keep local ingress queue config aligned with bridge egress queue config.
        localIngressIt->second.numQueues = 1;
        localIngressIt->second.maxPacketsPerQueue = egressNumQueues * egressMaxPacketsPerQueue;
        localIngressIt->second.pauseQueueCount = egressNumQueues;
        localIngressIt->second.subQueueLengths.assign(1, 0);
        localIngressIt->second.subQueueDropPackets.assign(1, 0);
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

    if (localInjection)
    {
        RdmaDispatchTag dispatchTag;
        if (packet->PeekPacketTag(dispatchTag))
        {
            Ptr<PointToPointNetDevice> p2pOutPort = DynamicCast<PointToPointNetDevice>(outPort);
            Ptr<RdmaPortEngine> portEngine =
                p2pOutPort ? p2pOutPort->GetObject<RdmaPortEngine>() : nullptr;
            if (portEngine &&
                portEngine->SubmitFromBridgeEgress(src, dst, protocol, packet->Copy()))
            {
                return true;
            }
        }
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
