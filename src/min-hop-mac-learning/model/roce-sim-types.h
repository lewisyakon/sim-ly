#ifndef ROCE_SIM_TYPES_H
#define ROCE_SIM_TYPES_H

#include <cstdint>
#include <set>
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

// RoCE 信息日志等级：
// - Off: 关闭信息级 RoCE 日志。
// - Basic: 仅保留配置、激活、入队等方向性日志。
// - Full: 打开逐 slice 的完整调试日志。
enum class RoceLogLevel : uint8_t {
  Off = 0,
  Basic = 1,
  Full = 2,
};

struct RoceConfig {
  bool enable = false;                 // 总开关
  bool dataUseWrite = true;            // DATA 阶段默认使用 WRITE 语义
  bool strictCompletion = false;       // 预留字段：当前不再参与 ACT 业务报文处理
  uint16_t etherType = 0x9999;         // 二层协议号
  uint32_t defaultSqDepth = 1024;      // 默认 SQ 深度
  uint32_t defaultRqDepth = 1024;      // 默认 RQ 深度
  bool enableTransportAck = true;      // 是否启用传输层 ACK/超时重传
  uint32_t transportWindowPackets = 32; // 每 QP 允许并行在途的 transport packet 数
  bool autoAckTimeout = false;         // 是否根据拓扑/链路配置自动推导 ACK 超时
  double ackTimeoutFactor = 2.0;       // 自动模式下乘在最坏 RTT 基线上的系数
  uint64_t ackTimeoutBaseNs = 0;       // 自动模式下推导得到的未乘系数 RTT 基线（ns）
  uint64_t ackTimeoutNs = 5000;        // 每包 ACK 超时时间（ns）
  uint32_t maxRetransmissions = 8;     // 单包最大重传次数
  RoceLogLevel logLevel = RoceLogLevel::Full; // RoCE 信息日志等级
  std::string modeName = "legacy";     // 模式名（例如 rc_3phase/read/send）
};

// flow 级键：沿用 tag/src/dst 三元组。
struct RoceFlowKey {
  uint32_t tag = 0;
  uint32_t src = 0;
  uint32_t dst = 0;

  bool operator==(const RoceFlowKey& other) const = default;

  bool operator<(const RoceFlowKey& other) const {
    if (tag != other.tag) {
      return tag < other.tag;
    }
    if (src != other.src) {
      return src < other.src;
    }
    return dst < other.dst;
  }
};

// QP 唯一键：src/dst + qpn。
struct RoceQpKey {
  uint32_t src = 0;
  uint32_t dst = 0;
  uint32_t qpn = 0;

  bool operator==(const RoceQpKey& other) const = default;

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
  uint32_t expectedRxPsn = 0;  // 当前最小缺失 PSN；乱序缓存会围绕它向前收敛
  uint32_t sqDepth = 0;
  uint32_t rqDepth = 0;
  uint64_t txPackets = 0;      // 已发送包计数
  uint64_t rxPackets = 0;      // 已接收唯一包计数
  uint64_t rxOutOfOrderPackets = 0; // 先于 expectedRxPsn 到达的唯一包计数
  uint64_t rxDuplicatePackets = 0;  // 重复包/旧包计数
  std::set<uint32_t> pendingRxPsns; // 已到达但尚未与 expectedRxPsn 接上的 future PSN
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
