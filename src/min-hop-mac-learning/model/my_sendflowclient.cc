#include "my_sendflowclient.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/inet-socket-address.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "roce-qp-engine.h"
#include "roce-sim-header.h"
#include "roce-tx-scheduler.h"
#include "roce-three-phase-fsm.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <chrono>


namespace ns3 {

NS_LOG_COMPONENT_DEFINE("SendFlowClient");

namespace {

constexpr uint16_t kRoceBthBytes = 12;

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

RoceUserPhase
ToRoceUserPhase(ThreePHeader::State state) {
  switch (state) {
    case ThreePHeader::REQUEST:
      return RoceUserPhase::Request;
    case ThreePHeader::DATA:
      return RoceUserPhase::Data;
    case ThreePHeader::ACT:
      return RoceUserPhase::Act;
    default:
      return RoceUserPhase::None;
  }
}

RoceOpcode
ToRoceOpcode(ThreePHeader::State state,
             const RoceConfig& cfg,
             const RoceThreePhaseFsm* fsm) {
  // 根据配置字符串决定 REQUEST/DATA 阶段偏向的 RoCE opcode。
  // - mode 含 read: REQUEST=ReadReq, DATA=ReadResp
  // - mode 含 send: DATA=Send
  // - ACT 作为业务控制报文，固定映射为 SEND，而不是 RC 传输 ACK
  // - 其余优先采用 FSM 决策，最终回退为 RdmaWrite
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
      if (fsm != nullptr) {
        return fsm->DataOpcode();
      }
      return RoceOpcode::RdmaWrite;
    case ThreePHeader::ACT:
      return RoceOpcode::Send;
    default:
      return RoceOpcode::Send;
  }
}

uint16_t
GetVisibleRoceWireBytes(ThreePHeader::State state,
                        const RoceConfig& cfg,
                        const RoceThreePhaseFsm* fsm) {
  return GetVisibleRoceWireBytes(ToRoceOpcode(state, cfg, fsm));
}

void
EnsureQpReadyToSend(RoceQpEngine* engine, const RoceQpKey& key) {
  if (engine == nullptr) {
    return;
  }
  // 仿真里在首次发包前主动把 QP 推到 RTS，避免业务路径阻塞在状态初始化。
  engine->EnsureQp(key);
  RoceQpState state = engine->GetQpState(key);
  if (state == RoceQpState::Err) {
    engine->ModifyQpState(key, RoceQpState::Reset);
    state = engine->GetQpState(key);
  }
  if (state == RoceQpState::Reset) {
    engine->ModifyQpState(key, RoceQpState::Init);
    state = engine->GetQpState(key);
  }
  if (state == RoceQpState::Init) {
    engine->ModifyQpState(key, RoceQpState::Rtr);
    state = engine->GetQpState(key);
  }
  if (state == RoceQpState::Rtr) {
    engine->ModifyQpState(key, RoceQpState::Rts);
  }
}

} // namespace


// ---------- 客户端实现 ----------

sendflowclient::sendflowclient()
{
  reqlen=acklen=100;
  localbridgeindex=0;
  m_roceQpEngine = std::make_unique<RoceQpEngine>();
  m_roceFsm = std::make_unique<RoceThreePhaseFsm>();
  m_roceTxScheduler = std::make_unique<RoceTxScheduler>();
  m_roceQpEngine->Configure(m_roceCfg);
  m_roceFsm->Configure(m_roceCfg);
  m_roceTxScheduler->Configure(m_roceCfg);
}
sendflowclient::~sendflowclient()
{
  StopApplication();
}

void sendflowclient::StartApplication()
{

}

void sendflowclient::StopApplication()
{
  if (m_roceDispatchEvent.IsPending()) {
    m_roceDispatchEvent.Cancel();
  }
  for (auto& [_, timerState] : m_roceRetransTimers) {
    if (timerState.event.IsPending()) {
      timerState.event.Cancel();
    }
  }
  m_roceRetransTimers.clear();

  if (m_io) {
    m_io->Stop();
  }

  m_sendfunctable.clear();
  m_recvfunctable.clear();
  m_request.clear();
  m_recvfuncflag.clear();
  m_recvHash.clear();
  m_dataPacketsExpected.clear();
  m_dataPacketsReceived.clear();
  m_ackPacketsExpected.clear();
  m_ackPacketsReceived.clear();
  m_alldatasize.clear();
  m_roceFlowToQpn.clear();
  m_roceOutstandingPackets.clear();
  m_roceBufferedUpperPackets.clear();
  m_rocePendingRetransmissions.clear();
  m_roceRecoveryTargetPsns.clear();

  if (m_roceTxScheduler) {
    m_roceTxScheduler->Reset();
  }
  if (m_roceQpEngine) {
    m_roceQpEngine->Reset();
  }
  if (m_roceFsm) {
    m_roceFsm->Reset();
  }
}


