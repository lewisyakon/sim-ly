#include "my_sendflowclient.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/inet-socket-address.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "rdma-dispatch-tag.h"
#include "rdma-ingress-port-tag.h"
#include "rdma-port-engine.h"
#include "roce-sim-header.h"
#include "roce-transport-engine.h"
#include "ns3/packet-socket-address.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <sstream>
#include <chrono>


namespace ns3 {

NS_LOG_COMPONENT_DEFINE("SendFlowClient");

namespace {

constexpr uint16_t kRoceBthBytes = 12;
constexpr uint32_t kRendezvousControlTagBase = 500000000;
std::atomic<uint64_t> g_lastNetworkActivityTimeFs{0};
std::atomic<uint64_t> g_lastFlowTxDoneTimeFs{0};

void
UpdateMaxTimestamp(std::atomic<uint64_t>* target, uint64_t timeFs) {
  if (target == nullptr) {
    return;
  }
  uint64_t observed = target->load(std::memory_order_relaxed);
  while (observed < timeFs &&
         !target->compare_exchange_weak(observed,
                                        timeFs,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
  }
}

void
RecordLastNetworkActivity() {
  UpdateMaxTimestamp(
      &g_lastNetworkActivityTimeFs,
      static_cast<uint64_t>(Simulator::Now().GetFemtoSeconds()));
}

void
RecordLastFlowTxDone() {
  const uint64_t nowFs =
      static_cast<uint64_t>(Simulator::Now().GetFemtoSeconds());
  UpdateMaxTimestamp(&g_lastNetworkActivityTimeFs, nowFs);
  UpdateMaxTimestamp(&g_lastFlowTxDoneTimeFs, nowFs);
}

std::string
ToLowerCopy(std::string v) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return v;
}

bool
IsRoceBasicLogEnabled(const RoceConfig& cfg) {
  return cfg.logLevel != RoceLogLevel::Off;
}

const char*
ToString(RoceLogLevel level) {
  switch (level) {
    case RoceLogLevel::Off:
      return "off";
    case RoceLogLevel::Basic:
      return "basic";
    case RoceLogLevel::Full:
      return "full";
    default:
      return "unknown";
  }
}

const char*
ToStringAckTimeoutMode(const RoceConfig& cfg) {
  return cfg.autoAckTimeout ? "auto" : "manual";
}

uint16_t
GetVisibleRoceWireBytes(RoceOpcode opcode) {
  return static_cast<uint16_t>(RoceSimHeader::GetVisibleWireBytes(opcode));
}

bool
TryMapRequestFlowToDataFlow(const RoceFlowKey& requestFlowKey, RoceFlowKey* dataFlowKey) {
  if (dataFlowKey == nullptr || requestFlowKey.tag < kRendezvousControlTagBase) {
    return false;
  }
  *dataFlowKey = requestFlowKey;
  dataFlowKey->tag -= kRendezvousControlTagBase;
  return true;
}

RoceOpcode
ToRoceOpcodeForConfig(ThreePHeader::State state, const RoceConfig& cfg) {
  const std::string mode = ToLowerCopy(cfg.modeName);
  const bool preferRead = mode.find("read") != std::string::npos;
  const bool preferSend = mode.find("send") != std::string::npos;

  switch (state) {
    case ThreePHeader::REQUEST:
      return preferRead ? RoceOpcode::RdmaReadReq : RoceOpcode::Send;
    case ThreePHeader::DATA:
      if (preferRead) {
        return RoceOpcode::RdmaReadResp;
      }
      if (preferSend) {
        return RoceOpcode::Send;
      }
      return cfg.dataUseWrite ? RoceOpcode::RdmaWrite : RoceOpcode::Send;
    case ThreePHeader::ACT:
      return RoceOpcode::Send;
    default:
      return RoceOpcode::Send;
  }
}

uint16_t
GetVisibleRoceWireBytes(ThreePHeader::State state, const RoceConfig& cfg) {
  return GetVisibleRoceWireBytes(ToRoceOpcodeForConfig(state, cfg));
}

const char*
GetRoceLaneLabel(ThreePHeader::State state) {
  switch (state) {
    case ThreePHeader::REQUEST:
      return "req_ctrl";
    case ThreePHeader::DATA:
      return "data";
    case ThreePHeader::ACT:
      return "act_ctrl";
    default:
      return "unknown";
  }
}

} // namespace

uint64_t
GetLastNetworkActivityTimeFs() {
  return g_lastNetworkActivityTimeFs.load(std::memory_order_relaxed);
}

uint64_t
GetLastFlowTxDoneTimeFs() {
  return g_lastFlowTxDoneTimeFs.load(std::memory_order_relaxed);
}


// ---------- 客户端实现 ----------

sendflowclient::sendflowclient()
{
  reqlen=acklen=100;
  localbridgeindex=0;
  m_roceTransport = std::make_unique<RoceTransportEngine>();
  m_roceTransport->Configure(m_roceCfg);
}
sendflowclient::~sendflowclient()
{
  if (m_roceTransport != nullptr) {
    m_roceTransport->Reset();
  }
}

void sendflowclient::StartApplication()
{
  if (m_roceTransport != nullptr && GetNode() != nullptr) {
    m_roceTransport->SetNodeId(GetNode()->GetId());
  }
  InitializeRdmaPortEngines_();
}

void sendflowclient::StopApplication()
{
}


void sendflowclient::HandleRead(Ptr<Socket> socket)
{
  Ptr<Packet> packet;
  Address from;

  while ((packet = socket->RecvFrom(from))) {
    RecordLastNetworkActivity();
    Ptr<Packet> p = packet->Copy();
    ThreePHeader TPheader;
    HopCountHeader hopHeader;
    p->RemoveHeader(hopHeader);  // 移除头部以读取状态
    p->RemoveHeader(TPheader);  // 移除头部以读取状态
    
    // 获取flowid，用作map的key
    uint32_t tag = TPheader.GetTag();
    uint32_t src = TPheader.GetSrc();
    uint32_t dst = TPheader.GetDst();
    Address local;
    socket->GetSockName(local);
    PacketSocketAddress laddr = PacketSocketAddress::ConvertFrom(local);
    uint32_t formIndex = laddr.GetSingleDevice(); //拿到来源的netdevice编号
    const uint32_t roceIngressDeviceIndex =
        ResolveRoceIngressDeviceIndex_(packet, formIndex);

    RoceSimHeader roceHeader;
    bool hasRoceHeader = false;
    if (IsRoceEnabled()) {
      // RoCE 头最小可见长度是 BTH；当前数据路径上的包在 RoCE 开启时都应携带该头。
      Ptr<Packet> probe = p->Copy();
      if (probe->GetSize() >= kRoceBthBytes) {
        probe->RemoveHeader(roceHeader);
        if (roceHeader.GetVersion() == 1) {
          p->RemoveHeader(roceHeader);
          hasRoceHeader = true;
        } else {
          NS_LOG_WARN("[ROCE][RX] missing/invalid RoCE header, fallback legacy parse"
                      << " tag=" << tag << " src=" << src << " dst=" << dst);
        }
      }
    }

    if (hasRoceHeader) {
      Ptr<RdmaPortEngine> ingressPortEngine =
          GetRdmaPortEngineByDeviceIndex_(roceIngressDeviceIndex);
      if (ingressPortEngine == nullptr) {
        NS_FATAL_ERROR("[ROCE][RX] ingress port engine missing");
      }
      RdmaPortEngine::RxResult rxResult =
          ingressPortEngine->HandleIncomingPacket(roceHeader, TPheader, p, from, formIndex);
      if (!rxResult.accepted || rxResult.readyPackets.empty()) {
        continue;
      }

      for (const auto& readyPacket : rxResult.readyPackets) {
        ProcessThreePhasePayload_(
            readyPacket.payload != nullptr ? readyPacket.payload->Copy() : Create<Packet>(0),
            readyPacket.tpHeader,
            readyPacket.from,
            readyPacket.formIndex,
            readyPacket.hasTransportFragmentId,
            readyPacket.transportFragmentId,
            true,
            readyPacket.qpKey,
            roceIngressDeviceIndex);
      }
      continue;
    }

    ProcessThreePhasePayload_(
        p, TPheader, from, formIndex, false, 0, false, RoceQpKey{}, UINT32_MAX);
  }
}

void
sendflowclient::ProcessThreePhasePayload_(Ptr<Packet> p,
                                          const ThreePHeader& TPheader,
                                          const Address& from,
                                          uint32_t formIndex,
                                          bool hasTransportFragmentId,
                                          uint32_t transportFragmentId,
                                          bool hasTransportQpKey,
                                          RoceQpKey transportQpKey,
                                          uint32_t roceIngressDeviceIndex)
{
  const uint32_t tag = TPheader.GetTag();
  const uint32_t src = TPheader.GetSrc();
  const uint32_t dst = TPheader.GetDst();
  const RoceFlowKey flowKey{tag, src, dst};
  FlowTagHeader FTHeader;

  switch (TPheader.GetState())
  {
  case ThreePHeader::REQUEST:
  {
    const uint64_t requestRxTimeFs =
        static_cast<uint64_t>(Simulator::Now().GetFemtoSeconds());
    RoceFlowKey dataFlowKey;
    const bool mappedToDataFlow = TryMapRequestFlowToDataFlow(flowKey, &dataFlowKey);
    if (mappedToDataFlow) {
      m_lastRequestArrivalTimeFs[dataFlowKey] = requestRxTimeFs;
    }
    if(check_recvfunc(tag,src,dst) == false){
      m_recvfuncflag[flowKey] = 1;
      MaybeReleaseRoceRxQp_(hasTransportQpKey, transportQpKey, roceIngressDeviceIndex);
      return ;
    }
    if (mappedToDataFlow) {
      m_lastRequestCallbackBeginTimeFs[dataFlowKey] = requestRxTimeFs;
    }
    fuc_and_par fp = m_recvfunctable[flowKey];
    fp.msg_handler(fp.fun_arg);
    const uint64_t requestCallbackEndFs =
        static_cast<uint64_t>(Simulator::Now().GetFemtoSeconds());
    if (mappedToDataFlow) {
      m_lastRequestCallbackEndTimeFs[dataFlowKey] = requestCallbackEndFs;
    }
    m_recvfunctable.erase(flowKey);
    m_recvfuncflag.erase(flowKey);
    MaybeReleaseRoceRxQp_(hasTransportQpKey, transportQpKey, roceIngressDeviceIndex);
  }
  break;

  case ThreePHeader::DATA:
  {
    p->RemoveHeader(FTHeader);
    AstraSim::sim_request now_request;
    FTHeader.ToFlowTag(now_request.flowTag);
    uint32_t PacketNum=FTHeader.get_packetnum();
    if (m_request.find(flowKey)==m_request.end()) m_request[flowKey]=now_request;
    if (m_dataPacketsExpected.find(flowKey)==m_dataPacketsExpected.end()){
      m_dataPacketsExpected[flowKey]=PacketNum;
      m_alldatasize[flowKey]=TPheader.GetDataSize();
    }

    if (m_dataPacketsExpected.count(flowKey)!=0) {
        if (hasTransportFragmentId) {
            const bool inserted =
                m_deliveredDataFragmentIds[flowKey].insert(transportFragmentId).second;
            if (!inserted) {
                break;
            }
        }
        m_dataPacketsReceived[flowKey]++;
        if (m_dataPacketsReceived[flowKey] == m_dataPacketsExpected[flowKey]) {
            Address ackDest = from;
            uint64_t acksize = acklen;
            uint32_t ackTag  = tag;
            uint32_t ackSrc  = src;
            uint32_t ackDst  = dst;
            int      ackIndex     = formIndex;
            if (IsRoceEnabled()) {
              EnqueueRoceWork_(ThreePHeader::ACT,
                               ackDest,
                               acksize,
                               ackTag,
                               ackSrc,
                               ackDst,
                               FlowSendMode::Loopback,
                               ackIndex);
            } else {
              ns3::Simulator::ScheduleWithContext((this->GetNode()->GetId()), Seconds(0), [this,ackDest,acksize,ackTag,ackSrc,ackDst,ackIndex](){
                  SendPacket(ThreePHeader::ACT,ackDest,acksize,ackTag,ackSrc,ackDst,FlowSendMode::Loopback,ackIndex); });
            }

            m_recvHash[flowKey]=m_alldatasize[flowKey];
            NS_LOG_INFO("[FLOW][RX_DONE] {tag,src,dst}= "
                        << tag << " " << src << " " << dst
                        << " dataPackets=" << m_dataPacketsReceived[flowKey]
                        << "/" << m_dataPacketsExpected[flowKey]
                        << " dataBytes=" << m_alldatasize[flowKey]
                        << " TIME=" << Simulator::Now().GetFemtoSeconds());
            bool deliveredToRegisteredReceiver = false;
            if (check_recvfunc(tag,src,dst)){
              fuc_and_par fp = m_recvfunctable[flowKey];

              AstraSim::RecvPacketEventHadndlerData* ehd = (AstraSim::RecvPacketEventHadndlerData*) fp.fun_arg;
              ehd->flowTag=m_request[flowKey].flowTag;

              fp.msg_handler(fp.fun_arg);
              m_recvfunctable.erase(flowKey);
              deliveredToRegisteredReceiver = true;
            }

            // DATA 聚合 scratch 到此已经结束；若回调是“晚注册”的，
            // 则仍需保留 flowTag/recvHash 供 sim_recv 路径消费后再删。
            if (deliveredToRegisteredReceiver) {
              m_recvHash.erase(flowKey);
              m_request.erase(flowKey);
            }
            m_alldatasize.erase(flowKey);
            m_recvfuncflag.erase(flowKey);
            m_dataPacketsExpected.erase(flowKey);
            m_dataPacketsReceived.erase(flowKey);
            m_deliveredDataFragmentIds.erase(flowKey);
            MaybeReleaseRoceRxQp_(hasTransportQpKey, transportQpKey, roceIngressDeviceIndex);
        }
    } else {
        NS_LOG_WARN("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst << ": Received DATA packet without expected count setup.");
    }

  }
  break;
  case ThreePHeader::ACT:
  {
    if (m_roceTransport != nullptr) {
        m_roceTransport->OnActReceived(flowKey);
    }
    if (m_ackPacketsExpected.find(flowKey) != m_ackPacketsExpected.end()) {
        m_ackPacketsReceived[flowKey]++;

        if (m_ackPacketsReceived[flowKey] == m_ackPacketsExpected[flowKey]) {
            if (m_sendfunctable.find(flowKey) != m_sendfunctable.end()) {
                fuc_and_par fp = m_sendfunctable[flowKey];
                RecordLastFlowTxDone();
                NS_LOG_INFO("[FLOW][TX_DONE] {tag,src,dst}= "
                            << tag << " " << src << " " << dst
                            << " actPackets=" << m_ackPacketsReceived[flowKey]
                            << "/" << m_ackPacketsExpected[flowKey]
                            << " TIME=" << Simulator::Now().GetFemtoSeconds());
                fp.msg_handler(fp.fun_arg);
                m_sendfunctable.erase(flowKey);
            } else {
                NS_LOG_WARN("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst <<": (Sender) Callback not found for flowid.");
            }

            // sender 侧 write_flowtag() 写入的 flowTag scratch 到此也已经没用了。
            m_request.erase(flowKey);
            m_ackPacketsExpected.erase(flowKey);
            m_ackPacketsReceived.erase(flowKey);
            m_lastRequestArrivalTimeFs.erase(flowKey);
            m_lastRequestCallbackBeginTimeFs.erase(flowKey);
            m_lastRequestCallbackEndTimeFs.erase(flowKey);
        }
    } else {
        NS_LOG_WARN("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst << ": Received ACT packet without expected count setup.");
    }
    if (m_roceTransport != nullptr) {
        m_roceTransport->ReleaseFlow(flowKey);
    }
    MaybeReleaseRoceRxQp_(hasTransportQpKey, transportQpKey, roceIngressDeviceIndex);
  }
  break;

  default:
    NS_LOG_WARN("Unknown state received");
    break;
  }
}

void
sendflowclient::TriggerSendTo_now(PacketSocketAddress dstadd,uint64_t datasize,uint32_t tag,uint32_t src,uint32_t dst,FlowSendMode sendmodel)
{
  const uint64_t issueTimeFs =
      static_cast<uint64_t>(Simulator::Now().GetFemtoSeconds());
  NS_LOG_INFO("TriggerSendTo_now declared now time = "
              << issueTimeFs
              << " src=" << src
              << " dst=" << dst
              << " tag=" << tag
              << " size=" << datasize
              << " sendMode=" << static_cast<uint32_t>(sendmodel));
  const ThreePHeader::State state =
      (datasize == 1023) ? ThreePHeader::REQUEST : ThreePHeader::DATA;
  if (IsRoceEnabled()) {
    EnqueueRoceWork_(state, dstadd, datasize, tag, src, dst, sendmodel);
  } else if (state == ThreePHeader::REQUEST) {
    ns3::Simulator::ScheduleWithContext((this->GetNode()->GetId()), Seconds(0), [this,dstadd,datasize,tag,src,dst,sendmodel](){
      SendPacket(ThreePHeader::REQUEST, dstadd, datasize, tag,src,dst,sendmodel);});
  } else {
    ns3::Simulator::ScheduleWithContext((this->GetNode()->GetId()), Seconds(0), [this,dstadd,datasize,tag,src,dst,sendmodel](){
      SendPacket(ThreePHeader::DATA, dstadd, datasize, tag,src,dst,sendmodel);});
  }
}
void
sendflowclient::realsend(Ptr<Packet> packet,const Address& dest,FlowSendMode sendmodel,int mindex)
{
    RecordLastNetworkActivity();
    m_io->Send(dest,packet,sendmodel,mindex);
}

void
sendflowclient::EnqueueRoceWork_(ThreePHeader::State state,
                                 const Address& dest,
                                 uint64_t datasize,
                                 uint32_t tag,
                                 uint32_t src,
                                 uint32_t dst,
                                 FlowSendMode sendmodel,
                                 int mindex) {
  if (!IsRoceEnabled()) {
    SendPacket(state, dest, datasize, tag, src, dst, sendmodel, mindex);
    return;
  }
  if (m_roceTransport == nullptr) {
    NS_FATAL_ERROR("[ROCE][SCHED] missing transport engine");
  }
  if (!m_roceActiveLogged && IsRoceBasicLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][ACTIVE] node=" << GetNode()->GetId()
                << " mode=" << m_roceCfg.modeName
                << " dataUseWrite=" << m_roceCfg.dataUseWrite
                << " sqDepth=" << m_roceCfg.defaultSqDepth
                << " rqDepth=" << m_roceCfg.defaultRqDepth
                << " transportWindowPackets=" << m_roceCfg.transportWindowPackets
                << " reorderWindowPackets=" << m_roceCfg.reorderWindowPackets
                << " etherType=0x" << std::hex << m_roceCfg.etherType << std::dec
                << " logLevel=" << ToString(m_roceCfg.logLevel)
                << " ackTimeoutMode=" << ToStringAckTimeoutMode(m_roceCfg)
                << " ackTimeoutBaseNs=" << m_roceCfg.ackTimeoutBaseNs
                << " ackTimeoutFactor=" << m_roceCfg.ackTimeoutFactor
                << " ackTimeoutNs=" << m_roceCfg.ackTimeoutNs);
    m_roceActiveLogged = true;
  }

  Ptr<Node> node = GetNode();
  NS_ASSERT(node != nullptr);
  Ptr<NetDevice> device = node->GetDevice(0);
  NS_ASSERT(device != nullptr);
  const uint32_t mtu = device->GetMtu();
  const RoceFlowKey flowKey{tag, src, dst};
  const Address wireDest = ExtractRoceWireDestination_(dest);

  uint64_t remainingBytes = 1;
  uint64_t totalPackets = 1;
  RoceTxClass txClass = RoceTxClass::Control;
  uint32_t controlPacketBytes =
      (state == ThreePHeader::REQUEST) ? static_cast<uint32_t>(reqlen)
                                       : static_cast<uint32_t>(acklen);
  FlowTagHeader flowTagHeader;
  bool hasFlowTagHeader = false;
  if (state == ThreePHeader::DATA) {
    const uint32_t visibleRoceBytes = GetVisibleRoceWireBytes(state, m_roceCfg);
    const uint32_t payloadBudget =
        mtu > visibleRoceBytes ? (mtu - visibleRoceBytes) : 1;
    remainingBytes = datasize;
    totalPackets = std::max<uint64_t>(
        1, (datasize + static_cast<uint64_t>(payloadBudget) - 1) / payloadBudget);
    txClass = RoceTxClass::Data;
    m_ackPacketsExpected[flowKey] = 1;
    m_ackPacketsReceived[flowKey] = 0;
    flowTagHeader.FromFlowTag(get_flowtag(tag, src, dst).flowTag);
    flowTagHeader.set_packetnum(totalPackets);
    hasFlowTagHeader = true;
  }

  RoceTxWorkItem item;
  item.workId = 0;
  item.state = state;
  item.flowKey = flowKey;
  item.qpKey = RoceQpKey{src, dst, 0};
  item.dest = wireDest;
  item.sendMode = sendmodel;
  item.mindex = mindex;
  item.totalBytes = datasize;
  item.remainingBytes = remainingBytes;
  item.packetIndex = 0;
  item.totalPackets = totalPackets;
  item.controlPacketBytes = controlPacketBytes;
  item.hasFlowTagHeader = hasFlowTagHeader;
  item.flowTagHeader = flowTagHeader;
  item.txClass = txClass;
  if (!m_roceTransport->PrepareTxWork(&item)) {
    NS_FATAL_ERROR("[ROCE][SQ] failed to prepare work"
                   << " tag=" << tag
                   << " src=" << src
                   << " dst=" << dst
                   << " state=" << static_cast<uint32_t>(state));
  }

  Ptr<RdmaPortEngine> portEngine =
      ResolveRoceTxPortEngine_(dest, sendmodel, mindex, item.workId);
  if (portEngine == nullptr) {
    NS_FATAL_ERROR("[ROCE][SQ] failed to resolve tx port"
                   << " tag=" << tag
                   << " src=" << src
                   << " dst=" << dst
                   << " sendMode=" << static_cast<uint32_t>(sendmodel)
                   << " mindex=" << mindex);
  }
  if (!portEngine->PostSend(item)) {
    NS_FATAL_ERROR("[ROCE][SQ] failed to post work"
                   << " tag=" << tag
                   << " src=" << src
                   << " dst=" << dst
                   << " qpn=" << item.qpKey.qpn
                   << " workId=" << item.workId);
  }
}

void
sendflowclient::FinalizeRoceSliceDispatch_(const RoceTxSlice& slice) {
  if (!slice.isLastSlice) {
    return;
  }

  if (slice.state == ThreePHeader::REQUEST) {
    auto it = m_sendfunctable.find(slice.flowKey);
    if (it == m_sendfunctable.end()) {
      NS_LOG_WARN("[ROCE][SCHED] request callback missing"
                  << " tag=" << slice.flowKey.tag
                  << " src=" << slice.flowKey.src
                  << " dst=" << slice.flowKey.dst);
    } else {
      fuc_and_par fp = it->second;
      NS_LOG_INFO("[FLOW][REQ_DONE] {tag,src,dst}= "
                  << slice.flowKey.tag << " "
                  << slice.flowKey.src << " "
                  << slice.flowKey.dst
                  << " workId=" << slice.workId
                  << " TIME=" << Simulator::Now().GetFemtoSeconds());
      fp.msg_handler(fp.fun_arg);
      m_sendfunctable.erase(it);
    }
  }

  if (m_roceTransport != nullptr &&
      (slice.state == ThreePHeader::REQUEST || slice.state == ThreePHeader::ACT)) {
    m_roceTransport->ReleaseLane(slice.flowKey, slice.state);
  }
}


void
sendflowclient::initialization(uint16_t Pronum,uint32_t m_outDev,uint32_t m_innerDev,Ptr<NetDevice> bridgenode)
{
    localbridgeindex=m_innerDev;
    m_outDeviceIndex = m_outDev;
    m_innerDeviceIndex = m_innerDev;
    m_bridgeDevice = bridgenode;
    if (!m_io) m_io = ns3::CreateObject<ns3::FlowIo>();
    m_io->SetNode(GetNode());
    m_io->SetProtocol(Pronum);
    m_io->SetOutDeviceIndex(m_outDev);
    m_io->SetInnerDeviceIndex(m_innerDev);   // 一般固定=0
    m_io->SetLocalBridge(bridgenode);            // 保证它就是 device0
    m_io->SetRxBindMode(RxBindMode::BridgeAndExternal);

    m_io->Start(MakeCallback(&sendflowclient::HandleRead, this));
    if (m_roceTransport != nullptr && GetNode() != nullptr) {
      m_roceTransport->SetNodeId(GetNode()->GetId());
    }
    InitializeRdmaPortEngines_();
}
void
sendflowclient::initialization(uint16_t Pronum,uint32_t m_innerDev,Ptr<NetDevice> bridgenode)
{
    localbridgeindex=m_innerDev;
    m_innerDeviceIndex = m_innerDev;
    m_bridgeDevice = bridgenode;
    if (!m_io) m_io = ns3::CreateObject<ns3::FlowIo>();
    m_io->SetNode(GetNode());
    m_io->SetProtocol(Pronum);
    m_io->SetInnerDeviceIndex(m_innerDev);   // 一般固定=0
    m_io->SetLocalBridge(bridgenode);            // 保证它就是 device0
    m_io->SetRxBindMode(RxBindMode::BridgeOnly);

    m_io->Start(MakeCallback(&sendflowclient::HandleRead, this));
    if (m_roceTransport != nullptr && GetNode() != nullptr) {
      m_roceTransport->SetNodeId(GetNode()->GetId());
    }
    InitializeRdmaPortEngines_();
}

void
sendflowclient::SendPacket(ThreePHeader::State state, const Address& dest, uint64_t datasize,
  uint32_t tag,uint32_t src,uint32_t dst,FlowSendMode sendmodel,int mindex)
{
  // NS_LOG_INFO("[" << this->GetNode()->GetId() << "]" << "SendPacket running now time = "  << Simulator::Now().GetFemtoSeconds());
  //这里需要先判断目标点和本机是否在一个机内

  Ptr<Packet> packet;
  HopCountHeader hopHeader;
  hopHeader.SetHopCount(0); // 初始跳数设为0
  hopHeader.SetIsFlood(false); 
  ThreePHeader TPheader;
  TPheader.SetState(state);
  TPheader.SetTag(tag);
  TPheader.SetSrc(src);
  TPheader.SetDst(dst);
  FlowTagHeader FTHeader;
  
  Ptr<Node> node = GetNode();
  Ptr<NetDevice> device = node->GetDevice(0);
  uint16_t m_mtu=device->GetMtu();
  // 开启 RoCE 时，DATA 分片预算需要扣掉可见 RoCE 头；隐藏的仿真控制头仍算在业务区里。
  const uint16_t roceWireBytes = 0;
  const uint16_t payloadBudget =
      IsRoceEnabled() && m_mtu > roceWireBytes
          ? static_cast<uint16_t>(m_mtu - roceWireBytes)
          : m_mtu;
  auto SafePayloadBytes = [](int64_t rawBytes) -> uint16_t {
    return rawBytes > 0 ? static_cast<uint16_t>(rawBytes) : 0;
  };
  const RoceFlowKey flowKey{tag, src, dst};
 
  if (state == ThreePHeader::REQUEST) {
      uint16_t real_packet_size = SafePayloadBytes(
          static_cast<int64_t>(reqlen) - TPheader.GetSerializedSize() -
          hopHeader.GetSerializedSize() - roceWireBytes);
      packet = Create<Packet>(real_packet_size);
      TPheader.SetDataSize(datasize);
      packet->AddHeader(TPheader);
      packet->AddHeader(hopHeader);
      realsend(packet,dest,sendmodel);

      fuc_and_par fp = m_sendfunctable[flowKey];
      NS_LOG_INFO("[FLOW][REQ_DONE] {tag,src,dst}= "
                  << tag << " " << src << " " << dst
                  << " TIME=" << Simulator::Now().GetFemtoSeconds());
      fp.msg_handler(fp.fun_arg);
      m_sendfunctable.erase(flowKey);

      // m_alldatasize[{tag,{src,dst}}]=datasize;
  }
  
  if (state==ThreePHeader::DATA){
    uint64_t numPackets;
    if (datasize<payloadBudget) numPackets=1;
    else {
      if (datasize % payloadBudget==0) numPackets=datasize / payloadBudget;
      else numPackets = (datasize / payloadBudget) + 1;
    }
    // m_ackPacketsExpected[{tag,{src,dst}}] = numPackets;
    // 发送前记录act
    m_ackPacketsExpected[flowKey] = 1;
    m_ackPacketsReceived[flowKey] = 0; // 初始化 ACT 计数器
    FTHeader.FromFlowTag(get_flowtag(tag,src,dst).flowTag);
    FTHeader.set_packetnum(numPackets);
    TPheader.SetDataSize(datasize);

    uint64_t len = datasize;
    for (uint64_t i = 0; i < numPackets; ++i) {
        uint16_t real_packet_size = 0;
        if (len >= payloadBudget) {
          real_packet_size = SafePayloadBytes(
              static_cast<int64_t>(payloadBudget) - TPheader.GetSerializedSize() -
              hopHeader.GetSerializedSize() - FTHeader.GetSerializedSize());
          packet = Create<Packet>(real_packet_size);
          
        } 
        else {
          real_packet_size = SafePayloadBytes(
              static_cast<int64_t>(len) - TPheader.GetSerializedSize() -
              hopHeader.GetSerializedSize() - FTHeader.GetSerializedSize());
          packet = Create<Packet>(real_packet_size);
        }
        packet->AddHeader(FTHeader);
        packet->AddHeader(TPheader);
        packet->AddHeader(hopHeader);
        
        realsend(packet,dest,sendmodel,mindex);
      
        len = (len > payloadBudget) ? (len - payloadBudget) : 0;
    }
  }
  if (state==ThreePHeader::ACT){
      uint16_t real_packet_size = SafePayloadBytes(
          static_cast<int64_t>(acklen) - TPheader.GetSerializedSize() -
          hopHeader.GetSerializedSize() - roceWireBytes);
      packet = Create<Packet>(real_packet_size);
      TPheader.SetDataSize(acklen);
      packet->AddHeader(TPheader);
      packet->AddHeader(hopHeader);
      realsend(packet,dest,sendmodel,mindex);
  }
    
  
  
  // NS_LOG_INFO("sendflowclient: Sent " << (state == ThreePHeader::REQUEST ? "REQUEST" : 
  //               state == ThreePHeader::ACT ? "ACT" : "DATA")
  //              << " to server on device " << dest
  //              << ", size=" << packet->GetSize());
}
void
sendflowclient::set_sendfunc(int tag,int src,int dst,fuc_and_par fp){
    m_sendfunctable[RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)}]=fp; 
}

