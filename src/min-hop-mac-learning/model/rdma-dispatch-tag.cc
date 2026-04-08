#include "rdma-dispatch-tag.h"

#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RdmaDispatchTag");
NS_OBJECT_ENSURE_REGISTERED(RdmaDispatchTag);

TypeId
RdmaDispatchTag::GetTypeId() {
  static TypeId tid =
      TypeId("ns3::RdmaDispatchTag").SetParent<Tag>().AddConstructor<RdmaDispatchTag>();
  return tid;
}

TypeId
RdmaDispatchTag::GetInstanceTypeId() const {
  return GetTypeId();
}

uint32_t
RdmaDispatchTag::GetSerializedSize() const {
  return 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 1 + 1 + 1 + 1;
}

void
RdmaDispatchTag::Serialize(TagBuffer buffer) const {
  buffer.WriteU64(m_workId);
  buffer.WriteU64(m_totalBytes);
  buffer.WriteU64(m_packetIndex);
  buffer.WriteU64(m_totalPackets);
  buffer.WriteU32(m_tag);
  buffer.WriteU32(m_src);
  buffer.WriteU32(m_dst);
  buffer.WriteU32(m_qpn);
  buffer.WriteU32(m_psn);
  buffer.WriteU32(m_sliceBytes);
  buffer.WriteU32(m_queueId);
  buffer.WriteU8(m_state);
  buffer.WriteU8(m_opcode);
  buffer.WriteU8(m_txClass);
  buffer.WriteU8(m_flags);
}

void
RdmaDispatchTag::Deserialize(TagBuffer buffer) {
  m_workId = buffer.ReadU64();
  m_totalBytes = buffer.ReadU64();
  m_packetIndex = buffer.ReadU64();
  m_totalPackets = buffer.ReadU64();
  m_tag = buffer.ReadU32();
  m_src = buffer.ReadU32();
  m_dst = buffer.ReadU32();
  m_qpn = buffer.ReadU32();
  m_psn = buffer.ReadU32();
  m_sliceBytes = buffer.ReadU32();
  m_queueId = buffer.ReadU32();
  m_state = buffer.ReadU8();
  m_opcode = buffer.ReadU8();
  m_txClass = buffer.ReadU8();
  m_flags = buffer.ReadU8();
}

void
RdmaDispatchTag::Print(std::ostream& os) const {
  os << "workId=" << m_workId << " tag=" << m_tag << " src=" << m_src
     << " dst=" << m_dst << " qpn=" << m_qpn << " psn=" << m_psn
     << " state=" << static_cast<uint32_t>(m_state)
     << " txClass=" << static_cast<uint32_t>(m_txClass)
     << " queueId=" << m_queueId << " flags=" << static_cast<uint32_t>(m_flags);
}

void
RdmaDispatchTag::SetWorkId(uint64_t workId) {
  m_workId = workId;
}

uint64_t
RdmaDispatchTag::GetWorkId() const {
  return m_workId;
}

void
RdmaDispatchTag::SetFlowKey(const RoceFlowKey& flowKey) {
  m_tag = flowKey.tag;
  m_src = flowKey.src;
  m_dst = flowKey.dst;
}

RoceFlowKey
RdmaDispatchTag::GetFlowKey() const {
  return RoceFlowKey{m_tag, m_src, m_dst};
}

void
RdmaDispatchTag::SetState(ThreePHeader::State state) {
  m_state = static_cast<uint8_t>(state);
}

ThreePHeader::State
RdmaDispatchTag::GetState() const {
  return static_cast<ThreePHeader::State>(m_state);
}

void
RdmaDispatchTag::SetQpn(uint32_t qpn) {
  m_qpn = qpn;
}

uint32_t
RdmaDispatchTag::GetQpn() const {
  return m_qpn;
}

void
RdmaDispatchTag::SetPsn(uint32_t psn) {
  m_psn = psn;
}

uint32_t
RdmaDispatchTag::GetPsn() const {
  return m_psn;
}

void
RdmaDispatchTag::SetOpcode(RoceOpcode opcode) {
  m_opcode = static_cast<uint8_t>(opcode);
}

RoceOpcode
RdmaDispatchTag::GetOpcode() const {
  return static_cast<RoceOpcode>(m_opcode);
}

void
RdmaDispatchTag::SetTxClass(RoceTxClass txClass) {
  m_txClass = static_cast<uint8_t>(txClass);
}

RoceTxClass
RdmaDispatchTag::GetTxClass() const {
  return static_cast<RoceTxClass>(m_txClass);
}

void
RdmaDispatchTag::SetTotalBytes(uint64_t totalBytes) {
  m_totalBytes = totalBytes;
}

uint64_t
RdmaDispatchTag::GetTotalBytes() const {
  return m_totalBytes;
}

void
RdmaDispatchTag::SetSliceBytes(uint32_t sliceBytes) {
  m_sliceBytes = sliceBytes;
}

uint32_t
RdmaDispatchTag::GetSliceBytes() const {
  return m_sliceBytes;
}

void
RdmaDispatchTag::SetPacketIndex(uint64_t packetIndex) {
  m_packetIndex = packetIndex;
}

uint64_t
RdmaDispatchTag::GetPacketIndex() const {
  return m_packetIndex;
}

void
RdmaDispatchTag::SetTotalPackets(uint64_t totalPackets) {
  m_totalPackets = totalPackets;
}

uint64_t
RdmaDispatchTag::GetTotalPackets() const {
  return m_totalPackets;
}

void
RdmaDispatchTag::SetQueueId(uint32_t queueId) {
  m_queueId = queueId;
}

uint32_t
RdmaDispatchTag::GetQueueId() const {
  return m_queueId;
}

void
RdmaDispatchTag::SetLastSlice(bool isLastSlice) {
  SetFlag_(FLAG_LAST_SLICE, isLastSlice);
}

bool
RdmaDispatchTag::IsLastSlice() const {
  return GetFlag_(FLAG_LAST_SLICE);
}

void
RdmaDispatchTag::SetRetransmission(bool isRetransmission) {
  SetFlag_(FLAG_RETRANSMISSION, isRetransmission);
}

bool
RdmaDispatchTag::IsRetransmission() const {
  return GetFlag_(FLAG_RETRANSMISSION);
}

void
RdmaDispatchTag::SetTransportAck(bool isTransportAck) {
  SetFlag_(FLAG_TRANSPORT_ACK, isTransportAck);
}

bool
RdmaDispatchTag::IsTransportAck() const {
  return GetFlag_(FLAG_TRANSPORT_ACK);
}

void
RdmaDispatchTag::SetFlag_(Flags flag, bool enabled) {
  if (enabled) {
    m_flags |= static_cast<uint8_t>(flag);
  } else {
    m_flags &= ~static_cast<uint8_t>(flag);
  }
}

bool
RdmaDispatchTag::GetFlag_(Flags flag) const {
  return (m_flags & static_cast<uint8_t>(flag)) != 0;
}

} // namespace ns3
