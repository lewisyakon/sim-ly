#ifndef RDMA_INGRESS_PORT_TAG_H
#define RDMA_INGRESS_PORT_TAG_H

#include "ns3/tag.h"

namespace ns3 {

class RdmaIngressPortTag : public Tag {
public:
  static TypeId GetTypeId();
  TypeId GetInstanceTypeId() const override;

  uint32_t GetSerializedSize() const override;
  void Serialize(TagBuffer buffer) const override;
  void Deserialize(TagBuffer buffer) override;
  void Print(std::ostream& os) const override;

  void SetDeviceIndex(uint32_t deviceIndex);
  uint32_t GetDeviceIndex() const;

private:
  uint32_t m_deviceIndex = 0;
};

} // namespace ns3

#endif // RDMA_INGRESS_PORT_TAG_H
