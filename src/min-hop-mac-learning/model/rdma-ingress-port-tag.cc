#include "rdma-ingress-port-tag.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(RdmaIngressPortTag);

TypeId
RdmaIngressPortTag::GetTypeId() {
  static TypeId tid =
      TypeId("ns3::RdmaIngressPortTag").SetParent<Tag>().AddConstructor<RdmaIngressPortTag>();
  return tid;
}

TypeId
RdmaIngressPortTag::GetInstanceTypeId() const {
  return GetTypeId();
}

uint32_t
RdmaIngressPortTag::GetSerializedSize() const {
  return 4;
}

void
RdmaIngressPortTag::Serialize(TagBuffer buffer) const {
  buffer.WriteU32(m_deviceIndex);
}

void
RdmaIngressPortTag::Deserialize(TagBuffer buffer) {
  m_deviceIndex = buffer.ReadU32();
}

void
RdmaIngressPortTag::Print(std::ostream& os) const {
  os << "devIndex=" << m_deviceIndex;
}

void
RdmaIngressPortTag::SetDeviceIndex(uint32_t deviceIndex) {
  m_deviceIndex = deviceIndex;
}

uint32_t
RdmaIngressPortTag::GetDeviceIndex() const {
  return m_deviceIndex;
}

} // namespace ns3
