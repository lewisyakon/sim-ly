#include "min-hop-bridge-net-device.h"
#include "hop-count-header.h"
#include "pause-header.h"
#include "flow-io.h"

#include "ns3/boolean.h"
#include "ns3/bridge-channel.h"
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

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("MinHopBridgeNetDevice");
NS_OBJECT_ENSURE_REGISTERED(MinHopBridgeNetDevice);

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
    ScheduleReceive(incomingPort,packet,protocol,src,dst,PacketType::PACKET_OTHERHOST);
}
void
MinHopBridgeNetDevice::ReceiveFromDevice(Ptr<NetDevice> incomingPort,
                                         Ptr<const Packet> packet,
                                         uint16_t protocol,
                                         const Address& src,
                                         const Address& dst,
                                         PacketType packetType)
{
    // NS_LOG_FUNCTION_NOARGS();
    // NS_LOG_DEBUG("UID is " << packet->GetUid());

    Ptr<Packet> p = packet->Copy();
    Simulator::Schedule(m_switchDelay,
                        &MinHopBridgeNetDevice::ScheduleReceive,
                        this,
                        incomingPort,
                        p,
                        protocol,
                        src,
                        dst,
                        packetType);
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
            if (newHops >= maxjump || flagmin == false)
            {
                // NS_LOG_LOGIC("Packet " << p->GetUid() << " dropped, hop count limit reached ("
                //                     << (uint32_t)newHops << " >= 4)");
                return; // Stop processing this packet
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
        ForwardBroadcast(incomingPort, p, protocol, src48, dst48);
        break;

    case PACKET_OTHERHOST:
        // Packet is unicast, but not for us. Forward it.
        if (dst48 != m_address)
        {
            if (newHops > 0)
            {
                for (auto iter = m_ports.begin(); iter != m_ports.end(); iter++)
                {
                    Ptr<NetDevice> port = *iter;
                    if (port != incomingPort)
                    {
                        maybePauseUpstreamPort(port, incomingPort, dst48);
                        sendFromWithDropStats(port, p, src, dst, protocol, false);
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
    if (ecmpmodel==0){//随机转发
        auto nowsys = std::chrono::system_clock::now();
        auto duration = nowsys.time_since_epoch();
        uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        uint32_t seed = static_cast<uint32_t>(now);
        uint32_t index = seed % outPorts.size();
        Ptr<NetDevice> outPort = outPorts[index];

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
        uint32_t index = hash % outPorts.size();
        // NS_LOG_INFO("m_address (converted to MAC): " << m_address);
        return outPorts[index];
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

        uint32_t idx = fastrange64(h, (uint32_t)outPorts.size());
        return outPorts[idx];
    }
    return nullptr;
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

    std::vector<Ptr<NetDevice> > outPort = GetLearnedState(dst);
    // for(auto p:outPort)
    // {
    //     in_to_out[p].insert(src);
    //     // NS_LOG_INFO("Insert src "<< src <<" to map of device"<< Mac48Address::ConvertFrom(p->GetAddress()));
    // }

    if (!outPort.empty())
    {
        Ptr<NetDevice> realoutport=ecmp(outPort,src,dst,packet->GetUid());

        if (realoutport!=incomingPort){
            // Detect congestion on egress and pause the corresponding upstream ingress link.
            maybePauseUpstreamPort(realoutport, incomingPort, dst);

            // NS_LOG_LOGIC("Learning bridge state says to use port `"
            //          << realoutport->GetInstanceTypeId().GetName() << "'");
            sendFromWithDropStats(realoutport, packet, src, dst, protocol, false);
        }
        
        // for (int i=0;i<outPort.size();i++){
        //     NS_LOG_LOGIC("Learning bridge state says to use port `"
        //              << outPort[i]->GetInstanceTypeId().GetName() << "'");
        //     outPort[i]->SendFrom(packet->Copy(), src, dst, protocol);    
        // }
    }
    else
    {
        // NS_LOG_LOGIC("At time:"<<Simulator::Now().GetSeconds()<< "s");
        // NS_LOG_INFO("No learned state: send through all ports");
        for (auto iter = m_ports.begin(); iter != m_ports.end(); iter++)
        {
            Ptr<NetDevice> port = *iter;
            if (port != incomingPort)
            {
                maybePauseUpstreamPort(port, incomingPort, dst);
                // NS_LOG_LOGIC("LearningBridgeForward ("
                //              << src << " => " << dst
                //              << "): " << incomingPort->GetInstanceTypeId().GetName() << " --> "
                //              << port->GetInstanceTypeId().GetName() << " (UID " << packet->GetUid()
                //              << ").");
                sendFromWithDropStats(port, packet, src, dst, protocol, false);
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

    // --- FIX: Learning is now done in ReceiveFromDevice, no need to do it here ---

    for (auto iter = m_ports.begin(); iter != m_ports.end(); iter++)
    {
        Ptr<NetDevice> port = *iter;
        if (port != incomingPort)
        {
            maybePauseUpstreamPort(port, incomingPort, dst);
            // NS_LOG_LOGIC("Flooding (" << src << " => " << dst << "): "
            //                           << " --> port " << port->GetIfIndex()
            //                           << " (UID " << packet->GetUid() << ").");
            sendFromWithDropStats(port, packet, src, dst, protocol, false);
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
    // bool connected = bridgePort -> TraceConnectWithoutContext("HighWatermark",
    //                             MakeCallback(&MinHopBridgeNetDevice::handleHighWatermark,this).Bind(bridgePort));
    // if(!connected){
    //     NS_LOG_ERROR("fail to connect highwatermark");
    // }     
    
    bool connected = bridgePort -> TraceConnectWithoutContext("LowWatermark",
                                MakeCallback(&MinHopBridgeNetDevice::handleLowWatermark,this).Bind(bridgePort));
    if(!connected){
        NS_LOG_ERROR("fail to connect lowwatermark");
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
    Mac48Address dst = Mac48Address::ConvertFrom(dest);

    if (!dst.IsGroup())
    {
        std::vector<Ptr<NetDevice>> outPorts = GetLearnedState(dst);
        if (!outPorts.empty())
        {
            bool allOk = true;
            for (auto const& outPort : outPorts)
            {
                maybePauseLocalFlowIo(outPort, dst);
                bool ok = sendFromWithDropStats(outPort, packet, src, dest, protocolNumber, true);
                allOk = ok && allOk;
            }
            return allOk;
        }
    }

    bool allOk = true;
    bool sentAny = false;
    for (auto iter = m_ports.begin(); iter != m_ports.end(); iter++)
    {
        Ptr<NetDevice> port = *iter;
        sentAny = true;
        maybePauseLocalFlowIo(port, dst);
        bool ok = sendFromWithDropStats(port, packet, src, dest, protocolNumber, true);
        allOk = ok && allOk;
    }

    return sentAny && allOk;
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
    bool ok = ingressPort->SendFrom(p, ingressPort->GetAddress(), upstreamPeer, 0x8808);
    NS_LOG_INFO("[PFC][" << (pause ? "PAUSE" : "RESUME") << "] bridge="
                << Mac48Address::ConvertFrom(GetAddress())
                << " ingressPort=" << Mac48Address::ConvertFrom(ingressPort->GetAddress())
                << " upstreamPeer=" << upstreamPeerMac
                << " qid=" << queueId
                << " sendOk=" << ok
                << " t=" << Simulator::Now().GetSeconds() << "s");
}

void
MinHopBridgeNetDevice::maybePauseUpstreamPort(Ptr<NetDevice> congestedEgress,
                                              Ptr<NetDevice> ingressPort,
                                              Mac48Address dst)
{
    if (!congestedEgress || !ingressPort || congestedEgress == ingressPort)
    {
        return;
    }

    Ptr<PointToPointNetDevice> egressP2p = DynamicCast<PointToPointNetDevice>(congestedEgress);
    if (!egressP2p)
    {
        return;
    }

    Ptr<PppRoundRobinQueue<Packet>> egressQueue =
        DynamicCast<PppRoundRobinQueue<Packet>>(egressP2p->GetQueue());
    if (!egressQueue)
    {
        return;
    }

    const uint32_t qid = egressQueue->GetQueueIndexByDst(dst);
    const uint32_t maxPerQueue = egressQueue->GetMaxPacketsPerQueue();
    if (maxPerQueue == 0)
    {
        return;
    }

    // +1 anticipates the packet we are about to enqueue to this egress sub-queue.
    double ratio = static_cast<double>(egressQueue->GetSubQueueLength(qid) + 1) /
                   static_cast<double>(maxPerQueue);
    if (ratio < m_highWatermark)
    {
        return;
    }

    auto& pausedByIngress = m_pausedIngressQueueIds[congestedEgress];
    auto& pausedQids = pausedByIngress[ingressPort];
    if (pausedQids.insert(qid).second)
    {
        egressP2p->AddPauseCount(1);
        sendPauseFrameToUpstream(ingressPort, qid, true);
    }
}

void
MinHopBridgeNetDevice::maybePauseLocalFlowIo(Ptr<NetDevice> congestedEgress, Mac48Address dst)
{
    if (!congestedEgress || !m_node)
    {
        return;
    }

    Ptr<PointToPointNetDevice> egressP2p = DynamicCast<PointToPointNetDevice>(congestedEgress);
    if (!egressP2p)
    {
        return;
    }

    Ptr<PppRoundRobinQueue<Packet>> egressQueue =
        DynamicCast<PppRoundRobinQueue<Packet>>(egressP2p->GetQueue());
    if (!egressQueue)
    {
        return;
    }

    const uint32_t qid = egressQueue->GetQueueIndexByDst(dst);
    const uint32_t maxPerQueue = egressQueue->GetMaxPacketsPerQueue();
    if (maxPerQueue == 0)
    {
        return;
    }

    // +1 anticipates the packet we are about to enqueue to this egress sub-queue.
    double ratio = static_cast<double>(egressQueue->GetSubQueueLength(qid) + 1) /
                   static_cast<double>(maxPerQueue);
    if (ratio < m_highWatermark)
    {
        return;
    }

    auto& pausedQids = m_pausedLocalQueueIds[congestedEgress];
    if (pausedQids.insert(qid).second)
    {
        egressP2p->AddPauseCount(1);
        FlowIo::PauseNodeQueue(m_node, qid);
        NS_LOG_INFO("[PFC][LOCAL][PAUSE] bridge="
                    << Mac48Address::ConvertFrom(GetAddress())
                    << " egress=" << Mac48Address::ConvertFrom(congestedEgress->GetAddress())
                    << " qid=" << qid
                    << " t=" << Simulator::Now().GetSeconds() << "s");
    }
}

void
MinHopBridgeNetDevice::handleLowWatermark(Ptr<NetDevice> port, uint32_t queueId)
{
    uint32_t count = 0;

    Ptr<PointToPointNetDevice> egressP2p = DynamicCast<PointToPointNetDevice>(port);
    Ptr<PppRoundRobinQueue<Packet>> egressQueue =
        egressP2p ? DynamicCast<PppRoundRobinQueue<Packet>>(egressP2p->GetQueue()) : nullptr;
    if (!egressQueue)
    {
        return;
    }

    const uint32_t maxPerQueue = egressQueue->GetMaxPacketsPerQueue();
    if (maxPerQueue == 0)
    {
        return;
    }

    double ratio = static_cast<double>(egressQueue->GetSubQueueLength(queueId)) /
                   static_cast<double>(maxPerQueue);
    if (ratio > m_lowWatermark)
    {
        return;
    }

    auto ingressItMap = m_pausedIngressQueueIds.find(port);
    if (ingressItMap != m_pausedIngressQueueIds.end())
    {
        for (auto& ingressEntry : ingressItMap->second)
        {
            Ptr<NetDevice> ingressPort = ingressEntry.first;
            if (ingressEntry.second.find(queueId) != ingressEntry.second.end())
            {
                ingressEntry.second.erase(queueId);
                sendPauseFrameToUpstream(ingressPort, queueId, false);
                count++;
            }
        }

        for (auto ingressIt = ingressItMap->second.begin(); ingressIt != ingressItMap->second.end();)
        {
            if (ingressIt->second.empty())
            {
                ingressIt = ingressItMap->second.erase(ingressIt);
            }
            else
            {
                ++ingressIt;
            }
        }
        if (ingressItMap->second.empty())
        {
            m_pausedIngressQueueIds.erase(ingressItMap);
        }
    }

    auto localIt = m_pausedLocalQueueIds.find(port);
    if (localIt != m_pausedLocalQueueIds.end() &&
        localIt->second.find(queueId) != localIt->second.end())
    {
        localIt->second.erase(queueId);
        FlowIo::ResumeNodeQueue(m_node, queueId);
        NS_LOG_INFO("[PFC][LOCAL][RESUME] bridge="
                    << Mac48Address::ConvertFrom(GetAddress())
                    << " egress=" << Mac48Address::ConvertFrom(port->GetAddress())
                    << " qid=" << queueId
                    << " t=" << Simulator::Now().GetSeconds() << "s");
        ++count;
        if (localIt->second.empty())
        {
            m_pausedLocalQueueIds.erase(localIt);
        }
    }

    if (egressP2p && count > 0)
    {
        egressP2p->SubPauseCount(count);
    }
}

bool
MinHopBridgeNetDevice::writemactable(std::string filepath){
    //不好写而且没意义，搁置
    std::ofstream fout(filepath); // 创建输出文件流并打开文件
    if (!fout) { // 检查是否打开成功
        NS_LOG_INFO("打开或创建mac表输出文件时失败");
        return 0;
    }
    int num_mac=m_learnState.size();
    fout<<std::to_string(num_mac)<<std::endl;
    for (const auto& [key, value] : m_learnState) {
        uint8_t buf[6];
        key.CopyTo(buf);
        std::ostringstream oss;
        for (int i = 0; i < 6; ++i) {
            if (i > 0) oss << ":"; // 加冒号分隔
            oss << std::hex << (int)buf[i];
        }
        std::string macStr = oss.str();
        fout<<macStr<<std::endl;

        fout<<value.size()<<std::endl;
        for (auto ai:value){
            fout<<ai.associatedPort->GetAddress()<<std::endl;
            fout<<ai.minHopCount<<std::endl;
        }
    }
    fout.close(); // 关闭文件
    return 1;
}

bool
MinHopBridgeNetDevice::readmactable(std::string filepath){
    //不好写而且没意义，搁置
    return 1;
}

} // namespace ns3