void sendflowclient::HandleRead(Ptr<Socket> socket)
{

  Ptr<Packet> packet;
  Address from;

  while ((packet = socket->RecvFrom(from))) {
    Ptr<Packet> p = packet->Copy();
    ThreePHeader TPheader;
    HopCountHeader hopHeader;
    FlowTagHeader FTHeader;
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
      bool bufferedForReorder = false;
      bool overflowedReorderWindow = false;
      const bool shouldDeliverUpper =
          HandleRoceRxHeader_(roceHeader,
                              TPheader.GetState(),
                              tag,
                              src,
                              dst,
                              from,
                              formIndex,
                              &bufferedForReorder,
                              &overflowedReorderWindow);
      if (bufferedForReorder) {
        const RoceQpKey qpKey{src, dst, roceHeader.GetQpn()};
        m_roceBufferedUpperPackets[qpKey][roceHeader.GetPsn()] =
            BufferedRoceUpperPacket{p->Copy(), TPheader, from, formIndex};
        if (IsRoceFullLogEnabled(m_roceCfg)) {
          NS_LOG_INFO("[ROCE][RX][REORDER_BUFFER] node=" << GetNode()->GetId()
                      << " src=" << src
                      << " dst=" << dst
                      << " qpn=" << roceHeader.GetQpn()
                      << " psn=" << roceHeader.GetPsn()
                      << " expectedRxPsn=" << m_roceQpEngine->GetExpectedRxPsn(qpKey)
                      << " pendingRxCount=" << m_roceQpEngine->GetPendingRxCount(qpKey)
                      << " bufferedUpperCount=" << m_roceBufferedUpperPackets[qpKey].size());
        }
      }
      if (!shouldDeliverUpper) {
        continue;
      }
    }


    // NS_LOG_DEBUG("[" << node->timeGetId() << "] Received packet for flow " << flowid
    //                  << " from " << from
    //                  << " with state=" << (int)TPheader.GetState()
    //                  << ", header datasize=" << datasize
    //                  << ", actual packet size=" << packet->GetSize());

    ProcessThreePhasePayload_(p, TPheader, from, formIndex);
    if (hasRoceHeader) {
      const RoceQpKey qpKey{src, dst, roceHeader.GetQpn()};
      MaybeReleaseBufferedRocePackets_(qpKey);
    }
  }
}

void
sendflowclient::ProcessThreePhasePayload_(Ptr<Packet> p,
                                          const ThreePHeader& TPheader,
                                          const Address& from,
                                          uint32_t formIndex)
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
              m_recvHash.erase(flowKey);
              m_recvfunctable.erase(flowKey);
              m_request.erase(flowKey);
            }

            m_dataPacketsExpected.erase(flowKey);
            m_dataPacketsReceived.erase(flowKey);
            m_alldatasize.erase(flowKey);
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

            CleanupFlowRuntimeTables_(flowKey);
            CleanupRoceQpRuntimeState_(flowKey);
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
sendflowclient::MaybeReleaseBufferedRocePackets_(const RoceQpKey& qpKey)
{
  if (m_roceQpEngine == nullptr) {
    return;
  }

  auto bufferedIt = m_roceBufferedUpperPackets.find(qpKey);
  if (bufferedIt == m_roceBufferedUpperPackets.end()) {
    return;
  }

  const uint32_t deliverUpToPsn = m_roceQpEngine->GetExpectedRxPsn(qpKey);
  auto& packets = bufferedIt->second;
  while (!packets.empty()) {
    auto it = packets.begin();
    if (it->first >= deliverUpToPsn) {
      break;
    }
    if (IsRoceFullLogEnabled(m_roceCfg)) {
      NS_LOG_INFO("[ROCE][RX][REORDER_RELEASE] node=" << GetNode()->GetId()
                  << " src=" << qpKey.src
                  << " dst=" << qpKey.dst
                  << " qpn=" << qpKey.qpn
                  << " psn=" << it->first
                  << " deliverUpToPsn=" << deliverUpToPsn
                  << " remainingBuffered=" << packets.size());
    }
    ProcessThreePhasePayload_(it->second.payload->Copy(),
                              it->second.tpHeader,
                              it->second.from,
                              it->second.formIndex);
    packets.erase(it);
  }
  if (packets.empty()) {
    m_roceBufferedUpperPackets.erase(bufferedIt);
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
  if (m_roceTxScheduler == nullptr) {
    NS_FATAL_ERROR("[ROCE][SCHED] missing tx scheduler");
  }

  Ptr<Node> node = GetNode();
  NS_ASSERT(node != nullptr);
  Ptr<NetDevice> device = node->GetDevice(0);
  NS_ASSERT(device != nullptr);
  const uint32_t mtu = device->GetMtu();
  const RoceFlowKey flowKey{tag, src, dst};
  const uint32_t qpn = GetOrAllocRoceQpn_(tag, src, dst);

  uint64_t remainingBytes = 1;
  uint64_t totalPackets = 1;
  RoceTxClass txClass = RoceTxClass::Control;
  if (state == ThreePHeader::DATA) {
    const uint32_t visibleRoceBytes =
        GetVisibleRoceWireBytes(state, m_roceCfg, m_roceFsm.get());
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
  item.workId = m_nextRoceWorkId++;
  item.state = state;
  item.flowKey = flowKey;
  item.qpKey = RoceQpKey{src, dst, qpn};
  item.dest = dest;
  item.sendMode = sendmodel;
  item.mindex = mindex;
  item.totalBytes = datasize;
  item.remainingBytes = remainingBytes;
  item.packetIndex = 0;
  item.totalPackets = totalPackets;
  item.txClass = txClass;
  if (!m_roceTxScheduler->Enqueue(item)) {
    NS_FATAL_ERROR("[ROCE][SCHED] failed to enqueue work"
                   << " tag=" << tag
                   << " src=" << src
                   << " dst=" << dst
                   << " state=" << static_cast<uint32_t>(state));
  }
  if (IsRoceBasicLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][SCHED][ENQ] workId=" << item.workId
                << " state=" << static_cast<uint32_t>(state)
                << " tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << qpn
                << " totalBytes=" << item.totalBytes
                << " totalPackets=" << item.totalPackets
                << " class=" << static_cast<uint32_t>(item.txClass));
  }
  ScheduleRoceDispatch_();
}

