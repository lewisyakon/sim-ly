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
#include <algorithm>
#include <atomic>
#include <cctype>
#include <sstream>
#include <chrono>


namespace ns3 {

NS_LOG_COMPONENT_DEFINE("SendFlowClient");

namespace {

constexpr uint16_t kRoceBthBytes = 12;
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

bool
IsRoceFullLogEnabled(const RoceConfig& cfg) {
  return cfg.logLevel == RoceLogLevel::Full;
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
  m_roceTransport->SetDispatchWakeup([this]() { ScheduleRoceDispatch_(); });
  m_roceTransport->Configure(m_roceCfg);
}
sendflowclient::~sendflowclient()
{
  if (m_roceDispatchEvent.IsPending()) {
    m_roceDispatchEvent.Cancel();
  }
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
      if (m_roceTransport == nullptr) {
        NS_FATAL_ERROR("[ROCE][RX] transport engine missing");
      }
      if (roceHeader.GetOpcode() == RoceOpcode::Ack) {
        Ptr<RdmaPortEngine> ingressPortEngine =
            GetRdmaPortEngineByDeviceIndex_(roceIngressDeviceIndex);
        if (ingressPortEngine != nullptr) {
          ingressPortEngine->HandleAck(roceHeader, TPheader);
        }
      }

      RoceTransportEngine::RxResult rxResult =
          m_roceTransport->OnRxPacket(roceHeader, TPheader, p, from, formIndex);
      if (!rxResult.accepted) {
        continue;
      }

      if (rxResult.needAck) {
        MaybeSendRoceAck_(from,
                          tag,
                          src,
                          dst,
                          roceHeader.GetQpn(),
                          formIndex,
                          roceHeader.GetOpcode());
      }
      if (rxResult.readyPackets.empty()) {
        continue;
      }

      for (const auto& readyPacket : rxResult.readyPackets) {
        ProcessThreePhasePayload_(
            readyPacket.payload != nullptr ? readyPacket.payload->Copy() : Create<Packet>(0),
            readyPacket.tpHeader,
            readyPacket.from,
            readyPacket.formIndex,
            readyPacket.hasTransportFragmentId,
            readyPacket.transportFragmentId);
      }
      continue;
    }

    ProcessThreePhasePayload_(p, TPheader, from, formIndex, false, 0);
  }
}