void
sendflowclient::set_recvfunc(int tag,int src,int dst,fuc_and_par fp){
    const RoceFlowKey flowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)};
    auto recvFlagIt = m_recvfuncflag.find(flowKey);
    if(recvFlagIt != m_recvfuncflag.end() && recvFlagIt->second){
      m_recvfuncflag.erase(recvFlagIt);
      const uint64_t callbackBeginFs =
          static_cast<uint64_t>(Simulator::Now().GetFemtoSeconds());
      RoceFlowKey dataFlowKey;
      const bool mappedToDataFlow = TryMapRequestFlowToDataFlow(flowKey, &dataFlowKey);
      if (mappedToDataFlow) {
        m_lastRequestCallbackBeginTimeFs[dataFlowKey] = callbackBeginFs;
      }

      // NS_LOG_INFO("[RZV] CALLBACK fired tag="<<tag<<" src="<<src<<" dst="<<dst);
      
      fp.msg_handler(fp.fun_arg);
      const uint64_t callbackEndFs =
          static_cast<uint64_t>(Simulator::Now().GetFemtoSeconds());
      if (mappedToDataFlow) {
        m_lastRequestCallbackEndTimeFs[dataFlowKey] = callbackEndFs;
      }
      return ;
    }
    m_recvfunctable[flowKey]=fp; 
}

