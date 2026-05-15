#ifndef MIN_HOP_BRIDGE_NET_DEVICE_H
#define MIN_HOP_BRIDGE_NET_DEVICE_H

#include <map>
#include <set>
#include <vector>
#include <fstream>
#include <string>
#include "ns3/channel.h"
#include "ns3/event-id.h"
#include "ns3/mac48-address.h"
#include "ns3/net-device.h"
#include "ns3/traced-callback.h"
#include "ns3/nstime.h"

// 包含自定义的跳数头部
#include "hop-count-header.h"

namespace ns3
{

class BridgeChannel;
class Packet;
template <typename Item>
class PppRoundRobinQueue;

/**
 * @ingroup bridge
 * @brief A custom bridge NetDevice that forwards packets based on the minimum hop count
 * and drops packets if hop count exceeds a limit.
 *
 * This device learns which port(s) provide the path with the minimum hop count
 * from a source MAC address. It can forward packets along multiple, equally-short
 * paths. It also implements a TTL-like mechanism by dropping packets when their
 * hop count reaches a threshold (>= 4).
 */
class MinHopBridgeNetDevice : public NetDevice
{
  public:
    static TypeId GetTypeId();
    MinHopBridgeNetDevice();
    ~MinHopBridgeNetDevice() override;

    // These methods are part of the NetDevice API
    void SetIfIndex(const uint32_t index) override;
    uint32_t GetIfIndex() const override;
    Ptr<Channel> GetChannel() const override;
    void SetAddress(Address address) override;
    Address GetAddress() const override;
    bool SetMtu(const uint16_t mtu) override;
    uint16_t GetMtu() const override;
    bool IsLinkUp() const override;
    void AddLinkChangeCallback(Callback<void> callback) override;
    bool IsBroadcast() const override;
    Address GetBroadcast() const override;
    bool IsMulticast() const override;
    Address GetMulticast(Ipv4Address multicastGroup) const override;
    Address GetMulticast(Ipv6Address addr) const override;
    bool IsPointToPoint() const override;
    bool IsBridge() const override;
    bool Send(Ptr<Packet> packet, const Address& dest, uint16_t protocolNumber) override;
    bool SendFrom(Ptr<Packet> packet,
                  const Address& src,
                  const Address& dest,
                  uint16_t protocolNumber) override;
    Ptr<Node> GetNode() const override;
    void SetNode(Ptr<Node> node) override;
    bool NeedsArp() const override;
    void SetReceiveCallback(ReceiveCallback cb) override;
    void SetPromiscReceiveCallback(PromiscReceiveCallback cb) override;
    bool SupportsSendFrom() const override;

    // Bridge-specific methods
    void AddBridgePort(Ptr<NetDevice> bridgePort);
    uint32_t GetNBridgePorts() const;
    Ptr<NetDevice> GetBridgePort(uint32_t n) const;
    void PrintLearnedState() const;
    void Print_LocalMacAddress() const;
    static void PrintFloodWindowSummary(bool printToStdout = false);
    
    void AddEntry(Mac48Address mac, const std::vector<uint32_t>& ports);
    void set_ecmp_model(int model);
    enum class EcmpTopologyLayer : uint8_t
    {
        Unknown = 0,
        Core = 1,
        Aggregation = 2,
        Edge = 3
    };
    void SetFatTreeEcmpContext(uint32_t k,
                               uint32_t gpusPerServer,
                               EcmpTopologyLayer layer,
                               uint32_t layerIndex);

    void set_maxjump(int jump);
    
    void Set_switchDelay(Time t);

    bool writemactable(std::string filepath);
    bool readmactable(std::string filepath);

    void hostsend(Ptr<NetDevice> incomingPort,Ptr<const Packet> packet,uint16_t protocol,
                    const Address& src,const Address& dst);

