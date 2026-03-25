#ifndef ROCE_SIM_HEADER_H
#define ROCE_SIM_HEADER_H

#include "ns3/header.h"
#include "ns3/type-id.h"
#include "roce-sim-types.h"

namespace ns3 {

// 仿真用 RoCE 扩展头：
// 挂载在 3P 头之后，用于携带 opcode/phase/qpn/psn 以及 RDMA 元数据。
class RoceSimHeader : public Header {
public:
  static TypeId GetTypeId();
  static uint32_t GetVisibleWireBytes(RoceOpcode opcode);
  TypeId GetInstanceTypeId() const override;

  RoceSimHeader() = default;

  uint32_t GetSerializedSize() const override;
  void Serialize(Buffer::Iterator start) const override;
  uint32_t Deserialize(Buffer::Iterator start) override;
  void Print(std::ostream& os) const override;

  void SetVersion(uint8_t version);
  uint8_t GetVersion() const;

  void SetOpcode(RoceOpcode opcode);
  RoceOpcode GetOpcode() const;

  void SetUserPhase(RoceUserPhase phase);
  RoceUserPhase GetUserPhase() const;

  void SetFlags(uint8_t flags);
  uint8_t GetFlags() const;

  void SetQpn(uint32_t qpn);
  uint32_t GetQpn() const;

  void SetPsn(uint32_t psn);
  uint32_t GetPsn() const;

  void SetAckPsn(uint32_t ackPsn);
  uint32_t GetAckPsn() const;

  void SetImmData(uint32_t immData);
  uint32_t GetImmData() const;

  void SetRemoteAddr(uint64_t remoteAddr);
  uint64_t GetRemoteAddr() const;

  void SetRkey(uint32_t rkey);
  uint32_t GetRkey() const;

  void SetLength(uint32_t length);
  uint32_t GetLength() const;

  void SetTag(uint32_t tag);
  uint32_t GetTag() const;

  void SetSrc(uint32_t src);
  uint32_t GetSrc() const;

  void SetDst(uint32_t dst);
  uint32_t GetDst() const;

private:
  // 基础控制字段
  uint8_t m_version = 1;
  uint8_t m_opcode = static_cast<uint8_t>(RoceOpcode::Send);
  uint8_t m_userPhase = static_cast<uint8_t>(RoceUserPhase::None);
  uint8_t m_flags = 0;

  // QP/有序传输相关
  uint32_t m_qpn = 0;
  uint32_t m_psn = 0;
  uint32_t m_ackPsn = 0;
  uint32_t m_immData = 0;

  // RDMA 语义相关
  uint64_t m_remoteAddr = 0;
  uint32_t m_rkey = 0;
  uint32_t m_length = 0;

  // 与 3P 业务流对齐的标识。
  // 这些字段保留在本地对象里做调试/关联，不计入序列化线长。
  uint32_t m_tag = 0;
  uint32_t m_src = 0;
  uint32_t m_dst = 0;
};

} // namespace ns3

#endif // ROCE_SIM_HEADER_H