bool
sendflowclient::check_recvfunc(int tag,int src,int dst){
    if (m_recvfunctable.find(RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)})!=m_recvfunctable.end()) return true;
    else return false;
}
void
sendflowclient::set_recvhash(int tag,int src,int dst,uint64_t countnum){
    m_recvHash[RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)}]=countnum;
}
void
sendflowclient::erase_recvhash(int tag,int src,int dst){
    m_recvHash.erase(RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)});
}
void
sendflowclient::erase_flowtag(int tag,int src,int dst){
    m_request.erase(RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)});
}
bool
sendflowclient::check_recvhash(int tag,int src,int dst){
    if (m_recvHash.find(RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)})!=m_recvHash.end()) return true;
    else return false;
}
uint64_t
sendflowclient::get_recvhash(int tag,int src,int dst){
    return m_recvHash[RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)}];
}
void
sendflowclient::write_flowtag(int tag,int src,int dst,AstraSim::sim_request request){
    m_request[RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)}]=request;
}
AstraSim::sim_request
sendflowclient::get_flowtag(int tag,int src,int dst){
  return m_request[RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)}];
}

void
sendflowclient::SetRoceConfig(const RoceConfig& cfg) {
  m_roceCfg = cfg;
  // 统一小写后再匹配 mode，避免 JSON 大小写差异带来的行为偏差。
  m_roceCfg.modeName = ToLowerCopy(m_roceCfg.modeName);
  if (m_roceCfg.enable) {
    m_roceMode = RoceMode::RcThreePhase;
  } else {
    m_roceMode = RoceMode::Legacy;
  }
  if (m_roceTransport != nullptr) {
    m_roceTransport->Configure(m_roceCfg);
    m_roceTransport->Reset();
    if (GetNode() != nullptr) {
      m_roceTransport->SetNodeId(GetNode()->GetId());
    }
  }
  InitializeRdmaPortEngines_();
  m_roceActiveLogged = false;
  if (IsRoceBasicLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][CFG] enable=" << m_roceCfg.enable
                << " mode=" << m_roceCfg.modeName
                << " dataUseWrite=" << m_roceCfg.dataUseWrite
                << " etherType=0x" << std::hex << m_roceCfg.etherType << std::dec
                << " logLevel=" << ToString(m_roceCfg.logLevel)
                << " transportAck=" << m_roceCfg.enableTransportAck
                << " transportWindowPackets=" << m_roceCfg.transportWindowPackets
                << " reorderWindowPackets=" << m_roceCfg.reorderWindowPackets
                << " ackTimeoutMode=" << ToStringAckTimeoutMode(m_roceCfg)
                << " ackTimeoutBaseNs=" << m_roceCfg.ackTimeoutBaseNs
                << " ackTimeoutFactor=" << m_roceCfg.ackTimeoutFactor
                << " ackTimeoutNs=" << m_roceCfg.ackTimeoutNs
                << " maxRetrans=" << m_roceCfg.maxRetransmissions
                << " reqOp=" << static_cast<uint32_t>(ToRoceOpcodeForConfig(ThreePHeader::REQUEST, m_roceCfg))
                << " dataOp=" << static_cast<uint32_t>(ToRoceOpcodeForConfig(ThreePHeader::DATA, m_roceCfg))
                << " actOp=" << static_cast<uint32_t>(ToRoceOpcodeForConfig(ThreePHeader::ACT, m_roceCfg)));
  }
}