void
sendflowclient::ScheduleRoceDispatch_() {
  if (!IsRoceEnabled() || m_roceTxScheduler == nullptr) {
    return;
  }
  if (m_rocePendingRetransmissions.empty() &&
      !m_roceTxScheduler->HasPendingWork()) {
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
  if (!IsRoceEnabled() || m_roceTxScheduler == nullptr) {
    return;
  }

  if (!m_rocePendingRetransmissions.empty()) {
    DispatchPendingRoceRetransmissions_();
    if (!m_rocePendingRetransmissions.empty() || m_roceTxScheduler->HasPendingWork()) {
      ScheduleRoceDispatch_();
    }
    return;
  }

  Ptr<Node> node = GetNode();
  NS_ASSERT(node != nullptr);
  Ptr<NetDevice> device = node->GetDevice(0);
  NS_ASSERT(device != nullptr);

  const std::set<RoceQpKey> blockedQps = CollectBlockedRoceQps_();
  RoceTxSlice slice;
  if (!m_roceTxScheduler->PickNextSlice(device->GetMtu(), &blockedQps, &slice)) {
    return;
  }
  if (IsRoceFullLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][SCHED][TX] workId=" << slice.workId
                << " state=" << static_cast<uint32_t>(slice.state)
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
  TrackRoceOutstandingPacket_(slice, packet, roceHeader);
  FinalizeRoceSliceDispatch_(slice);

  if (m_roceTxScheduler->HasPendingWork()) {
    ScheduleRoceDispatch_();
  }
}

void
sendflowclient::DispatchPendingRoceRetransmissions_() {
  if (m_rocePendingRetransmissions.empty()) {
    return;
  }

  auto it = m_rocePendingRetransmissions.begin();
  const RoceQpKey qpKey = it->first;
  const RoceOutstandingPacket outstanding = it->second;
  m_rocePendingRetransmissions.erase(it);

  if (IsRoceBasicLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][RTX][SEND] tag=" << outstanding.flowKey.tag
                << " node=" << GetNode()->GetId()
                << " src=" << outstanding.flowKey.src
                << " dst=" << outstanding.flowKey.dst
                << " qpn=" << qpKey.qpn
                << " psn=" << outstanding.psn
                << " retry=" << outstanding.retries
                << " outstandingCount=" << GetRoceOutstandingCount_(qpKey)
                << " oldestOutstanding=" << GetRoceOldestOutstandingPsn_(qpKey)
                << " recoveryTarget=" << GetRoceRecoveryTargetPsn_(qpKey)
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }
  realsend(outstanding.packet->Copy(),
           outstanding.dest,
           outstanding.sendMode,
           outstanding.mindex);
  ArmRoceRetransTimer_(qpKey);
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
  const uint16_t roceWireBytes =
      GetVisibleRoceWireBytes(slice.state, m_roceCfg, m_roceFsm.get());
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
    return;
  }

  if (slice.state == ThreePHeader::ACT && !m_roceCfg.enableTransportAck) {
    // 关闭 transport ACK 时，ACT 发完后不会再收到 ACK 回包；这里直接回收 QP 状态。
    CleanupRoceQpRuntimeState_(slice.flowKey, &slice.qpKey);
  }
}

bool
sendflowclient::BuildRoceAckPacket_(uint32_t tag,
                                    uint32_t src,
                                    uint32_t dst,
                                    uint32_t qpn,
                                    Ptr<Packet>* packetOut,
                                    RoceSimHeader* headerOut) {
  if (!IsRoceEnabled() || packetOut == nullptr || m_roceQpEngine == nullptr) {
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

  const RoceQpKey qpKey{src, dst, qpn};
  EnsureQpReadyToSend(m_roceQpEngine.get(), qpKey);

  RoceSimHeader ackHeader;
  ackHeader.SetVersion(1);
  ackHeader.SetOpcode(RoceOpcode::Ack);
  ackHeader.SetUserPhase(RoceUserPhase::None);
  ackHeader.SetFlags(0x4);
  ackHeader.SetQpn(qpn);
  const uint32_t ackPsn =
      m_roceQpEngine->GetExpectedRxPsn(qpKey) == 0
          ? 0
          : (m_roceQpEngine->GetExpectedRxPsn(qpKey) - 1);
  ackHeader.SetPsn(ackPsn);
  ackHeader.SetAckPsn(ackPsn);
  ackHeader.SetTag(tag);
  ackHeader.SetSrc(src);
  ackHeader.SetDst(dst);
  if (headerOut != nullptr) {
    *headerOut = ackHeader;
  }

  Ptr<Packet> packet = Create<Packet>(0);
  packet->AddHeader(ackHeader);
  packet->AddHeader(TPheader);
  packet->AddHeader(hopHeader);
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
  if (IsRoceFullLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][ACK][TX] tag=" << tag
                << " node=" << GetNode()->GetId()
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << qpn
                << " psn=" << ackHeader.GetPsn()
                << " ackPsn=" << ackHeader.GetAckPsn()
                << " expectedRxPsn=" << m_roceQpEngine->GetExpectedRxPsn(RoceQpKey{src, dst, qpn})
                << " pendingRxCount=" << m_roceQpEngine->GetPendingRxCount(RoceQpKey{src, dst, qpn})
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }
  realsend(ackPacket, dest, FlowSendMode::Loopback, rxInDevIndexForLoopback);
}