    struct IngressQueueSnapshot
    {
        uint32_t portIndex;
        Mac48Address portMac;
        uint32_t maxPacketsPerQueue;
        std::vector<uint32_t> subQueueLengths;
        std::vector<uint64_t> subQueueDropPackets;
    };
    std::vector<IngressQueueSnapshot> GetIngressQueueSnapshots() const;
    std::vector<Ptr<NetDevice>> GetLocalInjectionEgressCandidates(const Address& dst);
    Ptr<NetDevice> ResolveLocalInjectionEgressPort(const Address& src,
                                                   const Address& dst,
                                                   uint64_t uidHint = 0);
    
  protected:
    void DoDispose() override;

  private:
    /**
     * \brief State learned by the bridge for a given MAC address.
     */
    struct LearnedState
    {
        Ptr<NetDevice> associatedPort;
        /** The minimum hop count discovered for this MAC address. */
        uint8_t minHopCount;
        /** The expiration time for this entry. */
        Time expirationTime;

        LearnedState() : minHopCount(255)
        {
        } // Initialize with a large value
    };

    struct BridgeIngressQueueState
    {
        uint32_t numQueues = 0;
        uint32_t maxPacketsPerQueue = 0;
        uint32_t pauseQueueCount = 0;
        std::vector<uint32_t> subQueueLengths;
        std::vector<uint64_t> subQueueDropPackets;
    };

    struct BridgeIngressPacketRecord
    {
        Ptr<Packet> packet;
        uint16_t protocol = 0;
        Address src;
        Address dst;
        PacketType packetType = PacketType::PACKET_OTHERHOST;
        uint32_t queueId = 0;
    };

    void ScheduleReceive(Ptr<NetDevice> incomingPort,
                           Ptr<const Packet> packet,
                           uint16_t protocol,
                           const Address& src,
                           const Address& dst,
                           PacketType packetType);

    void ReceiveFromDevice(Ptr<NetDevice> incomingPort,
                           Ptr<const Packet> packet,
                           uint16_t protocol,
                           const Address& src,
                           const Address& dst,
                           PacketType packetType);
    bool EnqueueIngressPacket(Ptr<NetDevice> incomingPort,
                              Ptr<const Packet> packet,
                              uint16_t protocol,
                              const Address& src,
                              const Address& dst,
                              PacketType packetType,
                              bool addSwitchDelay = true);
    void ScheduleIngressDrainEvent(Ptr<NetDevice> incomingPort,
                                   uint64_t packetToken,
                                   Time delay);
    void ScheduleIngressDrainRetry(Ptr<NetDevice> incomingPort,
                                   uint64_t packetToken);
    void DrainIngressPacket(Ptr<NetDevice> incomingPort, uint64_t packetToken);
    uint32_t GetIngressQueueId(Ptr<NetDevice> ingressPort, Mac48Address dst);
    bool HasAvailableUnicastPath(Ptr<NetDevice> incomingPort, Mac48Address dst);
    bool HasAvailableFloodPath(Ptr<NetDevice> incomingPort, Mac48Address dst);
    bool IsEgressQueueCongested(Ptr<NetDevice> egressPort, Mac48Address dst, bool anticipateEnqueue);
    bool RefreshEgressQueueCongestionStateByQid(Ptr<NetDevice> egressPort,
                                                uint32_t queueId,
                                                bool anticipateEnqueue);
    void UpdateIngressPauseStateOnEnqueue(Ptr<NetDevice> ingressPort, Mac48Address dst);
    void UpdateIngressPauseStateOnDequeue(Ptr<NetDevice> ingressPort, uint32_t queueId);
    PacketType InferPacketTypeFromDst(const Address& dst, PacketType fallback) const;

    void ForwardUnicast(Ptr<NetDevice> incomingPort,
                        Ptr<const Packet> packet,
                        uint16_t protocol,
                        Mac48Address src,
                        Mac48Address dst);

    void ForwardBroadcast(Ptr<NetDevice> incomingPort,
                          Ptr<const Packet> packet,
                          uint16_t protocol,
                          Mac48Address src,
                          Mac48Address dst);