void
sendflowclient::SetRoceMode(RoceMode mode) {
  m_roceMode = mode;
}

RoceMode
sendflowclient::GetRoceMode() const {
  return m_roceMode;
}

bool
sendflowclient::IsRoceEnabled() const {
  return m_roceCfg.enable && m_roceMode != RoceMode::Legacy;
}

void
sendflowclient::InitializeRdmaPortEngines_() {
  Ptr<Node> node = GetNode();
  if (node == nullptr) {
    return;
  }

  for (uint32_t i = 0; i < node->GetNDevices(); ++i) {
    Ptr<PointToPointNetDevice> p2p = DynamicCast<PointToPointNetDevice>(node->GetDevice(i));
    if (!p2p) {
      continue;
    }

    Ptr<RdmaPortEngine> engine = p2p->GetObject<RdmaPortEngine>();
    if (engine == nullptr) {
      engine = CreateObject<RdmaPortEngine>();
      p2p->AggregateObject(engine);
    }
    engine->AttachToDevice(p2p);
    engine->Configure(m_roceCfg);
    if (GetNode() != nullptr) {
      engine->SetNodeId(GetNode()->GetId());
    }
    engine->SetTxNotifyCallback(MakeCallback(&sendflowclient::HandleRdmaPortPacketSent_, this));
  }
}

