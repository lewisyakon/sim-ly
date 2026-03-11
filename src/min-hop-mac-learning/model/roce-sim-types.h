#ifndef ROCE_SIM_TYPES_H
#define ROCE_SIM_TYPES_H

#include <cstdint>
#include <string>

namespace ns3 {

// RoCE 运行模式：
// - Legacy: 完全不启用 RoCE 旁路。
// - RcThreePhase: 以 3P 工作流承载 RC 语义。
enum class RoceMode : uint8_t {
  Legacy = 0,
  RcThreePhase = 1,
};

// 简化后的 QP 状态机。
enum class RoceQpState : uint8_t {
  Reset = 0,
  Init = 1,
  Rtr = 2,
  Rts = 3,
  Err = 4,
};

// 仿真侧支持的 opcode 子集。
enum class RoceOpcode : uint8_t {
  Send = 0,
  SendWithImm = 1,
  RdmaWrite = 2,
  RdmaWriteWithImm = 3,
  RdmaReadReq = 4,
  RdmaReadResp = 5,
  Ack = 6,
};

// 将现有 REQUEST/DATA/ACT 阶段映射到用户态语义相位。
enum class RoceUserPhase : uint8_t {
  None = 0,
  Request = 1,
  Data = 2,
  Act = 3,
};

struct RoceConfig {
  bool enable = false;                 // 总开关
  bool dataUseWrite = true;            // DATA 阶段默认使用 WRITE 语义
  bool strictCompletion = false;       // 预留字段：当前不再参与 ACT 业务报文处理
  uint16_t etherType = 0x9999;         // 二层协议号
  uint32_t defaultSqDepth = 1024;      // 默认 SQ 深度
  uint32_t defaultRqDepth = 1024;      // 默认 RQ 深度
  std::string modeName = "legacy";     // 模式名（例如 rc_3phase/read/send）
};

// QP 唯一键：src/dst + qpn。
struct RoceQpKey {
  uint32_t src = 0;
  uint32_t dst = 0;
  uint32_t qpn = 0;

  bool operator<(const RoceQpKey& other) const {
    if (src != other.src) {
      return src < other.src;
    }
    if (dst != other.dst) {
      return dst < other.dst;
    }
    return qpn < other.qpn;
  }
};

// QP 运行时上下文（PSN、状态、统计）。
struct RoceQpContext {
  RoceQpState state = RoceQpState::Reset;
  uint32_t nextPsn = 0;        // 下一个发送 PSN
  uint32_t expectedRxPsn = 0;  // 期望接收的下一个 PSN
  uint32_t sqDepth = 0;
  uint32_t rqDepth = 0;
  uint64_t txPackets = 0;      // 已发送包计数
  uint64_t rxPackets = 0;      // 已接收包计数
};

// 简化 completion 队列项。
struct RoceCompletion {
  uint32_t src = 0;
  uint32_t dst = 0;
  uint32_t qpn = 0;
  uint32_t psn = 0;
  uint32_t wrId = 0;
  bool success = true;
};

} // namespace ns3

#endif // ROCE_SIM_TYPES_H