    bool Learn(Mac48Address source, Ptr<NetDevice> port, uint8_t hopCount);
    std::vector<Ptr<NetDevice>> GetLearnedState(Mac48Address source);
    Ptr<NetDevice> ecmp(std::vector<Ptr<NetDevice> > outports,
                        Mac48Address src,
                        Mac48Address dst,
                        uint64_t uid,
                        uint32_t flowTag = 0,
                        bool hasFlowTag = false,
                        uint32_t flowSrc = 0,
                        uint32_t flowDst = 0,
                        bool hasFlowTuple = false,
                        uint32_t flowState = 0xffffffffu);

    struct FlowSprayKey
    {
        std::string balanceDomain;
        uint32_t state = 0;
        uint32_t tag = 0;
        uint32_t src = 0;
        uint32_t dst = 0;

        bool operator<(const FlowSprayKey& other) const
        {
            if (balanceDomain != other.balanceDomain)
            {
                return balanceDomain < other.balanceDomain;
            }
            if (state != other.state)
            {
                return state < other.state;
            }
            if (tag != other.tag)
            {
                return tag < other.tag;
            }
            if (src != other.src)
            {
                return src < other.src;
            }
            return dst < other.dst;
        }
    };

    /** A map of MAC address to learned state. */
    std::map<Mac48Address, std::vector<LearnedState> > m_learnState;

    Ptr<BridgeChannel> m_channel;
    std::vector<Ptr<NetDevice>> m_ports;
    Ptr<Node> m_node;
    uint32_t m_ifIndex;
    Address m_address;
    uint16_t m_mtu;
    ReceiveCallback m_rxCallback;
    PromiscReceiveCallback m_promiscRxCallback;
    Time m_expirationTime;
    bool m_enableLearning;
    bool m_terminalLearningOnly;

    double m_highWatermark;
    double m_lowWatermark;
    Time m_notHeadRetryDelay;
    bool m_bridgeDiagEnabled;
    uint64_t m_nextIngressPacketToken;
    std::map<Ptr<NetDevice>, std::set<uint32_t>> m_pausedIngressQueueIds;
    // Map ingress sub-queue -> upstream egress sub-queue id used in PAUSE/RESUME frames.
    std::map<Ptr<NetDevice>, std::map<uint32_t, uint32_t>> m_ingressPauseUpstreamQueueIds;
    std::map<Ptr<NetDevice>, std::map<uint32_t, bool>> m_egressQueueCongested;
    std::map<Ptr<NetDevice>, BridgeIngressQueueState> m_ingressQueueStates;
    std::map<Ptr<NetDevice>, std::map<uint64_t, BridgeIngressPacketRecord>> m_ingressPackets;
    std::map<Ptr<NetDevice>, std::map<uint64_t, EventId>> m_ingressRetryEvents;
    uint64_t m_ingressQueueDrops;
    uint64_t m_ingressQueueDropBytes;
    void sendPauseFrameToUpstream(Ptr<NetDevice> ingressPort, uint32_t queueId, bool pause);
    bool sendFromWithDropStats(Ptr<NetDevice> outPort,
                               Ptr<const Packet> packet,
                               const Address& src,
                               const Address& dst,
                               uint16_t protocol,
                               bool localInjection);
    uint64_t m_forwardSendFailDrops;
    uint64_t m_localSendFailDrops;

    int ecmpmodel=0;
    //0 随机发送 
    //1 packet hash
    //2 flow hash
    //3 DATA flow spray within a destination-aware balance domain; non-DATA uses model 0
    std::map<std::string, uint32_t> m_flowSprayNextPortByBalanceDomain;
    std::map<std::string, std::map<Mac48Address, uint32_t>> m_flowSprayPortLoadsByBalanceDomain;
    std::map<FlowSprayKey, Ptr<NetDevice>> m_flowSprayPorts;
    bool m_hasFatTreeEcmpContext = false;
    uint32_t m_fatTreeK = 0;
    uint32_t m_fatTreeGpusPerServer = 1;
    EcmpTopologyLayer m_ecmpTopologyLayer = EcmpTopologyLayer::Unknown;
    uint32_t m_fatTreeLayerIndex = 0;

    int maxjump=4;

    Time m_switchDelay;
    bool is_set_address;
};

} // namespace ns3

#endif /* MIN_HOP_BRIDGE_NET_DEVICE_H */