void
sendflowclient::TrackRoceOutstandingPacket_(const RoceTxSlice& slice,
                                            const Ptr<Packet>& packet,
                                            const RoceSimHeader& header) {
  if (!IsRoceEnabled() || !m_roceCfg.enableTransportAck ||
      header.GetOpcode() == RoceOpcode::Ack || packet == nullptr) {
    return;
  }

  RoceOutstandingPacket outstanding;
  outstanding.packet = packet->Copy();
  outstanding.dest = slice.dest;
  outstanding.sendMode = slice.sendMode;
  outstanding.mindex = slice.mindex;
  outstanding.flowKey = slice.flowKey;
  outstanding.qpKey = slice.qpKey;
  outstanding.psn = header.GetPsn();
  outstanding.retries = 0;
  m_roceOutstandingPackets[slice.qpKey][header.GetPsn()] = outstanding;
  ArmRoceRetransTimer_(slice.qpKey);
}

void
sendflowclient::HandleRoceAck_(const RoceFlowKey& flowKey,
                               const RoceQpKey& qpKey,
                               const RoceSimHeader& roceHeader) {
  auto mapIt = m_roceOutstandingPackets.find(qpKey);
  if (mapIt == m_roceOutstandingPackets.end()) {
    return;
  }

  const uint32_t ackPsn = roceHeader.GetAckPsn();
  const uint32_t outstandingBefore = static_cast<uint32_t>(mapIt->second.size());
  const uint32_t oldestOutstandingBefore = mapIt->second.begin()->first;
  const uint32_t recoveryTargetBefore = GetRoceRecoveryTargetPsn_(qpKey);
  auto& packets = mapIt->second;
  bool qpDrained = false;
  for (auto it = packets.begin(); it != packets.end();) {
    if (it->first > ackPsn) {
      ++it;
      continue;
    }
    it = packets.erase(it);
  }
  if (packets.empty()) {
    CancelRoceRetransTimer_(qpKey);
    m_roceOutstandingPackets.erase(mapIt);
    qpDrained = true;
  } else {
    ArmRoceRetransTimer_(qpKey);
  }

  auto recoveryIt = m_roceRecoveryTargetPsns.find(qpKey);
  if (recoveryIt != m_roceRecoveryTargetPsns.end() &&
      ackPsn >= recoveryIt->second) {
    m_roceRecoveryTargetPsns.erase(recoveryIt);
    m_rocePendingRetransmissions.erase(qpKey);
  }

  if (IsRoceFullLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][ACK][RX] tag=" << flowKey.tag
                << " node=" << GetNode()->GetId()
                << " src=" << flowKey.src
                << " dst=" << flowKey.dst
                << " qpn=" << qpKey.qpn
                << " psn=" << roceHeader.GetPsn()
                << " ackPsn=" << ackPsn
                << " outstandingBefore=" << outstandingBefore
                << " outstandingAfter=" << GetRoceOutstandingCount_(qpKey)
                << " oldestOutstandingBefore=" << oldestOutstandingBefore
                << " oldestOutstandingAfter=" << GetRoceOldestOutstandingPsn_(qpKey)
                << " recoveryTargetBefore=" << recoveryTargetBefore
                << " recoveryTargetAfter=" << GetRoceRecoveryTargetPsn_(qpKey)
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }

  if (qpDrained) {
    const bool hasPendingSchedulerWork =
        m_roceTxScheduler != nullptr &&
        m_roceTxScheduler->HasPendingWorkForQp(qpKey);
    if (!hasPendingSchedulerWork) {
      CleanupRoceQpRuntimeState_(flowKey, &qpKey);
    }
  }
  ScheduleRoceDispatch_();
}