void
sendflowclient::ProcessThreePhasePayload_(Ptr<Packet> p,
                                          const ThreePHeader& TPheader,
                                          const Address& from,
                                          uint32_t formIndex,
                                          bool hasTransportFragmentId,
                                          uint32_t transportFragmentId)
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
    if(check_recvfunc(tag,src,dst) == false){
      m_recvfuncflag[flowKey] = 1;
      return ;
    }
    fuc_and_par fp = m_recvfunctable[flowKey];
    fp.msg_handler(fp.fun_arg);
    m_recvfunctable.erase(flowKey);
    m_recvfuncflag.erase(flowKey);
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
                if (IsRoceFullLogEnabled(m_roceCfg)) {
                    NS_LOG_INFO("[ROCE][RX][DUPLICATE_DATA_IGNORED] node=" << GetNode()->GetId()
                                << " tag=" << tag
                                << " src=" << src
                                << " dst=" << dst
                                << " fragmentPsn=" << transportFragmentId
                                << " deliveredCount=" << m_dataPacketsReceived[flowKey]
                                << "/" << m_dataPacketsExpected[flowKey]
                                << " TIME=" << Simulator::Now().GetFemtoSeconds());
                }
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
            if (check_recvfunc(tag,src,dst)){
              fuc_and_par fp = m_recvfunctable[flowKey];

              AstraSim::RecvPacketEventHadndlerData* ehd = (AstraSim::RecvPacketEventHadndlerData*) fp.fun_arg;
              ehd->flowTag=m_request[flowKey].flowTag;

              fp.msg_handler(fp.fun_arg);
              m_recvfunctable.erase(flowKey);
            }

            // 这几张表只是 3P 收包过程中的临时 scratch 状态：
            // - m_recvfuncflag: REQUEST 先到、回调后注册时的占位
            // - m_alldatasize: DATA 聚合过程中的累计字节
            // - m_request / m_recvHash: 传给上层回调或旧轮询路径的临时结果
            // RX_DONE 之后无论是否存在回调，这些 flow 级状态都不该继续滞留。
            m_recvHash.erase(flowKey);
            m_request.erase(flowKey);
            m_alldatasize.erase(flowKey);
            m_recvfuncflag.erase(flowKey);
            m_dataPacketsExpected.erase(flowKey);
            m_dataPacketsReceived.erase(flowKey);
            m_deliveredDataFragmentIds.erase(flowKey);
        }
    } else {
        NS_LOG_WARN("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst << ": Received DATA packet without expected count setup.");
    }

  }
  break;
  case ThreePHeader::ACT:
  {
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
        }
    } else {
        NS_LOG_WARN("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst << ": Received ACT packet without expected count setup.");
    }
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
  NS_LOG_INFO("TriggerSendTo_now declared now time = "
              << Simulator::Now().GetFemtoSeconds()
              << " src=" << src
              << " dst=" << dst
              << " tag=" << tag
              << " size=" << datasize);
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

  Ptr<Node> node = GetNode();
  NS_ASSERT(node != nullptr);
  Ptr<NetDevice> device = node->GetDevice(0);
  NS_ASSERT(device != nullptr);
  const uint32_t mtu = device->GetMtu();
  const RoceFlowKey flowKey{tag, src, dst};

  uint64_t remainingBytes = 1;
  uint64_t totalPackets = 1;
  RoceTxClass txClass = RoceTxClass::Control;
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
  }

  RoceTxWorkItem item;
  item.workId = 0;
  item.state = state;
  item.flowKey = flowKey;
  item.qpKey = RoceQpKey{src, dst, 0};
  item.dest = dest;
  item.sendMode = sendmodel;
  item.mindex = mindex;
  item.totalBytes = datasize;
  item.remainingBytes = remainingBytes;
  item.packetIndex = 0;
  item.totalPackets = totalPackets;
  item.txClass = txClass;
  if (!m_roceTransport->Enqueue(&item)) {
    NS_FATAL_ERROR("[ROCE][SCHED] failed to enqueue work"
                   << " tag=" << tag
                   << " src=" << src
                   << " dst=" << dst
                   << " state=" << static_cast<uint32_t>(state));
  }
  if (IsRoceBasicLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][SCHED][ENQ] workId=" << item.workId
                << " state=" << static_cast<uint32_t>(state)
                << " lane=" << GetRoceLaneLabel(state)
                << " tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << item.qpKey.qpn
                << " totalBytes=" << item.totalBytes
                << " totalPackets=" << item.totalPackets
                << " class=" << static_cast<uint32_t>(item.txClass));
  }
  ScheduleRoceDispatch_();
}

void
sendflowclient::ScheduleRoceDispatch_() {
  if (!IsRoceEnabled() || m_roceTransport == nullptr) {
    return;
  }
  if (!m_roceTransport->HasPendingWork()) {
    return;
  }
  if (m_roceDispatchEvent.IsPending()) {
    return;
  }
  m_roceDispatchEvent = Simulator::Schedule(
      Seconds(0),
      &sendflowclient::DispatchOneRoceSlice_,
      this);
}

void
sendflowclient::DispatchOneRoceSlice_() {
  if (!IsRoceEnabled() || m_roceTransport == nullptr) {
    return;
  }

  Ptr<Node> node = GetNode();
  NS_ASSERT(node != nullptr);
  Ptr<NetDevice> device = node->GetDevice(0);
  NS_ASSERT(device != nullptr);

  RoceTransportEngine::DispatchAction action;
  if (!m_roceTransport->TryGetNextDispatchAction(device->GetMtu(), &action)) {
    return;
  }

  if (action.kind == RoceTransportEngine::DispatchKind::Retransmission) {
    Ptr<Packet> retransPacket = action.retransmission.packet->Copy();
    MarkRdmaDispatchTagRetransmission_(retransPacket);
    realsend(retransPacket,
             action.retransmission.dest,
             action.retransmission.sendMode,
             action.retransmission.mindex);
  } else if (action.kind == RoceTransportEngine::DispatchKind::NewSlice) {
    const RoceTxSlice& slice = action.slice;
    if (IsRoceFullLogEnabled(m_roceCfg)) {
      NS_LOG_INFO("[ROCE][SCHED][TX] workId=" << slice.workId
                  << " state=" << static_cast<uint32_t>(slice.state)
                  << " lane=" << GetRoceLaneLabel(slice.state)
                  << " tag=" << slice.flowKey.tag
                  << " src=" << slice.flowKey.src
                  << " dst=" << slice.flowKey.dst
                  << " qpn=" << slice.qpKey.qpn
                  << " packetIndex=" << slice.packetIndex
                  << " sliceBytes=" << slice.sliceBytes
                  << " totalPackets=" << slice.totalPackets
                  << " last=" << slice.isLastSlice
                  << " class=" << static_cast<uint32_t>(slice.txClass));
    }

    Ptr<Packet> packet;
    RoceSimHeader roceHeader;
    if (!BuildPacketForRoceSlice_(slice, &packet, &roceHeader)) {
      NS_FATAL_ERROR("[ROCE][SCHED] failed to build packet for dispatched slice"
                     << " tag=" << slice.flowKey.tag
                     << " src=" << slice.flowKey.src
                     << " dst=" << slice.flowKey.dst
                     << " workId=" << slice.workId);
    }
    realsend(packet, slice.dest, slice.sendMode, slice.mindex);
    m_roceTransport->OnPacketSent(slice, packet, roceHeader);
  }

  if (m_roceTransport->HasPendingWork()) {
    ScheduleRoceDispatch_();
  }
}

