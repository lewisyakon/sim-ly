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
#include <cstdint>
#include<map>
#include <memory>
#include <set>
#include"min-hop-bridge-net-device.h"
#include "FlowTagHeader.h"
#include "flow-io.h"
#include "roce-sim-types.h"

namespace ns3 {
class RoceSimHeader;
class RoceTransportEngine;
struct RoceTxSlice;
class RdmaDispatchTag;
class RdmaPortEngine;
class MinHopBridgeNetDevice;

uint64_t GetLastNetworkActivityTimeFs();
uint64_t GetLastFlowTxDoneTimeFs();
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
  sendflowclient();
  virtual ~sendflowclient();

  void TriggerSendTo_now(PacketSocketAddress dstadd,uint64_t datasize,uint32_t tag,uint32_t src,uint32_t dst,FlowSendMode sendmodel);
  
  void initialization(uint16_t Pronum,uint32_t m_outDev,uint32_t m_innerDev,Ptr<NetDevice> bridgenode);
  void initialization(uint16_t Pronum,uint32_t m_innerDev,Ptr<NetDevice> bridgenode);

  void set_sendfunc(int tag,int src,int dst,fuc_and_par fp); 
  void set_recvfunc(int tag,int src,int dst,fuc_and_par fp); 
  void erase_recvhash(int tag,int src,int dst);
  void erase_flowtag(int tag,int src,int dst);

  void write_flowtag(int tag,int src,int dst,AstraSim::sim_request request);
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
  
private:
  virtual void StartApplication(void);
  virtual void StopApplication(void);

  void HandleRead(Ptr<Socket> socket); // 处理接收
  void SendPacket(ThreePHeader::State state, const Address& dest,uint64_t datasize,
    uint32_t tag,uint32_t src,uint32_t dst,FlowSendMode sendmodel,int mindex=0);
  void realsend(Ptr<Packet> packet,const Address& dest,FlowSendMode sendmodel,int mindex=0);
  void EnqueueRoceWork_(ThreePHeader::State state,
                        const Address& dest,
                        uint64_t datasize,
                        uint32_t tag,
                        uint32_t src,
                        uint32_t dst,
                        FlowSendMode sendmodel,
                        int mindex = 0);
  void FinalizeRoceSliceDispatch_(const RoceTxSlice& slice);
  void ProcessThreePhasePayload_(Ptr<Packet> payload,
                                 const ThreePHeader& TPheader,
                                 const Address& from,
                                 uint32_t formIndex,
                                 bool hasTransportFragmentId = false,
                                 uint32_t transportFragmentId = 0,
                                 bool hasTransportQpKey = false,
                                 RoceQpKey transportQpKey = RoceQpKey{},
                                 uint32_t roceIngressDeviceIndex = UINT32_MAX);
  void InitializeRdmaPortEngines_();
  void HandleRdmaPortPacketSent_(Ptr<Packet> packet);
  void FinalizeRdmaTaggedDispatch_(const RdmaDispatchTag& tag);
  uint32_t ResolveRoceIngressDeviceIndex_(const Ptr<Packet>& packet,
                                          uint32_t fallbackIndex) const;
  Ptr<RdmaPortEngine> GetRdmaPortEngineByDeviceIndex_(uint32_t deviceIndex) const;
  Ptr<RdmaPortEngine> ResolveRoceTxPortEngine_(const Address& dest,
                                               FlowSendMode sendmodel,
                                               int mindex,
                                               uint64_t uidHint = 0) const;
  Address ExtractRoceWireDestination_(const Address& dest) const;
  void MaybeReleaseRoceRxQp_(bool hasTransportQpKey,
                             const RoceQpKey& transportQpKey,
                             uint32_t roceIngressDeviceIndex);


  std::map<RoceFlowKey, ns3::fuc_and_par> m_sendfunctable;  // key: flowid
  std::map<RoceFlowKey,ns3::fuc_and_par>m_recvfunctable;
  std::map<RoceFlowKey,AstraSim::sim_request>m_request;
  std::map<RoceFlowKey,bool> m_recvfuncflag;


  
  std::map<RoceFlowKey, uint64_t> m_recvHash;
  
  // 预期 DATA 包的总数
  std::map<RoceFlowKey, uint64_t> m_dataPacketsExpected;
  // 当前已收到的 DATA 包数量
  std::map<RoceFlowKey, uint64_t> m_dataPacketsReceived;
  // RoCE DATA 只统计第一次成功交付到 3P 的唯一 transport fragment。
  std::map<RoceFlowKey, std::set<uint32_t>> m_deliveredDataFragmentIds;


  // 预期 ACT 包的总数
  std::map<RoceFlowKey, uint64_t> m_ackPacketsExpected;
  //当前已收到的 ACT 包数量
  std::map<RoceFlowKey, uint64_t> m_ackPacketsReceived;
  std::map<RoceFlowKey, uint64_t> m_alldatasize;

  int reqlen,acklen;
  uint32_t localbridgeindex;
  ns3::Ptr<ns3::FlowIo> m_io;
  uint32_t m_outDeviceIndex = 1;
  uint32_t m_innerDeviceIndex = 0;
  Ptr<NetDevice> m_bridgeDevice;

  RoceConfig m_roceCfg; // 当前节点 RoCE 配置（来自 json）
  RoceMode m_roceMode = RoceMode::Legacy;
  std::unique_ptr<RoceTransportEngine> m_roceTransport;
  bool m_roceActiveLogged = false;
};


} // namespace ns3

#endif