void
sendflowclient::HandleRdmaPortPacketSent_(Ptr<Packet> packet) {
  if (packet == nullptr) {
    return;
  }

  RdmaDispatchTag tag;
  if (!packet->PeekPacketTag(tag)) {
    return;
  }
  if (tag.IsTransportAck()) {
    return;
  }
  if (!tag.IsRetransmission()) {
    FinalizeRdmaTaggedDispatch_(tag);
  }
}

void
sendflowclient::FinalizeRdmaTaggedDispatch_(const RdmaDispatchTag& tag) {
  if (!tag.IsLastSlice()) {
    return;
  }

  RoceTxSlice slice;
  slice.workId = tag.GetWorkId();
  slice.state = tag.GetState();
  slice.flowKey = tag.GetFlowKey();
  slice.qpKey = RoceQpKey{slice.flowKey.src, slice.flowKey.dst, tag.GetQpn()};
  slice.totalBytes = tag.GetTotalBytes();
  slice.sliceBytes = tag.GetSliceBytes();
  slice.packetIndex = tag.GetPacketIndex();
  slice.totalPackets = tag.GetTotalPackets();
  slice.isLastSlice = true;
  slice.txClass = tag.GetTxClass();
  FinalizeRoceSliceDispatch_(slice);
}

uint32_t
sendflowclient::ResolveRoceIngressDeviceIndex_(const Ptr<Packet>& packet,
                                               uint32_t fallbackIndex) const {
  if (packet != nullptr) {
    RdmaIngressPortTag ingressTag;
    if (packet->PeekPacketTag(ingressTag)) {
      return ingressTag.GetDeviceIndex();
    }
  }
  return fallbackIndex;
}

