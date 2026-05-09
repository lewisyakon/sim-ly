/*
 * Copyright (c) 2007, 2008 University of Washington
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "point-to-point-net-device.h"

#include "point-to-point-channel.h"
#include "ppp-header.h"

#include "ns3/error-model.h"
#include "ns3/llc-snap-header.h"
#include "ns3/log.h"
#include "ns3/mac48-address.h"
#include "ns3/pointer.h"
#include "ns3/queue.h"
#include "ns3/simulator.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/uinteger.h"
#include "ns3/pause-header.h"
#include "ns3/PppRoundRobinQueue.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("PointToPointNetDevice");

NS_OBJECT_ENSURE_REGISTERED(PointToPointNetDevice);

namespace {
struct P2pDropStats
{
    uint64_t linkDownDrops = 0;
    uint64_t enqueueFailDrops = 0;
    uint64_t enqueueFailDropBytes = 0;
    bool summaryScheduled = false;
};

P2pDropStats g_p2pDropStats;

void
PrintP2pDropSummary()
{
    NS_LOG_UNCOND("[DROP][SUMMARY][P2P] linkDownDrops=" << g_p2pDropStats.linkDownDrops
                  << " enqueueFailDrops=" << g_p2pDropStats.enqueueFailDrops
                  << " enqueueFailDropBytes=" << g_p2pDropStats.enqueueFailDropBytes);
}

void
EnsureP2pDropSummaryScheduled()
{
    if (!g_p2pDropStats.summaryScheduled)
    {
        g_p2pDropStats.summaryScheduled = true;
        Simulator::ScheduleDestroy(&PrintP2pDropSummary);
    }
}
} // namespace

TypeId
PointToPointNetDevice::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::PointToPointNetDevice")
            .SetParent<NetDevice>()
            .SetGroupName("PointToPoint")
            .AddConstructor<PointToPointNetDevice>()
            .AddAttribute("Mtu",
                          "The MAC-level Maximum Transmission Unit",
                          UintegerValue(DEFAULT_MTU),
                          MakeUintegerAccessor(&PointToPointNetDevice::SetMtu,
                                               &PointToPointNetDevice::GetMtu),
                          MakeUintegerChecker<uint16_t>())
            .AddAttribute("Address",
                          "The MAC address of this device.",
                          Mac48AddressValue(Mac48Address("ff:ff:ff:ff:ff:ff")),
                          MakeMac48AddressAccessor(&PointToPointNetDevice::m_address),
                          MakeMac48AddressChecker())
            .AddAttribute("DataRate",
                          "The default data rate for point to point links",
                          DataRateValue(DataRate("32768b/s")),
                          MakeDataRateAccessor(&PointToPointNetDevice::m_bps),
                          MakeDataRateChecker())
            .AddAttribute("ReceiveErrorModel",
                          "The receiver error model used to simulate packet loss",
                          PointerValue(),
                          MakePointerAccessor(&PointToPointNetDevice::m_receiveErrorModel),
                          MakePointerChecker<ErrorModel>())
            .AddAttribute("InterframeGap",
                          "The time to wait between packet (frame) transmissions",
                          TimeValue(Seconds(0)),
                          MakeTimeAccessor(&PointToPointNetDevice::m_tInterframeGap),
                          MakeTimeChecker())

            //
            // Transmit queueing discipline for the device which includes its own set
            // of trace hooks.
            //
            .AddAttribute("TxQueue",
                          "A queue to use as the transmit queue in the device.",
                          PointerValue(),
                          MakePointerAccessor(&PointToPointNetDevice::m_queue),
                          MakePointerChecker<Queue<Packet>>())

            //
            // Trace sources at the "top" of the net device, where packets transition
            // to/from higher layers.
            //
            .AddTraceSource("MacTx",
                            "Trace source indicating a packet has arrived "
                            "for transmission by this device",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_macTxTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("MacTxDrop",
                            "Trace source indicating a packet has been dropped "
                            "by the device before transmission",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_macTxDropTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("MacPromiscRx",
                            "A packet has been received by this device, "
                            "has been passed up from the physical layer "
                            "and is being forwarded up the local protocol stack.  "
                            "This is a promiscuous trace,",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_macPromiscRxTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("MacRx",
                            "A packet has been received by this device, "
                            "has been passed up from the physical layer "
                            "and is being forwarded up the local protocol stack.  "
                            "This is a non-promiscuous trace,",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_macRxTrace),
                            "ns3::Packet::TracedCallback")
#if 0
    // Not currently implemented for this device
    .AddTraceSource ("MacRxDrop",
                     "Trace source indicating a packet was dropped "
                     "before being forwarded up the stack",
                     MakeTraceSourceAccessor (&PointToPointNetDevice::m_macRxDropTrace),
                     "ns3::Packet::TracedCallback")
#endif
            //
            // Trace sources at the "bottom" of the net device, where packets transition
            // to/from the channel.
            //
            .AddTraceSource("PhyTxBegin",
                            "Trace source indicating a packet has begun "
                            "transmitting over the channel",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyTxBeginTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PhyTxEnd",
                            "Trace source indicating a packet has been "
                            "completely transmitted over the channel",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyTxEndTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PhyTxDrop",
                            "Trace source indicating a packet has been "
                            "dropped by the device during transmission",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyTxDropTrace),
                            "ns3::Packet::TracedCallback")
#if 0
    // Not currently implemented for this device
    .AddTraceSource ("PhyRxBegin",
                     "Trace source indicating a packet has begun "
                     "being received by the device",
                     MakeTraceSourceAccessor (&PointToPointNetDevice::m_phyRxBeginTrace),
                     "ns3::Packet::TracedCallback")
#endif
            .AddTraceSource("PhyRxEnd",
                            "Trace source indicating a packet has been "
                            "completely received by the device",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyRxEndTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PhyRxDrop",
                            "Trace source indicating a packet has been "
                            "dropped by the device during reception",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_phyRxDropTrace),
                            "ns3::Packet::TracedCallback")

            //
            // Trace sources designed to simulate a packet sniffer facility (tcpdump).
            // Note that there is really no difference between promiscuous and
            // non-promiscuous traces in a point-to-point link.
            //
            .AddTraceSource("Sniffer",
                            "Trace source simulating a non-promiscuous packet sniffer "
                            "attached to the device",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_snifferTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("PromiscSniffer",
                            "Trace source simulating a promiscuous packet sniffer "
                            "attached to the device",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_promiscSnifferTrace),
                            "ns3::Packet::TracedCallback")
            .AddTraceSource("HighWatermark",
                            "Callback triggered when queue exceed the highwatermark "
                            "send pause",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_highWatermarkCallback),
                            "ns3::TracedCallback")
            .AddTraceSource("LowWatermark",
                            "Callback triggered when queue drops below the lowwatermark "
                            "send resume",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_lowWatermarkCallback),
                            "ns3::TracedCallback")
            .AddTraceSource("QueuePauseState",
                            "Callback triggered when a specific transmit queue enters or exits pause.",
                            MakeTraceSourceAccessor(&PointToPointNetDevice::m_queuePauseStateTrace),
                            "ns3::TracedCallback::Uint32Bool");
                                 
    return tid;
}

PointToPointNetDevice::PointToPointNetDevice()
    : m_txMachineState(READY),
      m_channel(nullptr),
      m_linkUp(false),
      m_PauseCount(0),
    //   m_peerIsPaused(false),
      m_isPaused(false),
      m_highWaterMark(0.8),
      m_lowWaterMark(0.2),
      m_currentPktFromSubQueue(false),
      m_currentPktSubQueueId(0),
      m_currentPkt(nullptr)
{
    NS_LOG_FUNCTION(this);
}

PointToPointNetDevice::~PointToPointNetDevice()
{
    NS_LOG_FUNCTION(this);
}

void
PointToPointNetDevice::AddHeader(Ptr<Packet> p, uint16_t protocolNumber)
{
    NS_LOG_FUNCTION(this << p << protocolNumber);
    PppHeader ppp;
    ppp.SetProtocol(EtherToPpp(protocolNumber));
    p->AddHeader(ppp);
}
void
PointToPointNetDevice::AddHeader(Ptr<Packet> p, uint16_t protocolNumber,const Address& source,const Address& dest)
{
    NS_LOG_FUNCTION(this << p << protocolNumber);
    PppHeader ppp;
    ppp.SetProtocol(EtherToPpp(protocolNumber));
    ppp.SetSrcMac(source);
    ppp.SetDstMac(dest);
    p->AddHeader(ppp);
}

bool
PointToPointNetDevice::ProcessHeader(Ptr<Packet> p, uint16_t& param)
{
    NS_LOG_FUNCTION(this << p << param);
    PppHeader ppp;
    p->RemoveHeader(ppp);
    param = PppToEther(ppp.GetProtocol());
    return true;
}
bool
PointToPointNetDevice::ProcessHeader(Ptr<Packet> p, uint16_t& param,Address& source,Address& dest)
{
    NS_LOG_FUNCTION(this << p << param);
    PppHeader ppp;
    p->RemoveHeader(ppp);
    param = PppToEther(ppp.GetProtocol());
    source=ppp.GetSrcMac();
    dest=ppp.GetDstMac();
    return true;
}

void
PointToPointNetDevice::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_node = nullptr;
    m_channel = nullptr;
    m_receiveErrorModel = nullptr;
    m_currentPkt = nullptr;
    m_queue = nullptr;
    NetDevice::DoDispose();
}

void
PointToPointNetDevice::SetDataRate(DataRate bps)
{
    NS_LOG_FUNCTION(this);
    m_bps = bps;
}

void
PointToPointNetDevice::SetInterframeGap(Time t)
{
    NS_LOG_FUNCTION(this << t.As(Time::S));
    m_tInterframeGap = t;
}

void
PointToPointNetDevice::AddPauseCount(uint32_t increment)
{
    m_PauseCount += increment;
}

void
PointToPointNetDevice::SubPauseCount(uint32_t decrement)
{
    if (decrement >= m_PauseCount)
    {
        m_PauseCount = 0;
        return;
    }
    m_PauseCount -= decrement;
}

void
PointToPointNetDevice::TryStartNextTransmission()
{
    if (!m_queue || m_txMachineState != READY)
    {
        return;
    }

    // Under pause we only allow control frames (0x8808) to drain.
    if (m_isPaused)
    {
        Ptr<const Packet> head = m_queue->Peek();
        if (!head)
        {
            return;
        }
        Ptr<Packet> headCopy = head->Copy();
        uint16_t headProto = 0;
        if (!ProcessHeader(headCopy, headProto) || headProto != 0x8808)
        {
            return;
        }
    }

    Ptr<Packet> p = m_queue->Dequeue();
    if (!p)
    {
        return;
    }

    m_currentPktFromSubQueue = false;
    m_currentPktSubQueueId = 0;
    Ptr<PppRoundRobinQueue<Packet>> rrQueue = DynamicCast<PppRoundRobinQueue<Packet>>(m_queue);
    if (rrQueue)
    {
        uint32_t qid = 0;
        if (rrQueue->GetLastDequeuedSubQueueId(qid))
        {
            m_currentPktFromSubQueue = true;
            m_currentPktSubQueueId = qid;
        }
    }

    m_snifferTrace(p);
    m_promiscSnifferTrace(p);
    TransmitStart(p);
}

bool
PointToPointNetDevice::TransmitStart(Ptr<Packet> p)
{
    NS_LOG_FUNCTION(this << p);
    NS_LOG_LOGIC("UID is " << p->GetUid() << ")");

    //
    // This function is called to start the process of transmitting a packet.
    // We need to tell the channel that we've started wiggling the wire and
    // schedule an event that will be executed when the transmission is complete.
    //
    NS_ASSERT_MSG(m_txMachineState == READY, "Must be READY to transmit");
    m_txMachineState = BUSY;
    m_currentPkt = p;
    m_phyTxBeginTrace(m_currentPkt);

    Time txTime = m_bps.CalculateBytesTxTime(p->GetSize());
    Time txCompleteTime = txTime + m_tInterframeGap;

    NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.As(Time::S));
    Simulator::Schedule(txCompleteTime, &PointToPointNetDevice::TransmitComplete, this);

    bool result = m_channel->TransmitStart(p, this, txTime);
    if (!result)
    {
        m_phyTxDropTrace(p);
    }
    return result;
}

void
PointToPointNetDevice::TransmitComplete()
{
    NS_LOG_FUNCTION(this);

    //
    // This function is called to when we're all done transmitting a packet.
    // We try and pull another packet off of the transmit queue.  If the queue
    // is empty, we are done, otherwise we need to start transmitting the
    // next packet.
    //
    NS_ASSERT_MSG(m_txMachineState == BUSY, "Must be BUSY if transmitting");
    m_txMachineState = READY;

    NS_ASSERT_MSG(m_currentPkt, "PointToPointNetDevice::TransmitComplete(): m_currentPkt zero");

    m_phyTxEndTrace(m_currentPkt);

    if (m_queue && m_PauseCount > 0 && m_currentPktFromSubQueue)
    {
        m_lowWatermarkCallback(m_currentPktSubQueueId);
    }
    m_currentPktFromSubQueue = false;
    m_currentPktSubQueueId = 0;
    m_currentPkt = nullptr;

    TryStartNextTransmission();
}

bool
PointToPointNetDevice::Attach(Ptr<PointToPointChannel> ch)
{
    NS_LOG_FUNCTION(this << &ch);

    m_channel = ch;

    m_channel->Attach(this);

    //
    // This device is up whenever it is attached to a channel.  A better plan
    // would be to have the link come up when both devices are attached, but this
    // is not done for now.
    //
    NotifyLinkUp();
    return true;
}

void
PointToPointNetDevice::SetQueue(Ptr<Queue<Packet>> q)
{
    NS_LOG_FUNCTION(this << q);
    m_queue = q;
}

void
PointToPointNetDevice::SetReceiveErrorModel(Ptr<ErrorModel> em)
{
    NS_LOG_FUNCTION(this << em);
    m_receiveErrorModel = em;
}

void
PointToPointNetDevice::Receive(Ptr<Packet> packet)
{
    // NS_LOG_FUNCTION(this << packet);
    // NS_LOG_INFO("At time: " << Simulator::Now().GetSeconds() << "s, Device "
    //             << Mac48Address::ConvertFrom(m_address) << " Receive Pkt " << packet->GetSize());
    uint16_t protocol = 0;
    Address source,dest;

    if (m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet))
    {
        //
        // If we have an error model and it indicates that it is time to lose a
        // corrupted packet, don't forward this packet up, let it go.
        //
        m_phyRxDropTrace(packet);
    }
    else
    {
        //
        // Hit the trace hooks.  All of these hooks are in the same place in this
        // device because it is so simple, but this is not usually the case in
        // more complicated devices.
        //
        m_snifferTrace(packet);
        m_promiscSnifferTrace(packet);
        m_phyRxEndTrace(packet);

        //
        // Trace sinks will expect complete packets, not packets without some of the
        // headers.
        //
        Ptr<Packet> originalPacket = packet->Copy();

        //
        // Strip off the point-to-point protocol header and forward this packet
        // up the protocol stack.  Since this is a simple point-to-point link,
        // there is no difference in what the promisc callback sees and what the
        // normal receive callback sees.
        //
        ProcessHeader(packet, protocol,source,dest);

        PauseHeader header;
        uint32_t h_size = packet -> PeekHeader(header);
        if(h_size > 0)
        {
            packet -> RemoveHeader(header);
            Mac48Address pauseDst = header.GetDstAddr();
            uint16_t pauseQid = header.GetQueueId();
            if(header.GetFrameType() == PauseHeader::PAUSE && pauseDst == m_address)
            {
                if (IsPfcFullLogEnabled())
                {
                    NS_LOG_UNCOND("[PFC][RX][NONLOCAL][PAUSE] dev=" << Mac48Address::ConvertFrom(m_address)
                                  << " qid=" << pauseQid
                                  << " t=" << Simulator::Now().GetSeconds() << "s");
                }
                Ptr<PppRoundRobinQueue<Packet>> rrQueue =
                    DynamicCast<PppRoundRobinQueue<Packet>>(m_queue);
                if (rrQueue)
                {
                    rrQueue->PauseSubQueue(pauseQid);
                    m_queuePauseStateTrace(pauseQid, true);
                }
                else
                {
                    m_isPaused = true;
                    m_queuePauseStateTrace(0, true);
                }
                return ;
            }
            else if(header.GetFrameType() == PauseHeader::RESUME && pauseDst == m_address)
            {
                if (IsPfcFullLogEnabled())
                {
                    NS_LOG_UNCOND("[PFC][RX][NONLOCAL][RESUME] dev=" << Mac48Address::ConvertFrom(m_address)
                                  << " qid=" << pauseQid
                                  << " t=" << Simulator::Now().GetSeconds() << "s");
                }
                Ptr<PppRoundRobinQueue<Packet>> rrQueue =
                    DynamicCast<PppRoundRobinQueue<Packet>>(m_queue);
                if (rrQueue)
                {
                    rrQueue->ResumeSubQueue(pauseQid);
                    m_queuePauseStateTrace(pauseQid, false);
                }
                else
                {
                    m_isPaused = false;
                    m_queuePauseStateTrace(0, false);
                }
                TryStartNextTransmission();
                return ;
            }
        }

        if (!m_promiscCallback.IsNull())
        {
            m_macPromiscRxTrace(originalPacket);
            // m_promiscCallback(this,
            //                   packet,
            //                   protocol,
            //                   GetRemote(),
            //                   GetAddress(),
            //                   NetDevice::PACKET_HOST);
            m_promiscCallback(this,
                              packet,
                              protocol,
                              source,
                              dest,
                              NetDevice::PACKET_OTHERHOST);
        }

        m_macRxTrace(originalPacket);
        // m_rxCallback(this, packet, protocol, GetRemote());
        m_rxCallback(this, packet, protocol, source);
    }
}

Ptr<Queue<Packet>>
PointToPointNetDevice::GetQueue() const
{
    NS_LOG_FUNCTION(this);
    return m_queue;
}

Address
PointToPointNetDevice::GetPeerAddress() const
{
    return GetRemote();
}

void
PointToPointNetDevice::NotifyLinkUp()
{
    NS_LOG_FUNCTION(this);
    m_linkUp = true;
    m_linkChangeCallbacks();
}

void
PointToPointNetDevice::SetIfIndex(const uint32_t index)
{
    NS_LOG_FUNCTION(this);
    m_ifIndex = index;
}

uint32_t
PointToPointNetDevice::GetIfIndex() const
{
    return m_ifIndex;
}

Ptr<Channel>
PointToPointNetDevice::GetChannel() const
{
    return m_channel;
}

//
// This is a point-to-point device, so we really don't need any kind of address
// information.  However, the base class NetDevice wants us to define the
// methods to get and set the address.  Rather than be rude and assert, we let
// clients get and set the address, but simply ignore them.

void
PointToPointNetDevice::SetAddress(Address address)
{
    NS_LOG_FUNCTION(this << address);
    m_address = Mac48Address::ConvertFrom(address);
}

Address
PointToPointNetDevice::GetAddress() const
{
    return m_address;
}

bool
PointToPointNetDevice::IsLinkUp() const
{
    NS_LOG_FUNCTION(this);
    return m_linkUp;
}

void
PointToPointNetDevice::AddLinkChangeCallback(Callback<void> callback)
{
    NS_LOG_FUNCTION(this);
    m_linkChangeCallbacks.ConnectWithoutContext(callback);
}

//
// This is a point-to-point device, so every transmission is a broadcast to
// all of the devices on the network.
//
bool
PointToPointNetDevice::IsBroadcast() const
{
    NS_LOG_FUNCTION(this);
    return true;
}

//
// We don't really need any addressing information since this is a
// point-to-point device.  The base class NetDevice wants us to return a
// broadcast address, so we make up something reasonable.
//
Address
PointToPointNetDevice::GetBroadcast() const
{
    NS_LOG_FUNCTION(this);
    return Mac48Address::GetBroadcast();
}

bool
PointToPointNetDevice::IsMulticast() const
{
    NS_LOG_FUNCTION(this);
    return true;
}

Address
PointToPointNetDevice::GetMulticast(Ipv4Address multicastGroup) const
{
    NS_LOG_FUNCTION(this);
    return Mac48Address("01:00:5e:00:00:00");
}

Address
PointToPointNetDevice::GetMulticast(Ipv6Address addr) const
{
    NS_LOG_FUNCTION(this << addr);
    return Mac48Address("33:33:00:00:00:00");
}

bool
PointToPointNetDevice::IsPointToPoint() const
{
    NS_LOG_FUNCTION(this);
    return true;
}

bool
PointToPointNetDevice::IsBridge() const
{
    NS_LOG_FUNCTION(this);
    return false;
}

bool
PointToPointNetDevice::Send(Ptr<Packet> packet, const Address& dest, uint16_t protocolNumber)
{
    // NS_LOG_FUNCTION(this << packet << dest << protocolNumber);
    // NS_LOG_LOGIC("p=" << packet << ", dest=" << &dest);
    // NS_LOG_LOGIC("UID is " << packet->GetUid());

    //
    // If IsLinkUp() is false it means there is no channel to send any packet
    // over so we just hit the drop trace on the packet and return an error.
    //
    if (!IsLinkUp())
    {
        EnsureP2pDropSummaryScheduled();
        ++g_p2pDropStats.linkDownDrops;
        static uint64_t sLinkDownLogCount = 0;
        if (sLinkDownLogCount < 20 || (sLinkDownLogCount % 1000) == 0)
        {
            NS_LOG_UNCOND("[DROP][P2P][LINK_DOWN] dev=" << Mac48Address::ConvertFrom(m_address)
                          << " src=" << Mac48Address::ConvertFrom(m_address)
                          << " dst=" << Mac48Address::ConvertFrom(dest)
                          << " proto=0x" << std::hex << protocolNumber << std::dec
                          << " totalLinkDownDrops=" << g_p2pDropStats.linkDownDrops);
        }
        ++sLinkDownLogCount;
        m_macTxDropTrace(packet);
        return false;
    }

    // Stick a point to point protocol header on the packet in preparation for
    // shoving it out the door.
    //
    // AddHeader(packet, protocolNumber);
    AddHeader(packet, protocolNumber,m_address,dest);

    m_macTxTrace(packet);
    
    // NS_LOG_INFO("***At time: " << Simulator::Now().GetSeconds() << "s, Dev: " 
    //             << Mac48Address::ConvertFrom(m_address) << " send "
    //             << "Pkt " << packet->GetUid() << " to " << Mac48Address::ConvertFrom(dest));    

    //
    // We should enqueue and dequeue the packet to hit the tracing hooks.
    //
    if (m_queue->Enqueue(packet))
    {
        TryStartNextTransmission();
        return true;
    }

    // Enqueue may fail (overflow)
    EnsureP2pDropSummaryScheduled();
    ++g_p2pDropStats.enqueueFailDrops;
    g_p2pDropStats.enqueueFailDropBytes += packet->GetSize();
    static uint64_t sEnqueueFailLogCount = 0;
    if (sEnqueueFailLogCount < 20 || (sEnqueueFailLogCount % 1000) == 0)
    {
        NS_LOG_UNCOND("[DROP][P2P][ENQUEUE_FAIL] dev=" << Mac48Address::ConvertFrom(m_address)
                      << " src=" << Mac48Address::ConvertFrom(m_address)
                      << " dst=" << Mac48Address::ConvertFrom(dest)
                      << " proto=0x" << std::hex << protocolNumber << std::dec
                      << " queuePkts=" << (m_queue ? m_queue->GetNPackets() : 0)
                      << " pktSize=" << packet->GetSize()
                      << " totalEnqueueFailDrops=" << g_p2pDropStats.enqueueFailDrops
                      << " totalEnqueueFailDropBytes=" << g_p2pDropStats.enqueueFailDropBytes);
    }
    ++sEnqueueFailLogCount;

    m_macTxDropTrace(packet);
    return false;
}

bool
PointToPointNetDevice::SendFrom(Ptr<Packet> packet,
                                const Address& source,
                                const Address& dest,
                                uint16_t protocolNumber)
{
    NS_LOG_FUNCTION(this << packet << source << dest << protocolNumber);
    //
    // If IsLinkUp() is false it means there is no channel to send any packet
    // over so we just hit the drop trace on the packet and return an error.
    //
    if (!IsLinkUp())
    {
        EnsureP2pDropSummaryScheduled();
        ++g_p2pDropStats.linkDownDrops;
        static uint64_t sLinkDownLogCount = 0;
        if (sLinkDownLogCount < 20 || (sLinkDownLogCount % 1000) == 0)
        {
            NS_LOG_UNCOND("[DROP][P2P][LINK_DOWN] dev=" << Mac48Address::ConvertFrom(m_address)
                          << " src=" << Mac48Address::ConvertFrom(source)
                          << " dst=" << Mac48Address::ConvertFrom(dest)
                          << " proto=0x" << std::hex << protocolNumber << std::dec
                          << " totalLinkDownDrops=" << g_p2pDropStats.linkDownDrops);
        }
        ++sLinkDownLogCount;
        m_macTxDropTrace(packet);
        return false;
    }

    //
    // Stick a point to point protocol header on the packet in preparation for
    // shoving it out the door.
    //
    AddHeader(packet, protocolNumber,source,dest);

    m_macTxTrace(packet);

    //
    // We should enqueue and dequeue the packet to hit the tracing hooks.
    //
    if (m_queue->Enqueue(packet))
    {
        TryStartNextTransmission();
        return true;
    }

    // Enqueue may fail (overflow)
    EnsureP2pDropSummaryScheduled();
    ++g_p2pDropStats.enqueueFailDrops;
    g_p2pDropStats.enqueueFailDropBytes += packet->GetSize();
    static uint64_t sEnqueueFailLogCount = 0;
    if (sEnqueueFailLogCount < 20 || (sEnqueueFailLogCount % 1000) == 0)
    {
        NS_LOG_UNCOND("[DROP][P2P][ENQUEUE_FAIL] dev=" << Mac48Address::ConvertFrom(m_address)
                      << " src=" << Mac48Address::ConvertFrom(source)
                      << " dst=" << Mac48Address::ConvertFrom(dest)
                      << " proto=0x" << std::hex << protocolNumber << std::dec
                      << " queuePkts=" << (m_queue ? m_queue->GetNPackets() : 0)
                      << " pktSize=" << packet->GetSize()
                      << " totalEnqueueFailDrops=" << g_p2pDropStats.enqueueFailDrops
                      << " totalEnqueueFailDropBytes=" << g_p2pDropStats.enqueueFailDropBytes);
    }
    ++sEnqueueFailLogCount;

    m_macTxDropTrace(packet);
    // return false;
    return false;
}

Ptr<Node>
PointToPointNetDevice::GetNode() const
{
    return m_node;
}

void
PointToPointNetDevice::SetNode(Ptr<Node> node)
{
    NS_LOG_FUNCTION(this);
    m_node = node;
}

bool
PointToPointNetDevice::NeedsArp() const
{
    NS_LOG_FUNCTION(this);
    return false;
}

void
PointToPointNetDevice::SetReceiveCallback(NetDevice::ReceiveCallback cb)
{
    m_rxCallback = cb;
}

void
PointToPointNetDevice::SetPromiscReceiveCallback(NetDevice::PromiscReceiveCallback cb)
{
    m_promiscCallback = cb;
}

bool
PointToPointNetDevice::SupportsSendFrom() const
{
    NS_LOG_FUNCTION(this);
    // return false;
    return true;
}

void
PointToPointNetDevice::DoMpiReceive(Ptr<Packet> p)
{
    NS_LOG_FUNCTION(this << p);
    Receive(p);
}

Address
PointToPointNetDevice::GetRemote() const
{
    NS_LOG_FUNCTION(this);
    NS_ASSERT(m_channel->GetNDevices() == 2);
    for (std::size_t i = 0; i < m_channel->GetNDevices(); ++i)
    {
        Ptr<NetDevice> tmp = m_channel->GetDevice(i);
        if (tmp != this)
        {
            return tmp->GetAddress();
        }
    }
    NS_ASSERT(false);
    // quiet compiler.
    return Address();
}

bool
PointToPointNetDevice::SetMtu(uint16_t mtu)
{
    NS_LOG_FUNCTION(this << mtu);
    m_mtu = mtu;
    return true;
}

uint16_t
PointToPointNetDevice::GetMtu() const
{
    NS_LOG_FUNCTION(this);
    return m_mtu;
}

uint16_t
PointToPointNetDevice::PppToEther(uint16_t proto)
{
    NS_LOG_FUNCTION_NOARGS();
    switch (proto)
    {
    case 0x0021:
        return 0x0800; // IPv4
    case 0x0057:
        return 0x86DD; // IPv6
    case 0x9999:
        return 0x9999; // 自定义的ECMP
    default:
        // NS_ASSERT_MSG(false, "PPP Protocol number not defined!");
        return proto;
    }
    return 0;
}

uint16_t
PointToPointNetDevice::EtherToPpp(uint16_t proto)
{
    NS_LOG_FUNCTION_NOARGS();
    switch (proto)
    {
    case 0x0800:
        return 0x0021; // IPv4
    case 0x86DD:
        return 0x0057; // IPv6
    case 0x9999:
        return 0x9999; // 自定义的ECMP
    default:
        // NS_ASSERT_MSG(false, "PPP Protocol number not defined!");
        return proto;
    }
    return 0;
}

} // namespace ns3