bool
sendflowclient::BuildPacketForRoceSlice_(const RoceTxSlice& slice,
                                         Ptr<Packet>* packetOut,
                                         RoceSimHeader* headerOut) {
  if (packetOut == nullptr) {
    return false;
  }

  HopCountHeader hopHeader;
  hopHeader.SetHopCount(0);
  hopHeader.SetIsFlood(false);

  ThreePHeader TPheader;
  TPheader.SetState(slice.state);
  TPheader.SetTag(slice.flowKey.tag);
  TPheader.SetSrc(slice.flowKey.src);
  TPheader.SetDst(slice.flowKey.dst);
  TPheader.SetDataSize(slice.totalBytes);

  FlowTagHeader FTHeader;
  const uint16_t roceWireBytes = GetVisibleRoceWireBytes(slice.state, m_roceCfg);
  auto SafePayloadBytes = [](int64_t rawBytes) -> uint16_t {
    return rawBytes > 0 ? static_cast<uint16_t>(rawBytes) : 0;
  };

  Ptr<Packet> packet;
  uint16_t realPacketPayload = 0;
  if (slice.state == ThreePHeader::REQUEST) {
    realPacketPayload = SafePayloadBytes(
        static_cast<int64_t>(reqlen) - TPheader.GetSerializedSize() -
        hopHeader.GetSerializedSize() - roceWireBytes);
    packet = Create<Packet>(realPacketPayload);
  } else if (slice.state == ThreePHeader::DATA) {
    FTHeader.FromFlowTag(get_flowtag(slice.flowKey.tag, slice.flowKey.src, slice.flowKey.dst).flowTag);
    FTHeader.set_packetnum(slice.totalPackets);
    const int64_t baseBytes = static_cast<int64_t>(slice.sliceBytes);
    realPacketPayload = SafePayloadBytes(
        baseBytes - TPheader.GetSerializedSize() -
        hopHeader.GetSerializedSize() - FTHeader.GetSerializedSize());
    packet = Create<Packet>(realPacketPayload);
    packet->AddHeader(FTHeader);
  } else if (slice.state == ThreePHeader::ACT) {
    realPacketPayload = SafePayloadBytes(
        static_cast<int64_t>(acklen) - TPheader.GetSerializedSize() -
        hopHeader.GetSerializedSize() - roceWireBytes);
    packet = Create<Packet>(realPacketPayload);
  } else {
    return false;
  }

  RoceSimHeader roceHeader;
  const bool hasRoceHeader =
      BuildRoceTxHeader_(slice.state,
                         slice.totalBytes,
                         static_cast<uint32_t>(realPacketPayload),
                         slice.flowKey.tag,
                         slice.flowKey.src,
                         slice.flowKey.dst,
                         &roceHeader);
  if (!hasRoceHeader) {
    return false;
  }

  packet->AddHeader(roceHeader);
  packet->AddHeader(TPheader);
  packet->AddHeader(hopHeader);
  AttachRdmaDispatchTag_(packet, slice, roceHeader);
  if (headerOut != nullptr) {
    *headerOut = roceHeader;
  }
  *packetOut = packet;
  return true;
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
      return;
    }
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

