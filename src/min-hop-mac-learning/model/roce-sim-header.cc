#include "roce-sim-header.h"

#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RoceSimHeader");
NS_OBJECT_ENSURE_REGISTERED(RoceSimHeader);

TypeId
RoceSimHeader::GetTypeId() {
  static TypeId tid =
      TypeId("ns3::RoceSimHeader")
          .SetParent<Header>()
          .SetGroupName("Network")
          .AddConstructor<RoceSimHeader>();
  return tid;
}

TypeId
RoceSimHeader::GetInstanceTypeId() const {
  return GetTypeId();
}

uint32_t
RoceSimHeader::GetSerializedSize() const {
  // 固定长度编码：
  // 4B(version/opcode/phase/flags)
  // + 9 * 4B(qpn/psn/ack/imm/rkey/len/tag/src/dst)
  // + 8B(remoteAddr)
  return 4 + (4 * 9) + 8;
}

void
RoceSimHeader::Serialize(Buffer::Iterator start) const {
  // 按固定顺序写入，Deserialize 必须保持同顺序读取。
  start.WriteU8(m_version);
  start.WriteU8(m_opcode);
  start.WriteU8(m_userPhase);
  start.WriteU8(m_flags);

  start.WriteHtonU32(m_qpn);
  start.WriteHtonU32(m_psn);
  start.WriteHtonU32(m_ackPsn);
  start.WriteHtonU32(m_immData);

  start.WriteHtonU64(m_remoteAddr);
  start.WriteHtonU32(m_rkey);
  start.WriteHtonU32(m_length);

  start.WriteHtonU32(m_tag);
  start.WriteHtonU32(m_src);
  start.WriteHtonU32(m_dst);
}

uint32_t
RoceSimHeader::Deserialize(Buffer::Iterator start) {
  // 解析顺序与 Serialize 一一对应。
  m_version = start.ReadU8();
  m_opcode = start.ReadU8();
  m_userPhase = start.ReadU8();
  m_flags = start.ReadU8();

  m_qpn = start.ReadNtohU32();
  m_psn = start.ReadNtohU32();
  m_ackPsn = start.ReadNtohU32();
  m_immData = start.ReadNtohU32();

  m_remoteAddr = start.ReadNtohU64();
  m_rkey = start.ReadNtohU32();
  m_length = start.ReadNtohU32();

  m_tag = start.ReadNtohU32();
  m_src = start.ReadNtohU32();
  m_dst = start.ReadNtohU32();

  return GetSerializedSize();
}

void
RoceSimHeader::Print(std::ostream& os) const {
  os << "ver=" << static_cast<uint32_t>(m_version)
     << " opcode=" << static_cast<uint32_t>(m_opcode)
     << " phase=" << static_cast<uint32_t>(m_userPhase)
     << " qpn=" << m_qpn
     << " psn=" << m_psn
     << " ack_psn=" << m_ackPsn
     << " len=" << m_length
     << " tag=" << m_tag
     << " src=" << m_src
     << " dst=" << m_dst;
}

void
RoceSimHeader::SetVersion(uint8_t version) {
  m_version = version;
}

uint8_t
RoceSimHeader::GetVersion() const {
  return m_version;
}

void
RoceSimHeader::SetOpcode(RoceOpcode opcode) {
  m_opcode = static_cast<uint8_t>(opcode);
}

RoceOpcode
RoceSimHeader::GetOpcode() const {
  return static_cast<RoceOpcode>(m_opcode);
}

void
RoceSimHeader::SetUserPhase(RoceUserPhase phase) {
  m_userPhase = static_cast<uint8_t>(phase);
}

RoceUserPhase
RoceSimHeader::GetUserPhase() const {
  return static_cast<RoceUserPhase>(m_userPhase);
}

void
RoceSimHeader::SetFlags(uint8_t flags) {
  m_flags = flags;
}

uint8_t
RoceSimHeader::GetFlags() const {
  return m_flags;
}

void
RoceSimHeader::SetQpn(uint32_t qpn) {
  m_qpn = qpn;
}

uint32_t
RoceSimHeader::GetQpn() const {
  return m_qpn;
}

void
RoceSimHeader::SetPsn(uint32_t psn) {
  m_psn = psn;
}

uint32_t
RoceSimHeader::GetPsn() const {
  return m_psn;
}

void
RoceSimHeader::SetAckPsn(uint32_t ackPsn) {
  m_ackPsn = ackPsn;
}

uint32_t
RoceSimHeader::GetAckPsn() const {
  return m_ackPsn;
}

void
RoceSimHeader::SetImmData(uint32_t immData) {
  m_immData = immData;
}

uint32_t
RoceSimHeader::GetImmData() const {
  return m_immData;
}

void
RoceSimHeader::SetRemoteAddr(uint64_t remoteAddr) {
  m_remoteAddr = remoteAddr;
}

uint64_t
RoceSimHeader::GetRemoteAddr() const {
  return m_remoteAddr;
}

void
RoceSimHeader::SetRkey(uint32_t rkey) {
  m_rkey = rkey;
}

uint32_t
RoceSimHeader::GetRkey() const {
  return m_rkey;
}

void
RoceSimHeader::SetLength(uint32_t length) {
  m_length = length;
}

uint32_t
RoceSimHeader::GetLength() const {
  return m_length;
}

void
RoceSimHeader::SetTag(uint32_t tag) {
  m_tag = tag;
}

uint32_t
RoceSimHeader::GetTag() const {
  return m_tag;
}

void
RoceSimHeader::SetSrc(uint32_t src) {
  m_src = src;
}

uint32_t
RoceSimHeader::GetSrc() const {
  return m_src;
}

void
RoceSimHeader::SetDst(uint32_t dst) {
  m_dst = dst;
}

uint32_t
RoceSimHeader::GetDst() const {
  return m_dst;
}

} // namespace ns3