void
sendflowclient::HandleRoceRetransTimeout_(RoceQpKey qpKey) {
  auto mapIt = m_roceOutstandingPackets.find(qpKey);
  if (mapIt == m_roceOutstandingPackets.end()) {
    CancelRoceRetransTimer_(qpKey);
    return;
  }
  auto packetIt = mapIt->second.begin();
  if (packetIt == mapIt->second.end()) {
    CancelRoceRetransTimer_(qpKey);
    return;
  }

  RoceOutstandingPacket& outstanding = packetIt->second;
  const uint32_t psn = outstanding.psn;
  if (outstanding.retries >= m_roceCfg.maxRetransmissions) {
    NS_LOG_WARN("[ROCE][RTX][DROP] tag=" << outstanding.flowKey.tag
                << " node=" << GetNode()->GetId()
                << " src=" << outstanding.flowKey.src
                << " dst=" << outstanding.flowKey.dst
                << " qpn=" << qpKey.qpn
                << " psn=" << psn
                << " retries=" << outstanding.retries
                << " outstandingCount=" << GetRoceOutstandingCount_(qpKey)
                << " oldestOutstanding=" << GetRoceOldestOutstandingPsn_(qpKey)
                << " recoveryTarget=" << GetRoceRecoveryTargetPsn_(qpKey));
    mapIt->second.erase(packetIt);
    if (mapIt->second.empty()) {
      CancelRoceRetransTimer_(qpKey);
      m_roceOutstandingPackets.erase(mapIt);
    } else {
      ArmRoceRetransTimer_(qpKey);
    }
    m_rocePendingRetransmissions.erase(qpKey);
    m_roceRecoveryTargetPsns.erase(qpKey);
    // Flow abort cleanup: keep per-flow/QP tables bounded even when transport gives up.
    CleanupFlowRuntimeTables_(outstanding.flowKey);
    CleanupRoceQpRuntimeState_(outstanding.flowKey, &qpKey);
    ScheduleRoceDispatch_();
    return;
  }

  outstanding.retries++;
  if (IsRoceBasicLogEnabled(m_roceCfg)) {
    NS_LOG_INFO("[ROCE][RTX][TIMEOUT] tag=" << outstanding.flowKey.tag
                << " node=" << GetNode()->GetId()
                << " src=" << outstanding.flowKey.src
                << " dst=" << outstanding.flowKey.dst
                << " qpn=" << qpKey.qpn
                << " psn=" << psn
                << " retry=" << outstanding.retries
                << " outstandingCount=" << GetRoceOutstandingCount_(qpKey)
                << " oldestOutstanding=" << GetRoceOldestOutstandingPsn_(qpKey)
                << " recoveryTarget=" << GetRoceRecoveryTargetPsn_(qpKey)
                << " TIME=" << Simulator::Now().GetFemtoSeconds());
  }
  m_rocePendingRetransmissions[qpKey] = outstanding;
  m_roceRecoveryTargetPsns[qpKey] = psn;
  ScheduleRoceDispatch_();
}

void
sendflowclient::ArmRoceRetransTimer_(const RoceQpKey& qpKey) {
  auto outstandingIt = m_roceOutstandingPackets.find(qpKey);
  if (outstandingIt == m_roceOutstandingPackets.end() ||
      outstandingIt->second.empty()) {
    CancelRoceRetransTimer_(qpKey);
    return;
  }

  const uint32_t oldestPsn = outstandingIt->second.begin()->first;
  auto& timerState = m_roceRetransTimers[qpKey];
  if (timerState.event.IsPending()) {
    timerState.event.Cancel();
  }
  timerState.trackedPsn = oldestPsn;
  timerState.event = Simulator::Schedule(
      NanoSeconds(m_roceCfg.ackTimeoutNs),
      &sendflowclient::HandleRoceRetransTimeout_,
      this,
      qpKey);
}

void
sendflowclient::CancelRoceRetransTimer_(const RoceQpKey& qpKey) {
  auto it = m_roceRetransTimers.find(qpKey);
  if (it == m_roceRetransTimers.end()) {
    return;
  }
  if (it->second.event.IsPending()) {
    it->second.event.Cancel();
  }
  m_roceRetransTimers.erase(it);
}

uint32_t
sendflowclient::GetRoceOutstandingCount_(const RoceQpKey& qpKey) const {
  auto it = m_roceOutstandingPackets.find(qpKey);
  if (it == m_roceOutstandingPackets.end()) {
    return 0;
  }
  return static_cast<uint32_t>(it->second.size());
}

uint32_t
sendflowclient::GetRoceOldestOutstandingPsn_(const RoceQpKey& qpKey) const {
  auto it = m_roceOutstandingPackets.find(qpKey);
  if (it == m_roceOutstandingPackets.end() || it->second.empty()) {
    return 0;
  }
  return it->second.begin()->first;
}

uint32_t
sendflowclient::GetRoceRecoveryTargetPsn_(const RoceQpKey& qpKey) const {
  auto it = m_roceRecoveryTargetPsns.find(qpKey);
  if (it == m_roceRecoveryTargetPsns.end()) {
    return 0;
  }
  return it->second;
}

uint32_t
sendflowclient::GetRoceTransportWindowPackets_() const {
  return std::max<uint32_t>(1, m_roceCfg.transportWindowPackets);
}

std::set<RoceQpKey>
sendflowclient::CollectBlockedRoceQps_() const {
  std::set<RoceQpKey> blocked;
  const uint32_t windowPackets = GetRoceTransportWindowPackets_();
  for (const auto& [qpKey, packets] : m_roceOutstandingPackets) {
    if (packets.size() >= windowPackets) {
      blocked.insert(qpKey);
    }
  }
  for (const auto& [qpKey, _] : m_roceRecoveryTargetPsns) {
    blocked.insert(qpKey);
  }
  return blocked;
}