bool
sendflowclient::BuildRoceAckPacket_(uint32_t tag,
                                    uint32_t src,
                                    uint32_t dst,
                                    uint32_t qpn,
                                    Ptr<Packet>* packetOut,
                                    RoceSimHeader* headerOut) {
  if (!IsRoceEnabled() || packetOut == nullptr || m_roceTransport == nullptr) {
    return false;
  }

  HopCountHeader hopHeader;
  hopHeader.SetHopCount(0);
  hopHeader.SetIsFlood(false);

  ThreePHeader TPheader;
  TPheader.SetState(ThreePHeader::IDLE);
  TPheader.SetTag(tag);
  TPheader.SetSrc(src);
  TPheader.SetDst(dst);
  TPheader.SetDataSize(0);

  RoceSimHeader ackHeader;
  if (!m_roceTransport->BuildAckHeader(tag, src, dst, qpn, &ackHeader)) {
    return false;
  }
  if (headerOut != nullptr) {
    *headerOut = ackHeader;
  }

  Ptr<Packet> packet = Create<Packet>(0);
  packet->AddHeader(ackHeader);
  packet->AddHeader(TPheader);
  packet->AddHeader(hopHeader);
  AttachRdmaAckDispatchTag_(packet, tag, src, dst, ackHeader);
  *packetOut = packet;
  return true;
}

void
sendflowclient::MaybeSendRoceAck_(const Address& dest,
                                  uint32_t tag,
                                  uint32_t src,
                                  uint32_t dst,
                                  uint32_t qpn,
                                  uint32_t rxInDevIndexForLoopback,
                                  RoceOpcode rxOpcode) {
  if (!IsRoceEnabled() || !m_roceCfg.enableTransportAck ||
      rxOpcode == RoceOpcode::Ack) {
    return;
  }

  Ptr<Packet> ackPacket;
  RoceSimHeader ackHeader;
  if (!BuildRoceAckPacket_(tag, src, dst, qpn, &ackPacket, &ackHeader)) {
    NS_LOG_WARN("[ROCE][ACK][TX] failed to build ack"
                << " node=" << GetNode()->GetId()
                << " tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << qpn);
    return;
  }
  const RoceQpKey qpKey{src, dst, qpn};
  if (IsRoceFullLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][ACK][TX] tag=" << tag
                << " node=" << GetNode()->GetId()
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << qpn
                << " psn=" << ackHeader.GetPsn()
                << " ackPsn=" << ackHeader.GetAckPsn()
                << " expectedRxPsn=" << m_roceTransport->GetExpectedRxPsn(qpKey)
                << " pendingRxCount=" << m_roceTransport->GetPendingRxCount(qpKey)
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }
  realsend(ackPacket, dest, FlowSendMode::Loopback, rxInDevIndexForLoopback);
}


void
sendflowclient::initialization(uint16_t Pronum,uint32_t m_outDev,uint32_t m_innerDev,Ptr<NetDevice> bridgenode)
{
    localbridgeindex=m_innerDev;
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
  const uint16_t roceWireBytes =
      IsRoceEnabled() ? GetVisibleRoceWireBytes(state, m_roceCfg) : 0;
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
      RoceSimHeader roceHeader;
      const bool hasRoceHeader =
          BuildRoceTxHeader_(state,
                             datasize,
                             static_cast<uint32_t>(real_packet_size),
                             tag,
                             src,
                             dst,
                             &roceHeader);
      if (hasRoceHeader) {
        packet->AddHeader(roceHeader);
      }
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
        RoceSimHeader roceHeader;
        const bool hasRoceHeader =
            BuildRoceTxHeader_(state,
                               datasize,
                               static_cast<uint32_t>(real_packet_size),
                               tag,
                               src,
                               dst,
                               &roceHeader);
        packet->AddHeader(FTHeader);
        if (hasRoceHeader) {
          packet->AddHeader(roceHeader);
        }
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
      RoceSimHeader roceHeader;
      const bool hasRoceHeader =
          BuildRoceTxHeader_(state,
                             static_cast<uint64_t>(acklen),
                             static_cast<uint32_t>(real_packet_size),
                             tag,
                             src,
                             dst,
                             &roceHeader);
      if (hasRoceHeader) {
        packet->AddHeader(roceHeader);
      }
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

      // NS_LOG_INFO("[RZV] CALLBACK fired tag="<<tag<<" src="<<src<<" dst="<<dst);
      
      fp.msg_handler(fp.fun_arg);
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

bool
sendflowclient::BuildRoceTxHeader_(ThreePHeader::State state,
                                   uint64_t totalDataSize,
                                   uint32_t payloadBytes,
                                   uint32_t tag,
                                   uint32_t src,
                                   uint32_t dst,
                                   RoceSimHeader* headerOut) {
  if (!IsRoceEnabled()) {
    return false;
  }
  if (headerOut == nullptr || m_roceTransport == nullptr) {
    return false;
  }

  if (!m_roceActiveLogged) {
    // 每个 app 实例只打印一次 ACTIVE，避免日志过量。
    if (IsRoceBasicLogEnabled(m_roceCfg)) {
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
    }
    m_roceActiveLogged = true;
  }

  if (!m_roceTransport->BuildTxHeader(
          state, totalDataSize, payloadBytes, tag, src, dst, headerOut)) {
    NS_LOG_WARN("[ROCE][TX] failed to build RoCE header tag="
                << tag << " src=" << src << " dst=" << dst);
    return false;
  }
  return true;
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
  if (m_roceDispatchEvent.IsPending()) {
    m_roceDispatchEvent.Cancel();
  }
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
    engine->SetTxNotifyCallback(MakeCallback(&sendflowclient::HandleRdmaPortPacketSent_, this));
  }
}

