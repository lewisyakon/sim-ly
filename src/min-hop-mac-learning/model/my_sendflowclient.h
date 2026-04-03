#ifndef MY_SEND_FLOW_CLIENT_H
#define MY_SEND_FLOW_CLIENT_H

#include "astra-sim/system/AstraNetworkAPI.hh"
#include "astra-sim/system/Sys.hh"
#include "astra-sim/system/RecvPacketEventHadndlerData.hh"

#include "ns3/application.h"
#include "ns3/address.h"
#include "ns3/ptr.h"
#include "ns3/socket.h"
#include "ns3/log.h"
#include "ns3/packet.h"
#include "ns3/nstime.h"
#include "ns3/event-id.h"

#include "ns3/packet-socket.h"           // PacketSocket
#include "ns3/packet-socket-factory.h"
#include "ns3/packet-socket-address.h"
#include "ns3/mac48-address.h"           // MAC 地址
#include "ns3/hop-count-header.h"
#include "3PHeader.h"
#include<map>
#include <memory>
#include <set>
#include"min-hop-bridge-net-device.h"
#include "FlowTagHeader.h"
#include "flow-io.h"
#include "roce-sim-types.h"
#include <cstdint>

namespace ns3 {
class RoceQpEngine;
class RoceThreePhaseFsm;
class RoceSimHeader;
class RoceTxScheduler;
struct RoceTxSlice;
}


namespace ns3 {
struct fuc_and_par{
  void (*msg_handler)(void *fun_arg);//函数
  void *fun_arg;//参数
};


// ---------- 客户端（请求方） ----------
class sendflowclient : public Application
{
public:
  struct RuntimeTableStats {
    uint64_t sendFuncEntries = 0;
    uint64_t recvFuncEntries = 0;
    uint64_t requestEntries = 0;
    uint64_t recvFuncFlagEntries = 0;
    uint64_t recvHashEntries = 0;
    uint64_t dataExpectedEntries = 0;
    uint64_t dataReceivedEntries = 0;
    uint64_t ackExpectedEntries = 0;
    uint64_t ackReceivedEntries = 0;
    uint64_t allDataSizeEntries = 0;
    uint64_t roceFlowToQpnEntries = 0;
    uint64_t roceOutstandingQps = 0;
    uint64_t roceOutstandingPackets = 0;
    uint64_t roceRetransTimerQps = 0;
    uint64_t roceBufferedQps = 0;
    uint64_t roceBufferedPackets = 0;
    uint64_t rocePendingRetransQps = 0;
    uint64_t roceRecoveryTargetQps = 0;
    uint64_t rocePendingWorkItems = 0;
    uint64_t totalTrackedEntries = 0;
  };

  sendflowclient();
  virtual ~sendflowclient();

  void TriggerSendTo_now(PacketSocketAddress dstadd,uint64_t datasize,uint32_t tag,uint32_t src,uint32_t dst,FlowSendMode sendmodel);
  
  void initialization(uint16_t Pronum,uint32_t m_outDev,uint32_t m_innerDev,Ptr<NetDevice> bridgenode);
  void initialization(uint16_t Pronum,uint32_t m_innerDev,Ptr<NetDevice> bridgenode);

  void set_sendfunc(int tag,int src,int dst,fuc_and_par fp); 
  void set_recvfunc(int tag,int src,int dst,fuc_and_par fp); 
  void erase_recvhash(int tag,int src,int dst);

  void write_flowtag(int tag,int src,int dst,AstraSim::sim_request request);
  void erase_flowtag(int tag,int src,int dst);
  AstraSim::sim_request get_flowtag(int tag,int src,int dst);

  uint64_t get_recvhash(int tag,int src,int dst);
  void set_recvhash(int tag,int src,int dst,uint64_t countnum);
  bool check_recvfunc(int tag,int src,int dst);
  bool check_recvhash(int tag,int src,int dst);

  // RoCE 配置入口；未启用时保持 legacy 行为。
  void SetRoceConfig(const RoceConfig& cfg);
  void SetRoceMode(RoceMode mode);
  RoceMode GetRoceMode() const;
  bool IsRoceEnabled() const;
  RuntimeTableStats GetRuntimeTableStats() const;
  
private:
  virtual void StartApplication(void);
  virtual void StopApplication(void);

