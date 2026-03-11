#include "my_sendflowclient.h"
#include "ns3/udp-socket-factory.h"
#include "ns3/inet-socket-address.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "roce-qp-engine.h"
#include "roce-sim-header.h"
#include "roce-three-phase-fsm.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <chrono>


namespace ns3 {

NS_LOG_COMPONENT_DEFINE("SendFlowClient");

namespace {

uint32_t
MakeSyntheticQpnFromTag(uint32_t tag) {
  // Keep qpn stable per flow tag in the simulation path.
  return tag;
}

std::string
ToLowerCopy(std::string v) {
  for (char& c : v) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return v;
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
  m_roceQpEngine->Configure(m_roceCfg);
  m_roceFsm->Configure(m_roceCfg);
}
sendflowclient::~sendflowclient()
{
}

void sendflowclient::StartApplication()
{

}

void sendflowclient::StopApplication()
{
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

    RoceSimHeader roceHeader;
    bool hasRoceHeader = false;
    if (IsRoceEnabled()) {
      // 先用 probe 包试探 RoCE 头；只有 tag/src/dst 匹配才真正消费头部。
      Ptr<Packet> probe = p->Copy();
      if (probe->GetSize() >= roceHeader.GetSerializedSize()) {
        probe->RemoveHeader(roceHeader);
        if (roceHeader.GetTag() == tag &&
            roceHeader.GetSrc() == src &&
            roceHeader.GetDst() == dst) {
          p->RemoveHeader(roceHeader);
          hasRoceHeader = true;
        } else {
          NS_LOG_WARN("[ROCE][RX] missing/invalid RoCE header, fallback legacy parse"
                      << " tag=" << tag << " src=" << src << " dst=" << dst);
        }
      }
    }
    if (hasRoceHeader) {
      HandleRoceRxHeader_(roceHeader, TPheader.GetState(), tag, src, dst);
    }



    Address local;
    socket->GetSockName(local);
    PacketSocketAddress laddr = PacketSocketAddress::ConvertFrom(local);
    uint32_t formIndex = laddr.GetSingleDevice(); //拿到来源的netdevice编号

    // NS_LOG_DEBUG("[" << node->timeGetId() << "] Received packet for flow " << flowid
    //                  << " from " << from
    //                  << " with state=" << (int)TPheader.GetState()
    //                  << ", header datasize=" << datasize
    //                  << ", actual packet size=" << packet->GetSize());

    switch (TPheader.GetState())
    {
    case ThreePHeader::REQUEST:
    {
      if(check_recvfunc(tag,src,dst) == false){
        m_recvfuncflag[{tag,{src,dst}}] = 1;
        return ;
      }
      fuc_and_par fp = m_recvfunctable[{tag,{src,dst}}];
      fp.msg_handler(fp.fun_arg); 
      m_recvfunctable.erase({tag,{src,dst}});
    }
    break;

    case ThreePHeader::DATA:
    {
      p->RemoveHeader(FTHeader);
      AstraSim::sim_request now_request;
      FTHeader.ToFlowTag(now_request.flowTag);
      uint32_t PacketNum=FTHeader.get_packetnum();
      if (m_request.find({tag,{src,dst}})==m_request.end()) m_request[{tag,{src,dst}}]=now_request;
      if (m_dataPacketsExpected.find({tag,{src,dst}})==m_dataPacketsExpected.end()){
        m_dataPacketsExpected[{tag,{src,dst}}]=PacketNum;
        m_alldatasize[{tag,{src,dst}}]=TPheader.GetDataSize();
      } 

      // 逻辑1 (计数): (接收方) 收到 DATA
      if (m_dataPacketsExpected.count({tag,{src,dst}})!=0) {
          m_dataPacketsReceived[{tag,{src,dst}}]++;
          // realsend(packet,from,4,formIndex);
          // SendPacket(ThreePHeader::ACT,from,acklen,tag,src,dst,4,formIndex);  
          // 检查是否是最后一个 DATA 包
          if (m_dataPacketsReceived[{tag,{src,dst}}] == m_dataPacketsExpected[{tag,{src,dst}}]) {
              // NS_LOG_INFO("Flow " << flowid << ": (Receiver) LAST DATA packet received (" << m_dataPacketsReceived[flowid] << "/" << m_dataPacketsExpected[flowid] << ").");
              // --- 填充recvHash ---
              Address ackDest = from;                          // 目的地址
              uint64_t acksize = acklen;
              uint32_t ackTag  = tag;
              uint32_t ackSrc  = src;
              uint32_t ackDst  = dst;
              int      ackIndex     = formIndex;
              ns3::Simulator::ScheduleWithContext((this->GetNode()->GetId()), Seconds(0), [this,ackDest,acksize,ackTag,ackSrc,ackDst,ackIndex](){
                  SendPacket(ThreePHeader::ACT,ackDest,acksize,ackTag,ackSrc,ackDst,FlowSendMode::Loopback,ackIndex); });              


              m_recvHash[{tag,{src,dst}}]=m_alldatasize[{tag,{src,dst}}];
              if (check_recvfunc(tag,src,dst)){
                fuc_and_par fp = m_recvfunctable[{tag,{src,dst}}];

                AstraSim::RecvPacketEventHadndlerData* ehd = (AstraSim::RecvPacketEventHadndlerData*) fp.fun_arg;
                ehd->flowTag=m_request[{tag,{src,dst}}].flowTag;

                fp.msg_handler(fp.fun_arg); 
                m_recvHash.erase({tag,{src,dst}});
                m_recvfunctable.erase({tag,{src,dst}});
                m_request.erase({tag,{src,dst}});
                // NS_LOG_INFO("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst 
                //   << " 接收完成; TIME= "<<Simulator::Now().GetFemtoSeconds());
              }
              // NS_LOG_INFO("flowid= " << flowid << " 接收完成 ");
              
              // 清理map
              m_dataPacketsExpected.erase({tag,{src,dst}});
              m_dataPacketsReceived.erase({tag,{src,dst}});
          }
      } else {
          NS_LOG_WARN("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst << ": Received DATA packet without expected count setup.");
      }

    }
    break;
    case ThreePHeader::ACT:
    {
      // 逻辑2 (计数): (发送方) 收到 ACT 控制报文
      if (m_ackPacketsExpected.find({tag,{src,dst}}) != m_ackPacketsExpected.end()) {
          m_ackPacketsReceived[{tag,{src,dst}}]++;
            
          // 检查是否是最后一个 ACT 包
          if (m_ackPacketsReceived[{tag,{src,dst}}] == m_ackPacketsExpected[{tag,{src,dst}}]) {
              // NS_LOG_INFO("Flow " << flowid << ": (Sender) LAST ACT packet received (" << m_ackPacketsReceived[flowid] << "/" << m_ackPacketsExpected[flowid] << ").");
              // (未来引用notify_send回调函数的地方)
              if (m_sendfunctable.find({tag,{src,dst}}) != m_sendfunctable.end()) {
                  fuc_and_par fp = m_sendfunctable[{tag,{src,dst}}];
                  fp.msg_handler(fp.fun_arg); 
                  m_sendfunctable.erase({tag,{src,dst}});
                  // NS_LOG_INFO("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst 
                  //   << " 发送完成; TIME= "<<Simulator::Now().GetFemtoSeconds());
              } else {
                  NS_LOG_WARN("{tag,src,dst}= " << tag<<"  "<<src<<" "<<dst <<": (Sender) Callback not found for flowid.");
              }
                
              // 清理map
              m_ackPacketsExpected.erase({tag,{src,dst}});
              m_ackPacketsReceived.erase({tag,{src,dst}});
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
}

void
sendflowclient::TriggerSendTo_now(PacketSocketAddress dstadd,uint64_t datasize,uint32_t tag,uint32_t src,uint32_t dst,FlowSendMode sendmodel)
{
  NS_LOG_INFO("TriggerSendTo_now declared now time = "  << Simulator::Now().GetFemtoSeconds()<<" src="<<src<<" dst="<<dst<<" tag="<<tag<<" size="<<datasize);
  if(datasize == 1023)
  {
    ns3::Simulator::ScheduleWithContext((this->GetNode()->GetId()), Seconds(0), [this,dstadd,datasize,tag,src,dst,sendmodel](){
      SendPacket(ThreePHeader::REQUEST, dstadd, datasize, tag,src,dst,sendmodel);});
  }
  else
  {
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
  // uint16_t m_mtu=device->GetMtu()-TPheader.GetSerializedSize()-hopHeader.GetSerializedSize();
  uint16_t m_mtu=device->GetMtu();
  RoceSimHeader roceWireHeader;
  // 开启 RoCE 时，payload 预算需要扣除 RoCE 头，避免 MTU 计算偏大导致流程卡住。
  const uint16_t roceWireBytes =
      IsRoceEnabled() ? static_cast<uint16_t>(roceWireHeader.GetSerializedSize())
                      : 0;
  auto SafePayloadBytes = [](int64_t rawBytes) -> uint16_t {
    return rawBytes > 0 ? static_cast<uint16_t>(rawBytes) : 0;
  };
 
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

      fuc_and_par fp = m_sendfunctable[{tag,{src,dst}}];
      fp.msg_handler(fp.fun_arg);
      m_sendfunctable.erase({tag,{src,dst}});

      // m_alldatasize[{tag,{src,dst}}]=datasize;
  }
  
  if (state==ThreePHeader::DATA){
    uint64_t numPackets;
    if (datasize<m_mtu) numPackets=1;
    else {
      if (datasize % m_mtu==0) numPackets=datasize / m_mtu;
      else numPackets = (datasize / m_mtu) + 1;
    }
    // m_ackPacketsExpected[{tag,{src,dst}}] = numPackets;
    // 发送前记录act
    m_ackPacketsExpected[{tag,{src,dst}}] = 1;
    m_ackPacketsReceived[{tag,{src,dst}}] = 0; // 初始化 ACT 计数器
    FTHeader.FromFlowTag(get_flowtag(tag,src,dst).flowTag);
    FTHeader.set_packetnum(numPackets);
    TPheader.SetDataSize(datasize);

    uint64_t len = datasize;
    for (uint64_t i = 0; i < numPackets; ++i) {
        uint16_t real_packet_size = 0;
        if (len >= m_mtu) {
          real_packet_size = SafePayloadBytes(
              static_cast<int64_t>(m_mtu) - TPheader.GetSerializedSize() -
              hopHeader.GetSerializedSize() - FTHeader.GetSerializedSize() -
              roceWireBytes);
          packet = Create<Packet>(real_packet_size);
          
        } 
        else {
          real_packet_size = SafePayloadBytes(
              static_cast<int64_t>(len) - TPheader.GetSerializedSize() -
              hopHeader.GetSerializedSize() - FTHeader.GetSerializedSize() -
              roceWireBytes);
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
      
        len = (len > m_mtu) ? (len - m_mtu) : 0;
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
    m_sendfunctable[{tag,{src,dst}}]=fp; 
}

void
sendflowclient::set_recvfunc(int tag,int src,int dst,fuc_and_par fp){
    if(m_recvfuncflag[{tag,{src,dst}}] == 1){
      m_recvfuncflag[{tag,{src,dst}}] = 0;

      // NS_LOG_INFO("[RZV] CALLBACK fired tag="<<tag<<" src="<<src<<" dst="<<dst);
      
      fp.msg_handler(fp.fun_arg);
      return ;
    }
    m_recvfunctable[{tag,{src,dst}}]=fp; 
}

bool
sendflowclient::check_recvfunc(int tag,int src,int dst){
    if (m_recvfunctable.find({tag,{src,dst}})!=m_recvfunctable.end()) return true;
    else return false;
}
void
sendflowclient::set_recvhash(int tag,int src,int dst,uint64_t countnum){
    m_recvHash[{tag,{src,dst}}]=countnum;
}
void
sendflowclient::erase_recvhash(int tag,int src,int dst){
    m_recvHash.erase({tag,{src,dst}});
}
bool
sendflowclient::check_recvhash(int tag,int src,int dst){
    if (m_recvHash.find({tag,{src,dst}})!=m_recvHash.end()) return true;
    else return false;
}
uint64_t
sendflowclient::get_recvhash(int tag,int src,int dst){
    return m_recvHash[{tag,{src,dst}}];
}
void
sendflowclient::write_flowtag(int tag,int src,int dst,AstraSim::sim_request request){
    m_request[{tag,{src,dst}}]=request;
}
AstraSim::sim_request
sendflowclient::get_flowtag(int tag,int src,int dst){
  return m_request[{tag,{src,dst}}];
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
    NS_LOG_INFO("[ROCE][ACTIVE] node=" << GetNode()->GetId()
                << " mode=" << m_roceCfg.modeName
                << " dataUseWrite=" << m_roceCfg.dataUseWrite
                << " sqDepth=" << m_roceCfg.defaultSqDepth
                << " rqDepth=" << m_roceCfg.defaultRqDepth
                << " etherType=0x" << std::hex << m_roceCfg.etherType << std::dec);
    m_roceActiveLogged = true;
  }

  const RoceQpKey qpKey{src, dst, MakeSyntheticQpnFromTag(tag)};
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

void
sendflowclient::HandleRoceRxHeader_(const RoceSimHeader& roceHeader,
                                    ThreePHeader::State state,
                                    uint32_t tag,
                                    uint32_t src,
                                    uint32_t dst) {
  if (!IsRoceEnabled()) {
    return;
  }

  const RoceQpKey qpKey{src, dst, roceHeader.GetQpn()};
  EnsureQpReadyToSend(m_roceQpEngine.get(), qpKey);

  bool qpAccepted = true;
  RoceCompletion cqe;
  if (m_roceQpEngine != nullptr) {
    // 由 QP 引擎校验 PSN/状态等接收侧条件。
    qpAccepted = m_roceQpEngine->OnRxHeader(qpKey, roceHeader, &cqe);
    if (!qpAccepted) {
      NS_LOG_WARN("[ROCE][RX] QP rejected packet tag=" << tag
                  << " src=" << src
                  << " dst=" << dst
                  << " qpn=" << roceHeader.GetQpn()
                  << " psn=" << roceHeader.GetPsn());
    }
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
      m_roceFsm->OnActReceived(RoceFlowKey{tag, src, dst});
    }
  }
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
  m_roceActiveLogged = false;
  NS_LOG_INFO("[ROCE][CFG] enable=" << m_roceCfg.enable
              << " mode=" << m_roceCfg.modeName
              << " dataUseWrite=" << m_roceCfg.dataUseWrite
              << " etherType=0x" << std::hex << m_roceCfg.etherType << std::dec
              << " reqOp=" << static_cast<uint32_t>(ToRoceOpcode(ThreePHeader::REQUEST, m_roceCfg, m_roceFsm.get()))
              << " dataOp=" << static_cast<uint32_t>(ToRoceOpcode(ThreePHeader::DATA, m_roceCfg, m_roceFsm.get()))
              << " actOp=" << static_cast<uint32_t>(ToRoceOpcode(ThreePHeader::ACT, m_roceCfg, m_roceFsm.get())));
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


}