Ptr<RdmaPortEngine>
sendflowclient::GetRdmaPortEngineByDeviceIndex_(uint32_t deviceIndex) const {
  Ptr<Node> node = GetNode();
  if (node == nullptr || deviceIndex >= node->GetNDevices()) {
    return nullptr;
  }
  Ptr<PointToPointNetDevice> p2p = DynamicCast<PointToPointNetDevice>(node->GetDevice(deviceIndex));
  if (!p2p) {
    return nullptr;
  }
  return p2p->GetObject<RdmaPortEngine>();
}

Address
sendflowclient::ExtractRoceWireDestination_(const Address& dest) const {
  if (PacketSocketAddress::IsMatchingType(dest)) {
    return PacketSocketAddress::ConvertFrom(dest).GetPhysicalAddress();
  }
  return dest;
}

void
sendflowclient::MaybeReleaseRoceRxQp_(bool hasTransportQpKey,
                                      const RoceQpKey& transportQpKey,
                                      uint32_t roceIngressDeviceIndex) {
  if (!hasTransportQpKey || !IsRoceEnabled()) {
    return;
  }

  Ptr<RdmaPortEngine> ingressPortEngine =
      GetRdmaPortEngineByDeviceIndex_(roceIngressDeviceIndex);
  if (ingressPortEngine == nullptr) {
    return;
  }
  ingressPortEngine->ReleaseQpState(transportQpKey);
}

