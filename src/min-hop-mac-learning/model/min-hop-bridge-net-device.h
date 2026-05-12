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
                                   Ptr<Packet> packet,
                                   Time delay);
    void ScheduleIngressDrainRetry(Ptr<NetDevice> incomingPort,
                                   uint64_t packetToken,
                                   Ptr<Packet> packet);
    void ScheduleIngressNotHeadRetry(Ptr<NetDevice> incomingPort,
                                     uint64_t packetToken,
                                     Ptr<Packet> packet);
    void DrainIngressQueue(Ptr<NetDevice> incomingPort, uint64_t packetToken, Ptr<Packet> packet);
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
                        bool hasFlowTag = false);

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
    uint64_t m_nextIngressPacketToken;
    std::map<Ptr<NetDevice>, std::set<uint32_t>> m_pausedIngressQueueIds;
    // Map ingress sub-queue -> upstream egress sub-queue id used in PAUSE/RESUME frames.
    std::map<Ptr<NetDevice>, std::map<uint32_t, uint32_t>> m_ingressPauseUpstreamQueueIds;
    std::map<Ptr<NetDevice>, std::map<uint32_t, bool>> m_egressQueueCongested;
    std::map<Ptr<NetDevice>, Ptr<PppRoundRobinQueue<Packet>>> m_ingressQueues;
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
    //1 hash
    //2 best-fit

    int maxjump=4;

    Time m_switchDelay;
    bool is_set_address;
};

} // namespace ns3

#endif /* MIN_HOP_BRIDGE_NET_DEVICE_H */