void
sendflowclient::HandleRdmaPortPacketSent_(Ptr<Packet> packet) {
  if (packet == nullptr || m_roceTransport == nullptr) {
    return;
  }

  RdmaDispatchTag tag;
  if (!packet->PeekPacketTag(tag)) {
    return;
  }
  if (tag.IsTransportAck()) {
    return;
  }

  const RoceFlowKey flowKey = tag.GetFlowKey();
  const RoceQpKey qpKey{flowKey.src, flowKey.dst, tag.GetQpn()};
  m_roceTransport->OnPacketWireTx(qpKey, tag.GetPsn(), tag.IsRetransmission());
  if (!tag.IsRetransmission()) {
    FinalizeRdmaTaggedDispatch_(tag);
  }
}

void
sendflowclient::AttachRdmaDispatchTag_(Ptr<Packet> packet,
                                       const RoceTxSlice& slice,
                                       const RoceSimHeader& header) {
  if (packet == nullptr) {
    return;
  }

  RdmaDispatchTag tag;
  tag.SetWorkId(slice.workId);
  tag.SetFlowKey(slice.flowKey);
  tag.SetState(slice.state);
  tag.SetQpn(slice.qpKey.qpn);
  tag.SetPsn(header.GetPsn());
  tag.SetOpcode(header.GetOpcode());
  tag.SetTxClass(slice.txClass);
  tag.SetTotalBytes(slice.totalBytes);
  tag.SetSliceBytes(slice.sliceBytes);
  tag.SetPacketIndex(slice.packetIndex);
  tag.SetTotalPackets(slice.totalPackets);
  tag.SetLastSlice(slice.isLastSlice);
  tag.SetRetransmission(false);
  tag.SetTransportAck(false);
  packet->AddPacketTag(tag);
}

void
sendflowclient::AttachRdmaAckDispatchTag_(Ptr<Packet> packet,
                                          uint32_t tagValue,
                                          uint32_t src,
                                          uint32_t dst,
                                          const RoceSimHeader& header) {
  if (packet == nullptr) {
    return;
  }

  RdmaDispatchTag tag;
  tag.SetFlowKey(RoceFlowKey{tagValue, src, dst});
  tag.SetState(ThreePHeader::IDLE);
  tag.SetQpn(header.GetQpn());
  tag.SetPsn(header.GetPsn());
  tag.SetOpcode(header.GetOpcode());
  tag.SetTxClass(RoceTxClass::Control);
  tag.SetTotalBytes(0);
  tag.SetSliceBytes(0);
  tag.SetPacketIndex(0);
  tag.SetTotalPackets(1);
  tag.SetLastSlice(true);
  tag.SetRetransmission(false);
  tag.SetTransportAck(true);
  packet->AddPacketTag(tag);
}

void
sendflowclient::MarkRdmaDispatchTagRetransmission_(Ptr<Packet> packet) {
  if (packet == nullptr) {
    return;
  }

  RdmaDispatchTag tag;
  if (!packet->PeekPacketTag(tag)) {
    return;
  }
  tag.SetRetransmission(true);
  packet->ReplacePacketTag(tag);
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


}