void
sendflowclient::CleanupFlowRuntimeTables_(const RoceFlowKey& flowKey) {
  m_sendfunctable.erase(flowKey);
  m_recvfunctable.erase(flowKey);
  m_request.erase(flowKey);
  m_recvfuncflag.erase(flowKey);
  m_recvHash.erase(flowKey);
  m_dataPacketsExpected.erase(flowKey);
  m_dataPacketsReceived.erase(flowKey);
  m_ackPacketsExpected.erase(flowKey);
  m_ackPacketsReceived.erase(flowKey);
  m_alldatasize.erase(flowKey);
}

void
sendflowclient::CleanupRoceQpRuntimeState_(const RoceFlowKey& flowKey,
                                           const RoceQpKey* qpHint) {
  RoceQpKey qpKey;
  bool hasQpKey = false;
  if (qpHint != nullptr && qpHint->qpn != 0) {
    qpKey = *qpHint;
    hasQpKey = true;
  } else {
    auto qpnIt = m_roceFlowToQpn.find(flowKey);
    if (qpnIt != m_roceFlowToQpn.end()) {
      qpKey = RoceQpKey{flowKey.src, flowKey.dst, qpnIt->second};
      hasQpKey = true;
    }
  }

  m_roceFlowToQpn.erase(flowKey);
  if (m_roceFsm != nullptr) {
    m_roceFsm->ResetFlow(flowKey);
  }

  if (!hasQpKey) {
    return;
  }

  CancelRoceRetransTimer_(qpKey);
  m_roceOutstandingPackets.erase(qpKey);
  m_roceBufferedUpperPackets.erase(qpKey);
  m_rocePendingRetransmissions.erase(qpKey);
  m_roceRecoveryTargetPsns.erase(qpKey);
  if (m_roceTxScheduler != nullptr) {
    m_roceTxScheduler->DropQp(qpKey);
  }
  if (m_roceQpEngine != nullptr) {
    m_roceQpEngine->EraseQp(qpKey);
  }
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
      IsRoceEnabled() ? GetVisibleRoceWireBytes(state, m_roceCfg, m_roceFsm.get())
                      : 0;
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
    auto flagIt = m_recvfuncflag.find(flowKey);
    if (flagIt != m_recvfuncflag.end() && flagIt->second){
      m_recvfuncflag.erase(flagIt);

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
void
sendflowclient::erase_flowtag(int tag,int src,int dst){
  m_request.erase(
      RoceFlowKey{static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)});
}
AstraSim::sim_request
sendflowclient::get_flowtag(int tag,int src,int dst){
  const RoceFlowKey flowKey{
      static_cast<uint32_t>(tag), static_cast<uint32_t>(src), static_cast<uint32_t>(dst)};
  auto it = m_request.find(flowKey);
  if (it == m_request.end()) {
    NS_LOG_WARN("get_flowtag missing key tag=" << tag
                << " src=" << src
                << " dst=" << dst);
    return AstraSim::sim_request{};
  }
  return it->second;
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
  if (headerOut == nullptr) {
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

  const uint32_t qpn = GetOrAllocRoceQpn_(tag, src, dst);
  const RoceQpKey qpKey{src, dst, qpn};
  EnsureQpReadyToSend(m_roceQpEngine.get(), qpKey);
  if (m_roceQpEngine == nullptr || !m_roceQpEngine->CanPostSend(qpKey)) {
    // 当前实现的 QP 就绪是同步推进到 RTS；走到这里说明状态机内部存在错误。
    NS_FATAL_ERROR("[ROCE][TX] QP not ready after EnsureQpReadyToSend"
                   << " tag=" << tag
                   << " src=" << src
                   << " dst=" << dst
                   << " qpn=" << qpKey.qpn
                   << " state="
                   << static_cast<uint32_t>(
                          m_roceQpEngine == nullptr
                              ? RoceQpState::Err
                              : m_roceQpEngine->GetQpState(qpKey)));
  }

  const RoceOpcode opcode = ToRoceOpcode(state, m_roceCfg, m_roceFsm.get());
  const RoceUserPhase phase = ToRoceUserPhase(state);
  const uint32_t encodedBytes =
      payloadBytes > 0
          ? payloadBytes
          : static_cast<uint32_t>(std::min<uint64_t>(totalDataSize, 0xffffffffULL));
  const bool built = m_roceQpEngine->BuildSendHeader(
      qpKey,
      opcode,
      phase,
      encodedBytes,
      tag,
      headerOut);
  if (!built) {
    NS_LOG_WARN("[ROCE][TX] failed to build RoCE header tag="
                << tag << " src=" << src << " dst=" << dst);
    return false;
  }

  // 为 SEND/WRITE/READ 仿真补齐操作元数据，不改变现有 workload 时序路径。
  const uint64_t remoteAddr =
      (static_cast<uint64_t>(dst) << 32) | static_cast<uint64_t>(tag);
  const uint32_t rkey =
      (tag * 2654435761u) ^ (src << 16) ^ (dst & 0xffffu);
  uint8_t flags = 0;
  if (opcode == RoceOpcode::RdmaWrite || opcode == RoceOpcode::RdmaReadReq ||
      opcode == RoceOpcode::RdmaReadResp) {
    flags |= 0x1; // Remote memory semantic.
  }
  if (opcode == RoceOpcode::RdmaReadResp) {
    flags |= 0x2; // Read response.
  }
  if (opcode == RoceOpcode::Ack) {
    flags |= 0x4; // Ack semantic.
  }
  headerOut->SetFlags(flags);
  headerOut->SetRemoteAddr(remoteAddr);
  headerOut->SetRkey(rkey);
  headerOut->SetImmData(static_cast<uint32_t>(std::min<uint64_t>(totalDataSize, 0xffffffffULL)));
  if (opcode == RoceOpcode::RdmaReadReq) {
    headerOut->SetLength(static_cast<uint32_t>(std::min<uint64_t>(totalDataSize, 0xffffffffULL)));
  }

  if (m_roceFsm != nullptr) {
    RoceFlowKey flowKey{tag, src, dst};
    if (state == ThreePHeader::REQUEST) {
      m_roceFsm->OnRequestIssued(flowKey);
    } else if (state == ThreePHeader::DATA) {
      m_roceFsm->OnDataIssued(flowKey);
    }
  }
  return true;
}

bool
sendflowclient::HandleRoceRxHeader_(const RoceSimHeader& roceHeader,
                                    ThreePHeader::State state,
                                    uint32_t tag,
                                    uint32_t src,
                                    uint32_t dst,
                                    const Address& from,
                                    uint32_t rxInDevIndexForLoopback,
                                    bool* bufferedForReorder,
                                    bool* overflowedReorderWindow) {
  if (!IsRoceEnabled()) {
    return true;
  }

  const RoceFlowKey flowKey{tag, src, dst};
  RememberRoceQpn_(tag, src, dst, roceHeader.GetQpn());
  const RoceQpKey qpKey{src, dst, roceHeader.GetQpn()};
  if (roceHeader.GetOpcode() == RoceOpcode::Ack) {
    if (bufferedForReorder != nullptr) {
      *bufferedForReorder = false;
    }
    if (overflowedReorderWindow != nullptr) {
      *overflowedReorderWindow = false;
    }
    HandleRoceAck_(flowKey, qpKey, roceHeader);
    return false;
  }
  EnsureQpReadyToSend(m_roceQpEngine.get(), qpKey);

  bool qpAccepted = true;
  bool shouldDeliverUpper = true;
  bool qpBufferedForReorder = false;
  bool qpOverflowedReorderWindow = false;
  RoceCompletion cqe;
  if (m_roceQpEngine != nullptr) {
    // 由 QP 引擎校验 PSN/状态等接收侧条件。
    qpAccepted = m_roceQpEngine->OnRxHeader(qpKey,
                                            roceHeader,
                                            &cqe,
                                            &shouldDeliverUpper,
                                            &qpBufferedForReorder,
                                            &qpOverflowedReorderWindow);
    if (!qpAccepted) {
      NS_LOG_WARN("[ROCE][RX] QP rejected packet tag=" << tag
                  << " src=" << src
                  << " dst=" << dst
                  << " qpn=" << roceHeader.GetQpn()
                  << " psn=" << roceHeader.GetPsn());
      return false;
    }
  }
  if (bufferedForReorder != nullptr) {
    *bufferedForReorder = qpBufferedForReorder;
  }
  if (overflowedReorderWindow != nullptr) {
    *overflowedReorderWindow = qpOverflowedReorderWindow;
  }

  if (qpOverflowedReorderWindow) {
    NS_LOG_WARN("[ROCE][RX][REORDER_OVERFLOW] tag=" << tag
                << " node=" << GetNode()->GetId()
                << " src=" << src
                << " dst=" << dst
                << " qpn=" << roceHeader.GetQpn()
                << " psn=" << roceHeader.GetPsn()
                << " expectedRxPsn=" << m_roceQpEngine->GetExpectedRxPsn(qpKey)
                << " pendingRxCount=" << m_roceQpEngine->GetPendingRxCount(qpKey)
                << " reorderWindowPackets=" << m_roceCfg.reorderWindowPackets);
  }

  if (qpAccepted) {
    MaybeSendRoceAck_(from,
                      tag,
                      src,
                      dst,
                      roceHeader.GetQpn(),
                      rxInDevIndexForLoopback,
                      roceHeader.GetOpcode());
  }

  if (!shouldDeliverUpper) {
    return false;
  }

  if (roceHeader.GetUserPhase() != ToRoceUserPhase(state)) {
    NS_LOG_WARN("[ROCE][RX] phase mismatch tag=" << tag
                << " expected=" << static_cast<uint32_t>(ToRoceUserPhase(state))
                << " got=" << static_cast<uint32_t>(roceHeader.GetUserPhase()));
  }
  const RoceOpcode expectedOpcode = ToRoceOpcode(state, m_roceCfg, m_roceFsm.get());
  if (roceHeader.GetOpcode() != expectedOpcode) {
    NS_LOG_WARN("[ROCE][RX] opcode mismatch tag=" << tag
                << " expected=" << static_cast<uint32_t>(expectedOpcode)
                << " got=" << static_cast<uint32_t>(roceHeader.GetOpcode()));
  }

  if (state == ThreePHeader::ACT && qpAccepted) {
    // ACT 仅作为 3P 业务控制报文存在，不再承担可靠传输 completion 语义。
    if (m_roceFsm != nullptr) {
      m_roceFsm->OnActReceived(flowKey);
    }
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
  if (m_roceQpEngine) {
    m_roceQpEngine->Configure(m_roceCfg);
    m_roceQpEngine->Reset();
  }
  if (m_roceFsm) {
    m_roceFsm->Configure(m_roceCfg);
    m_roceFsm->Reset();
  }
  if (m_roceTxScheduler) {
    m_roceTxScheduler->Configure(m_roceCfg);
  }
  for (auto& [_, timerState] : m_roceRetransTimers) {
    if (timerState.event.IsPending()) {
      timerState.event.Cancel();
    }
  }
  m_roceOutstandingPackets.clear();
  m_roceRetransTimers.clear();
  m_roceBufferedUpperPackets.clear();
  m_rocePendingRetransmissions.clear();
  m_roceRecoveryTargetPsns.clear();
  m_roceFlowToQpn.clear();
  m_nextSyntheticQpn = 1;
  m_nextRoceWorkId = 1;
  m_roceDispatchEvent.Cancel();
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
                << " reqOp=" << static_cast<uint32_t>(ToRoceOpcode(ThreePHeader::REQUEST, m_roceCfg, m_roceFsm.get()))
                << " dataOp=" << static_cast<uint32_t>(ToRoceOpcode(ThreePHeader::DATA, m_roceCfg, m_roceFsm.get()))
                << " actOp=" << static_cast<uint32_t>(ToRoceOpcode(ThreePHeader::ACT, m_roceCfg, m_roceFsm.get())));
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

sendflowclient::RuntimeTableStats
sendflowclient::GetRuntimeTableStats() const {
  RuntimeTableStats stats;
  stats.sendFuncEntries = m_sendfunctable.size();
  stats.recvFuncEntries = m_recvfunctable.size();
  stats.requestEntries = m_request.size();
  stats.recvFuncFlagEntries = m_recvfuncflag.size();
  stats.recvHashEntries = m_recvHash.size();
  stats.dataExpectedEntries = m_dataPacketsExpected.size();
  stats.dataReceivedEntries = m_dataPacketsReceived.size();
  stats.ackExpectedEntries = m_ackPacketsExpected.size();
  stats.ackReceivedEntries = m_ackPacketsReceived.size();
  stats.allDataSizeEntries = m_alldatasize.size();
  stats.roceFlowToQpnEntries = m_roceFlowToQpn.size();
  stats.roceOutstandingQps = m_roceOutstandingPackets.size();
  for (const auto& [_, packets] : m_roceOutstandingPackets) {
    stats.roceOutstandingPackets += packets.size();
  }
  stats.roceRetransTimerQps = m_roceRetransTimers.size();
  stats.roceBufferedQps = m_roceBufferedUpperPackets.size();
  for (const auto& [_, buffered] : m_roceBufferedUpperPackets) {
    stats.roceBufferedPackets += buffered.size();
  }
  stats.rocePendingRetransQps = m_rocePendingRetransmissions.size();
  stats.roceRecoveryTargetQps = m_roceRecoveryTargetPsns.size();
  if (m_roceTxScheduler != nullptr) {
    stats.rocePendingWorkItems = m_roceTxScheduler->GetPendingWorkCount();
  }
  stats.totalTrackedEntries =
      stats.sendFuncEntries + stats.recvFuncEntries + stats.requestEntries +
      stats.recvFuncFlagEntries + stats.recvHashEntries +
      stats.dataExpectedEntries + stats.dataReceivedEntries +
      stats.ackExpectedEntries + stats.ackReceivedEntries +
      stats.allDataSizeEntries + stats.roceFlowToQpnEntries +
      stats.roceOutstandingPackets + stats.roceRetransTimerQps +
      stats.roceBufferedPackets + stats.rocePendingRetransQps +
      stats.roceRecoveryTargetQps + stats.rocePendingWorkItems;
  return stats;
}

uint32_t
sendflowclient::GetOrAllocRoceQpn_(uint32_t tag, uint32_t src, uint32_t dst) {
  const RoceFlowKey flowKey{tag, src, dst};
  auto it = m_roceFlowToQpn.find(flowKey);
  if (it != m_roceFlowToQpn.end()) {
    return it->second;
  }

  // 运行时按 flow 三元组稳定分配 synthetic QPN，避免直接用 tag 造成复用冲突。
  uint32_t qpn = m_nextSyntheticQpn++;
  if (qpn == 0) {
    qpn = m_nextSyntheticQpn++;
  }
  m_roceFlowToQpn[flowKey] = qpn;
  return qpn;
}

void
sendflowclient::RememberRoceQpn_(uint32_t tag,
                                 uint32_t src,
                                 uint32_t dst,
                                 uint32_t qpn) {
  if (qpn == 0) {
    return;
  }

  const RoceFlowKey flowKey{tag, src, dst};
  auto it = m_roceFlowToQpn.find(flowKey);
  if (it != m_roceFlowToQpn.end() && it->second != qpn) {
    NS_LOG_WARN("[ROCE][QPN] flow remap detected tag=" << tag
                << " src=" << src
                << " dst=" << dst
                << " oldQpn=" << it->second
                << " newQpn=" << qpn);
  }
  m_roceFlowToQpn[flowKey] = qpn;
  if (m_nextSyntheticQpn <= qpn) {
    m_nextSyntheticQpn = qpn + 1;
  }
}


}