  void HandleRead(Ptr<Socket> socket); // 处理接收
  void SendPacket(ThreePHeader::State state, const Address& dest,uint64_t datasize,
    uint32_t tag,uint32_t src,uint32_t dst,FlowSendMode sendmodel,int mindex=0);
  void realsend(Ptr<Packet> packet,const Address& dest,FlowSendMode sendmodel,int mindex=0);
  // 发送侧：根据当前 3P 状态构建 RoCE 头（含 opcode / phase / psn 等）。
  bool BuildRoceTxHeader_(ThreePHeader::State state,
                          uint64_t totalDataSize,
                          uint32_t payloadBytes,
                          uint32_t tag,
                          uint32_t src,
                          uint32_t dst,
                          RoceSimHeader* headerOut);
  // 接收侧：校验并消费 RoCE 头，同时驱动 completion/FSM。
  bool HandleRoceRxHeader_(const RoceSimHeader& roceHeader,
                           ThreePHeader::State state,
                           uint32_t tag,
                           uint32_t src,
                           uint32_t dst,
                           const Address& from,
                           uint32_t rxInDevIndexForLoopback,
                           bool* bufferedForReorder = nullptr,
                           bool* overflowedReorderWindow = nullptr);
  void EnqueueRoceWork_(ThreePHeader::State state,
                        const Address& dest,
                        uint64_t datasize,
                        uint32_t tag,
                        uint32_t src,
                        uint32_t dst,
                        FlowSendMode sendmodel,
                        int mindex = 0);
  void ScheduleRoceDispatch_();
  void DispatchOneRoceSlice_();
  bool BuildPacketForRoceSlice_(const RoceTxSlice& slice,
                                Ptr<Packet>* packetOut,
                                RoceSimHeader* headerOut = nullptr);
  void FinalizeRoceSliceDispatch_(const RoceTxSlice& slice);
  void DispatchPendingRoceRetransmissions_();
  bool BuildRoceAckPacket_(uint32_t tag,
                           uint32_t src,
                           uint32_t dst,
                           uint32_t qpn,
                           Ptr<Packet>* packetOut,
                           RoceSimHeader* headerOut = nullptr);
  void MaybeSendRoceAck_(const Address& dest,
                         uint32_t tag,
                         uint32_t src,
                         uint32_t dst,
                         uint32_t qpn,
                         uint32_t rxInDevIndexForLoopback,
                         RoceOpcode rxOpcode);
  void TrackRoceOutstandingPacket_(const RoceTxSlice& slice,
                                   const Ptr<Packet>& packet,
                                   const RoceSimHeader& header);
  void HandleRoceAck_(const RoceFlowKey& flowKey,
                      const RoceQpKey& qpKey,
                      const RoceSimHeader& roceHeader);
  void HandleRoceRetransTimeout_(RoceQpKey qpKey);
  void MaybeReleaseBufferedRocePackets_(const RoceQpKey& qpKey);
  void ProcessThreePhasePayload_(Ptr<Packet> payload,
                                 const ThreePHeader& TPheader,
                                 const Address& from,
                                 uint32_t formIndex);
  void ArmRoceRetransTimer_(const RoceQpKey& qpKey);
  void CancelRoceRetransTimer_(const RoceQpKey& qpKey);
  uint32_t GetRoceOutstandingCount_(const RoceQpKey& qpKey) const;
  uint32_t GetRoceOldestOutstandingPsn_(const RoceQpKey& qpKey) const;
  uint32_t GetRoceRecoveryTargetPsn_(const RoceQpKey& qpKey) const;
  uint32_t GetRoceTransportWindowPackets_() const;
  std::set<RoceQpKey> CollectBlockedRoceQps_() const;
  uint32_t GetOrAllocRoceQpn_(uint32_t tag, uint32_t src, uint32_t dst);
  void RememberRoceQpn_(uint32_t tag, uint32_t src, uint32_t dst, uint32_t qpn);
  void CleanupFlowRuntimeTables_(const RoceFlowKey& flowKey);
  void CleanupRoceQpRuntimeState_(const RoceFlowKey& flowKey,
                                  const RoceQpKey* qpHint = nullptr);


  std::map<RoceFlowKey, ns3::fuc_and_par> m_sendfunctable;  // key: flowid
  std::map<RoceFlowKey,ns3::fuc_and_par>m_recvfunctable;
  std::map<RoceFlowKey,AstraSim::sim_request>m_request;
  std::map<RoceFlowKey,bool> m_recvfuncflag;


  
  std::map<RoceFlowKey, uint64_t> m_recvHash;
  
  // 预期 DATA 包的总数
  std::map<RoceFlowKey, uint64_t> m_dataPacketsExpected;
  // 当前已收到的 DATA 包数量
  std::map<RoceFlowKey, uint64_t> m_dataPacketsReceived;


  // 预期 ACT 包的总数
  std::map<RoceFlowKey, uint64_t> m_ackPacketsExpected;
  //当前已收到的 ACT 包数量
  std::map<RoceFlowKey, uint64_t> m_ackPacketsReceived;
  std::map<RoceFlowKey, uint64_t> m_alldatasize;

  struct RoceOutstandingPacket {
    Ptr<Packet> packet;
    Address dest;
    FlowSendMode sendMode = FlowSendMode::External;
    int mindex = 0;
    RoceFlowKey flowKey;
    RoceQpKey qpKey;
    uint32_t psn = 0;
    uint32_t retries = 0;
  };

  struct RoceRetransTimerState {
    EventId event;
    uint32_t trackedPsn = 0;
  };

  struct BufferedRoceUpperPacket {
    Ptr<Packet> payload;
    ThreePHeader tpHeader;
    Address from;
    uint32_t formIndex = 0;
  };

  int reqlen,acklen;
  uint32_t localbridgeindex;
  ns3::Ptr<ns3::FlowIo> m_io;

  RoceConfig m_roceCfg; // 当前节点 RoCE 配置（来自 json）
  RoceMode m_roceMode = RoceMode::Legacy;
  std::unique_ptr<RoceQpEngine> m_roceQpEngine;   // QP 状态机 + CQ
  std::unique_ptr<RoceThreePhaseFsm> m_roceFsm;   // REQUEST/DATA/ACT 语义状态机
  std::unique_ptr<RoceTxScheduler> m_roceTxScheduler; // 发送调度器（第一阶段骨架）
  std::map<RoceFlowKey, uint32_t> m_roceFlowToQpn; // flow -> synthetic qpn
  std::map<RoceQpKey, std::map<uint32_t, RoceOutstandingPacket>> m_roceOutstandingPackets;
  std::map<RoceQpKey, RoceRetransTimerState> m_roceRetransTimers;
  std::map<RoceQpKey, std::map<uint32_t, BufferedRoceUpperPacket>> m_roceBufferedUpperPackets;
  std::map<RoceQpKey, RoceOutstandingPacket> m_rocePendingRetransmissions;
  std::map<RoceQpKey, uint32_t> m_roceRecoveryTargetPsns;
  uint32_t m_nextSyntheticQpn = 1;
  uint64_t m_nextRoceWorkId = 1;
  EventId m_roceDispatchEvent;
  bool m_roceActiveLogged = false;
};


} // namespace ns3

#endif
