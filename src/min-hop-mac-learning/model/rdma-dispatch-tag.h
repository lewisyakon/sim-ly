#ifndef RDMA_DISPATCH_TAG_H
#define RDMA_DISPATCH_TAG_H

#include "3PHeader.h"
#include "ns3/tag.h"
#include "roce-sim-types.h"
#include "roce-tx-scheduler.h"

namespace ns3 {

class RdmaDispatchTag : public Tag {
public:
  static TypeId GetTypeId();
  TypeId GetInstanceTypeId() const override;

  uint32_t GetSerializedSize() const override;
  void Serialize(TagBuffer buffer) const override;
  void Deserialize(TagBuffer buffer) override;
  void Print(std::ostream& os) const override;

  void SetWorkId(uint64_t workId);
  uint64_t GetWorkId() const;

  void SetFlowKey(const RoceFlowKey& flowKey);
  RoceFlowKey GetFlowKey() const;

  void SetState(ThreePHeader::State state);
  ThreePHeader::State GetState() const;

  void SetQpn(uint32_t qpn);
  uint32_t GetQpn() const;

  void SetPsn(uint32_t psn);
  uint32_t GetPsn() const;

  void SetOpcode(RoceOpcode opcode);
  RoceOpcode GetOpcode() const;

  void SetTxClass(RoceTxClass txClass);
  RoceTxClass GetTxClass() const;

  void SetTotalBytes(uint64_t totalBytes);
  uint64_t GetTotalBytes() const;

  void SetSliceBytes(uint32_t sliceBytes);
  uint32_t GetSliceBytes() const;

  void SetPacketIndex(uint64_t packetIndex);
  uint64_t GetPacketIndex() const;

  void SetTotalPackets(uint64_t totalPackets);
  uint64_t GetTotalPackets() const;

  void SetQueueId(uint32_t queueId);
  uint32_t GetQueueId() const;

  void SetLastSlice(bool isLastSlice);
  bool IsLastSlice() const;

  void SetRetransmission(bool isRetransmission);
  bool IsRetransmission() const;

  void SetTransportAck(bool isTransportAck);
  bool IsTransportAck() const;

private:
  enum Flags : uint8_t {
    FLAG_LAST_SLICE = 1 << 0,
    FLAG_RETRANSMISSION = 1 << 1,
    FLAG_TRANSPORT_ACK = 1 << 2,
  };

  void SetFlag_(Flags flag, bool enabled);
  bool GetFlag_(Flags flag) const;

private:
  uint64_t m_workId = 0;
  uint64_t m_totalBytes = 0;
  uint64_t m_packetIndex = 0;
  uint64_t m_totalPackets = 1;
  uint32_t m_tag = 0;
  uint32_t m_src = 0;
  uint32_t m_dst = 0;
  uint32_t m_qpn = 0;
  uint32_t m_psn = 0;
  uint32_t m_sliceBytes = 0;
  uint32_t m_queueId = 0;
  uint8_t m_state = static_cast<uint8_t>(ThreePHeader::DATA);
  uint8_t m_opcode = static_cast<uint8_t>(RoceOpcode::Send);
  uint8_t m_txClass = static_cast<uint8_t>(RoceTxClass::Data);
  uint8_t m_flags = 0;
};

} // namespace ns3

#endif // RDMA_DISPATCH_TAG_H