Ptr<RdmaPortEngine>
sendflowclient::ResolveRoceTxPortEngine_(const Address& dest,
                                         FlowSendMode sendmodel,
                                         int mindex,
                                         uint64_t uidHint) const {
  const Address wireDest = ExtractRoceWireDestination_(dest);

  if (sendmodel == FlowSendMode::External) {
    return GetRdmaPortEngineByDeviceIndex_(m_outDeviceIndex);
  }
  if (sendmodel == FlowSendMode::Loopback && static_cast<uint32_t>(mindex) == m_outDeviceIndex) {
    return GetRdmaPortEngineByDeviceIndex_(m_outDeviceIndex);
  }

  Ptr<MinHopBridgeNetDevice> bridge = DynamicCast<MinHopBridgeNetDevice>(m_bridgeDevice);
  if (bridge == nullptr) {
    return nullptr;
  }

  Ptr<NetDevice> outPort =
      bridge->ResolveLocalInjectionEgressPort(bridge->GetAddress(), wireDest, uidHint);
  if (outPort == nullptr) {
    return nullptr;
  }
  Ptr<PointToPointNetDevice> p2p = DynamicCast<PointToPointNetDevice>(outPort);
  if (p2p == nullptr) {
    return nullptr;
  }
  return p2p->GetObject<RdmaPortEngine>();
}


}
