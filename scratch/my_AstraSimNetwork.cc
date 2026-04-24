/* 
*Copyright (c) 2024, Alibaba Group;
*Licensed under the Apache License, Version 2.0 (the "License");
*you may not use this file except in compliance with the License.
*You may obtain a copy of the License at

*   http://www.apache.org/licenses/LICENSE-2.0

*Unless required by applicable law or agreed to in writing, software
*distributed under the License is distributed on an "AS IS" BASIS,
*WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*See the License for the specific language governing permissions and
*limitations under the License.
*/

#include "astra-sim/system/AstraNetworkAPI.hh"
#include "astra-sim/system/Sys.hh"
#include "astra-sim/system/RecvPacketEventHadndlerData.hh"
#include "astra-sim/system/Common.hh"
#include "astra-sim/system/MockNcclLog.h"
#include "astra-sim/system/OverlayCollectiveInjector.hh"
#include "astra-sim/workload/Workload.hh"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/node-list.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/min-hop-bridge-net-device.h"
#include "ns3/PppRoundRobinQueue.h"
#include "my_entry.h"
#include <execinfo.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <stdio.h>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <malloc.h>
#ifdef NS3_MTP
#include "ns3/mtp-module.h"
#endif
#ifdef NS3_MPI
#include "ns3/mpi-module.h"
#endif
#include <mutex>

#define RESULT_PATH "./ncclFlowModel_"

using namespace std;
using namespace ns3;

extern std::map<std::pair<std::pair<int, int>,int>, AstraSim::ncclFlowTag> receiver_pending_queue;
extern uint32_t node_num, switch_num, link_num, trace_num, nvswitch_num, gpus_per_server;
extern GPUType gpu_type;
extern std::vector<int>NVswitchs;
string network_config;
extern string network_workload;
std::mutex g_recvHashMutex;
std::mutex g_totRendsend;
static const int mx_sys = 32;

uint64_t totRendezvousSend=0, totRendezvousRecv=0;

namespace conf
{
  uint32_t system = 0;
  uint32_t rank = 0;
  uint32_t thread = 2;
  bool nullmsg = false;
}; 

struct sim_event {
  void *buffer;
  uint64_t count;
  int type;
  int dst;
  int tag;
  string fnType;
};

class ASTRASimNetwork : public AstraSim::AstraNetworkAPI {
private:
  int npu_offset;

public:
  queue<sim_event> sim_event_queue;
  ASTRASimNetwork(int rank, int npu_offset) : AstraNetworkAPI(rank) {
    this->npu_offset = npu_offset;
  }
  ~ASTRASimNetwork() {}
  int sim_comm_size(AstraSim::sim_comm comm, int *size) { return 0; }
  int sim_finish() {
    for (auto it = nodeHash.begin(); it != nodeHash.end(); it++) {
      pair<int, int> p = it->first;
      if (p.second == 0) {
        // std::cout << "sim_finish on sent, " << " Thread id: " << pthread_self() << std::endl;
        // cout << "All data sent from node " << p.first << " is " << it->second
        //      << "\n";
      } else {
        // std::cout << "sim_finish on received, " << " Thread id: " << pthread_self() << std::endl;
        // cout << "All data received by node " << p.first << " is " << it->second
        //      << "\n";
      }
    }
    // uint64_t eventCount = Simulator::GetEventCount();
    // NS_LOG_INFO(" * * * total events : " << eventCount );
    // NS_LOG_INFO("[Rendezvous]: recv="<<totRendezvousRecv<<" send="<<totRendezvousSend);

    // uint64_t eventCounts[mx_sys];
    // MPI_Gather(&eventCount,
    //         1,
    //         MPI_UNSIGNED_LONG_LONG,
    //         eventCounts,
    //         1,
    //         MPI_UNSIGNED_LONG_LONG,
    //         0,
    //         MpiInterface::GetCommunicator());

    // NS_LOG_INFO(" total events : " << std::accumulate(eventCounts, eventCounts + conf::system, 0ULL));

    // Simulator::Destroy();
    // #ifdef NS3_MPI
    // MpiInterface::Disable();
    // #endif

    // exit(0);
    return 0;
  }
  double sim_time_resolution() { return 0; }
  int sim_init(AstraSim::AstraMemoryAPI *MEM) { return 0; }
  AstraSim::timespec_t sim_get_time() {
    AstraSim::timespec_t timeSpec;
    timeSpec.time_val = Simulator::Now().GetNanoSeconds();
    return timeSpec;
  }
  virtual void sim_schedule(AstraSim::timespec_t delta,
                            void (*fun_ptr)(void *fun_arg), void *fun_arg) {
    task1 t;
    t.type = 2;
    t.fun_arg = fun_arg;
    t.msg_handler = fun_ptr;
    t.schTime = delta.time_val;
    Simulator::Schedule(NanoSeconds(t.schTime), t.msg_handler, t.fun_arg);
    return;
  }
  virtual int sim_send(void *buffer,   
                       uint64_t count, 
                       int type,       
                       int dst,
                       int tag,                       
                       AstraSim::sim_request *request, 
                       void (*msg_handler)(void *fun_arg), void *fun_arg) {
    if(count==1023)
    {
      #ifdef NS3_MTP
      std::unique_lock<std::mutex> lock(g_totRendsend);
      #endif
      totRendezvousSend++;

      #ifdef NS3_MTP
      lock.unlock();
      #endif

      dst+=npu_offset;
      // NS_LOG_INFO(" * * * [Rendezvous send] tag="<<tag<<" src="<<rank<<" dst="<<dst);
      SendFlow(rank, dst, count, msg_handler, fun_arg, tag, request);

      // msg_handler(fun_arg);
      return 0;
    }

    dst += npu_offset;
    task1 t;
    t.src = rank;
    t.dest = dst;
    t.count = count;
    t.type = 0;
    t.fun_arg = fun_arg;
    t.msg_handler = msg_handler;
    AstraSim::RecvPacketEventHadndlerData* ehd = (AstraSim::RecvPacketEventHadndlerData*) t.fun_arg;

    tag=request->flowTag.tag_id;
    // NS_LOG_INFO(" * * * [sim send] tag="<<tag<<" src="<<rank<<" dst="<<dst);
    SendFlow(rank, dst, count, msg_handler, fun_arg, tag, request);
    return 0;
  }
  virtual int sim_recv(void *buffer, uint64_t count, int type, int src, int tag,
                       AstraSim::sim_request *request,
                       void (*msg_handler)(void *fun_arg), void *fun_arg) {
    if(count==1023){
      // totRendezvousRecv++;
      src += npu_offset;
      // NS_LOG_INFO(" * * * [Rendezvous recv] tag="<<tag<<" src="<<src<<" dst="<<rank);
      Ptr<sendflowclient> clientHelper = nodeapptable[rank];
      clientHelper->set_recvfunc(tag,src,rank,{msg_handler,fun_arg});        
      // msg_handler(fun_arg);
      return 0;
    }

    //这里recvhash要改，不然不对
    // #ifdef NS3_MTP
    // std::unique_lock<std::mutex> lock(g_recvHashMutex);
    // // MtpInterface::explicitCriticalSection cs;
    // #endif
    // MockNcclLog* NcclLog = MockNcclLog::getInstance();
    AstraSim::ncclFlowTag flowTag = request->flowTag;
    src += npu_offset;
    task1 t;
    t.src = src;
    t.dest = rank;
    t.count = count;
    t.type = 1;
    t.fun_arg = fun_arg;
    t.msg_handler = msg_handler;
    int flow_id = request->flowTag.current_flow_id;

    // NcclLog->writeLog(NcclLogLevel::DEBUG,"[Receive event registration] src %d sim_recv on rank %d tag_id %d channdl id %d",src,rank,tag,ehd->flowTag.channel_id);
    AstraSim::RecvPacketEventHadndlerData* ehd = (AstraSim::RecvPacketEventHadndlerData*) t.fun_arg;
    tag=ehd->flowTag.tag_id;
    // NS_LOG_INFO(" * * * [sim recv] tag="<<tag<<" src="<<src<<" dst="<<rank);
    Ptr<sendflowclient> clientHelper=nodeapptable[rank];     
    
    if (clientHelper->check_recvhash(tag,src,rank)) {
      uint64_t count = clientHelper->get_recvhash(tag,src,rank);
      if (count == t.count) {
        clientHelper->erase_recvhash(tag,src,rank);
        // #ifdef NS3_MTP
        // lock.unlock();
        // // cs.ExitSection();
        // #endif
        ehd->flowTag=clientHelper->get_flowtag(tag,src,rank).flowTag;
        t.msg_handler(t.fun_arg);
        clientHelper->erase_flowtag(tag,src,rank);
        // NS_LOG_INFO("flowid= " << flow_id << " 接收确认 ");
        goto sim_recv_end_section;
      } else if (count > t.count) {
        clientHelper->set_recvhash(tag,src,rank,count - t.count);
        // #ifdef NS3_MTP
        // lock.unlock();
        // // cs.ExitSection();
        // #endif
        // NS_LOG_INFO("flowid= " << flow_id << " 接收确认 ");
        ehd->flowTag=clientHelper->get_flowtag(tag,src,rank).flowTag;
        t.msg_handler(t.fun_arg);
        goto sim_recv_end_section;
      } else {
        clientHelper->set_recvfunc(tag,src,rank,{msg_handler,fun_arg});
        // recvHash.erase({tag,{src,rank}});
      }
    } else {
      clientHelper->set_recvfunc(tag,src,rank,{msg_handler,fun_arg});
    }
    // #ifdef NS3_MTP
    // lock.unlock();
    // // cs.ExitSection();
    // #endif

sim_recv_end_section:    
    return 0;
  }
  void handleEvent(int dst, int cnt) {
  }
};

struct user_param {
  int thread;
  string workload;
  string network_topo;
  string network_conf;
  user_param() {
    thread = 1;
    workload = "";
    network_topo = "";
    network_conf = "";
  };
  ~user_param(){};
};

static int ParseAllGpusFromWorkloadHeader(const std::string& workload_path) {
  std::ifstream in(workload_path);
  if (!in.is_open()) {
    return -1;
  }
  std::string line;
  if (!std::getline(in, line)) {
    return -1;
  }
  std::istringstream iss(line);
  std::string tok;
  while (iss >> tok) {
    if (tok == "all_gpus:") {
      int v = -1;
      if (iss >> v) {
        return v;
      }
      return -1;
    }
  }
  return -1;
}

static std::string ResolveNetworkConfigPathFromArgv(int argc, char* argv[]) {
  std::string config_path = "simconfig/config.json";
  const std::string long_prefix = "--network_conf=";
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == nullptr) {
      continue;
    }
    const std::string arg = argv[i];
    if (arg == "-c" && (i + 1) < argc && argv[i + 1] != nullptr) {
      config_path = argv[i + 1];
      ++i;
      continue;
    }
    if (arg.rfind(long_prefix, 0) == 0) {
      config_path = arg.substr(long_prefix.size());
    }
  }
  return config_path;
}

static uint32_t ResolveThreadFromConfig(const std::string& config_path, uint32_t fallback) {
  std::ifstream in(config_path);
  if (!in.is_open()) {
    return fallback;
  }
  nlohmann::json j;
  try {
    in >> j;
  } catch (...) {
    return fallback;
  }
  if (!j.contains("thread")) {
    return fallback;
  }
  try {
    int thread = -1;
    if (j["thread"].is_number_integer()) {
      thread = j["thread"].get<int>();
    } else if (j["thread"].is_string()) {
      thread = std::stoi(j["thread"].get<std::string>());
    }
    if (thread > 0) {
      return static_cast<uint32_t>(thread);
    }
  } catch (...) {
    return fallback;
  }
  return fallback;
}

static std::string ResolvePpScheduleFromConfig(const std::string& config_path) {
  std::ifstream in(config_path);
  if (!in.is_open()) {
    return "gpipe";
  }
  nlohmann::json j;
  try {
    in >> j;
  } catch (...) {
    return "gpipe";
  }
  if (j.contains("pp_schedule") && j["pp_schedule"].is_string()) {
    return j["pp_schedule"].get<std::string>();
  }
  if (j.contains("workload") && j["workload"].is_object()) {
    const auto& wl = j["workload"];
    if (wl.contains("pp_schedule") && wl["pp_schedule"].is_string()) {
      return wl["pp_schedule"].get<std::string>();
    }
  }
  return "gpipe";
}

/* Disable memory/log monitoring implementation for pure simulation runs.
struct ProcessMemSnapshot {
  uint64_t vmRssKb = 0;
  uint64_t vmPeakKb = 0;
  uint64_t vmSizeKb = 0;
  uint64_t vmHwmKb = 0;
  uint64_t vmDataKb = 0;
  uint64_t vmSwapKb = 0;
  uint64_t rssAnonKb = 0;
  uint64_t rssFileKb = 0;
  uint64_t rssShmemKb = 0;
  uint64_t threads = 0;
  bool valid = false;
};

struct AllocatorMemSnapshot {
  uint64_t arenaBytes = 0;
  uint64_t mmapBytes = 0;
  uint64_t inUseBytes = 0;
  uint64_t freeBytes = 0;
  uint64_t releasableBytes = 0;
  bool valid = false;
};

struct MemWatchConfig {
  bool enable = false;
  bool useWallClock = true;
  double intervalSeconds = 100.0;
  uint32_t maxSamples = 120;
  uint32_t topNodes = 0;
  uint32_t nodeDetailEverySamples = 6;
  std::vector<int> watchNodes;
  bool wallPeakEnable = true;
  double wallPeakIntervalSeconds = 1.0;
  bool wallPeakLogOnNewPeak = true;
  uint32_t wallPeakPeriodicEverySamples = 60;
};

struct QueueMemStats {
  uint64_t p2pRrqDevices = 0;
  uint64_t p2pRrqTotalPackets = 0;
  uint64_t p2pRrqPriorityPackets = 0;
  uint64_t p2pRrqSubQueuePackets = 0;
  uint64_t p2pRrqPriorityBytes = 0;
  uint64_t p2pRrqSubQueueBytes = 0;
  uint64_t p2pRrqApproxBytes = 0;

  uint64_t bridgeIngressQueues = 0;
  uint64_t bridgeIngressLocalQueues = 0;
  uint64_t bridgeIngressTotalPackets = 0;
  uint64_t bridgeIngressPriorityPackets = 0;
  uint64_t bridgeIngressSubQueuePackets = 0;
  uint64_t bridgeIngressPriorityBytes = 0;
  uint64_t bridgeIngressSubQueueBytes = 0;
  uint64_t bridgeIngressApproxBytes = 0;
};

struct MemWatchState {
  MemWatchConfig cfg;
  uint32_t sampleIndex = 0;
  uint64_t lastEventCount = 0;
  uint64_t lastVmRssKb = 0;
  uint64_t lastVmSwapKb = 0;
  uint64_t lastTrackedEntries = 0;
  sendflowclient::RuntimeTableStats lastAggStats;
  QueueMemStats lastQueueStats;
  bool hasLastAggStats = false;
  bool hasLastQueueStats = false;
  bool hasLast = false;
  std::atomic<bool> stopRequested{false};
  std::atomic<bool> wallSampleScheduled{false};
  std::atomic<bool> maxSamplesLogged{false};
  std::atomic<bool> invalidContextLogged{false};
  uint32_t wallEventContext = ns3::Simulator::NO_CONTEXT;
  std::thread wallSamplerThread;
  std::chrono::steady_clock::time_point wallStartTime;
  std::atomic<bool> wallPeakStopRequested{false};
  std::thread wallPeakSamplerThread;
  std::chrono::steady_clock::time_point wallPeakStartTime;
  std::mutex wallPeakMutex;
  uint64_t wallPeakSampleIndex = 0;
  uint64_t wallPeakAtMs = 0;
  uint64_t wallPeakVmRssKb = 0;
  uint64_t wallPeakVmHwmKb = 0;
  uint64_t wallPeakVmSizeKb = 0;
  uint64_t wallPeakRssAnonKb = 0;
  uint64_t wallPeakThreads = 0;
  uint64_t wallPeakHeapArenaKb = 0;
  uint64_t wallPeakHeapMmapKb = 0;
  uint64_t wallPeakHeapInUseKb = 0;
  uint64_t wallPeakHeapFreeKb = 0;
  bool hasWallPeak = false;
};

static bool ParseStatusValueKb(const std::string& line,
                               const std::string& key,
                               uint64_t* valueKb) {
  if (valueKb == nullptr) {
    return false;
  }
  if (line.rfind(key, 0) != 0) {
    return false;
  }
  std::istringstream iss(line.substr(key.size()));
  uint64_t value = 0;
  if (!(iss >> value)) {
    return false;
  }
  *valueKb = value;
  return true;
}

static ProcessMemSnapshot ReadProcessMemSnapshot() {
  ProcessMemSnapshot snapshot;
  std::ifstream in("/proc/self/status");
  if (!in.is_open()) {
    return snapshot;
  }

  std::string line;
  while (std::getline(in, line)) {
    ParseStatusValueKb(line, "VmRSS:", &snapshot.vmRssKb);
    ParseStatusValueKb(line, "VmPeak:", &snapshot.vmPeakKb);
    ParseStatusValueKb(line, "VmSize:", &snapshot.vmSizeKb);
    ParseStatusValueKb(line, "VmHWM:", &snapshot.vmHwmKb);
    ParseStatusValueKb(line, "VmData:", &snapshot.vmDataKb);
    ParseStatusValueKb(line, "VmSwap:", &snapshot.vmSwapKb);
    ParseStatusValueKb(line, "RssAnon:", &snapshot.rssAnonKb);
    ParseStatusValueKb(line, "RssFile:", &snapshot.rssFileKb);
    ParseStatusValueKb(line, "RssShmem:", &snapshot.rssShmemKb);
    ParseStatusValueKb(line, "Threads:", &snapshot.threads);
  }
  snapshot.valid = true;
  return snapshot;
}

static AllocatorMemSnapshot ReadAllocatorMemSnapshot() {
  AllocatorMemSnapshot snapshot;
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
  const struct mallinfo2 info = mallinfo2();
  snapshot.arenaBytes = static_cast<uint64_t>(info.arena);
  snapshot.mmapBytes = static_cast<uint64_t>(info.hblkhd);
  snapshot.inUseBytes = static_cast<uint64_t>(info.uordblks);
  snapshot.freeBytes = static_cast<uint64_t>(info.fordblks);
  snapshot.releasableBytes = static_cast<uint64_t>(info.keepcost);
  snapshot.valid = true;
#else
  const struct mallinfo info = mallinfo();
  snapshot.arenaBytes = static_cast<uint64_t>(std::max(0, info.arena));
  snapshot.mmapBytes = static_cast<uint64_t>(std::max(0, info.hblkhd));
  snapshot.inUseBytes = static_cast<uint64_t>(std::max(0, info.uordblks));
  snapshot.freeBytes = static_cast<uint64_t>(std::max(0, info.fordblks));
  snapshot.releasableBytes = static_cast<uint64_t>(std::max(0, info.keepcost));
  snapshot.valid = true;
#endif
#endif
  return snapshot;
}

static void MemWatchLog(const std::string& line) {
  static std::mutex sMemWatchLogMutex;
  std::lock_guard<std::mutex> guard(sMemWatchLogMutex);
  NS_LOG_UNCOND(line);
}

static MemWatchConfig ResolveMemWatchConfigFromConfig(const std::string& config_path) {
  MemWatchConfig cfg;
  std::ifstream in(config_path);
  if (!in.is_open()) {
    return cfg;
  }

  nlohmann::json j;
  try {
    in >> j;
  } catch (...) {
    return cfg;
  }
  if (!j.contains("debug") || !j["debug"].is_object()) {
    return cfg;
  }

  const auto& debug = j["debug"];
  if (debug.contains("mem_log_enable") && debug["mem_log_enable"].is_boolean()) {
    cfg.enable = debug["mem_log_enable"].get<bool>();
  }
  if (debug.contains("mem_log_time_mode") && debug["mem_log_time_mode"].is_string()) {
    std::string mode = debug["mem_log_time_mode"].get<std::string>();
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (mode == "sim") {
      cfg.useWallClock = false;
    } else if (mode == "wall") {
      cfg.useWallClock = true;
    }
  }
  if (debug.contains("mem_log_interval_s") && debug["mem_log_interval_s"].is_number()) {
    const double interval = debug["mem_log_interval_s"].get<double>();
    if (interval > 0) {
      cfg.intervalSeconds = interval;
    }
  }
  if (debug.contains("mem_log_interval_us") && debug["mem_log_interval_us"].is_number()) {
    const double intervalUs = debug["mem_log_interval_us"].get<double>();
    if (intervalUs > 0) {
      cfg.intervalSeconds = intervalUs * 1e-6;
    }
  }
  if (debug.contains("mem_log_interval_ns") && debug["mem_log_interval_ns"].is_number()) {
    const double intervalNs = debug["mem_log_interval_ns"].get<double>();
    if (intervalNs > 0) {
      cfg.intervalSeconds = intervalNs * 1e-9;
    }
  }
  if (debug.contains("mem_log_max_samples") && debug["mem_log_max_samples"].is_number_integer()) {
    const int samples = debug["mem_log_max_samples"].get<int>();
    // 0 means unlimited sampling; negative values keep default.
    if (samples >= 0) {
      cfg.maxSamples = static_cast<uint32_t>(samples);
    }
  }
  if (debug.contains("mem_log_top_nodes") && debug["mem_log_top_nodes"].is_number_integer()) {
    const int topNodes = debug["mem_log_top_nodes"].get<int>();
    if (topNodes >= 0) {
      cfg.topNodes = static_cast<uint32_t>(topNodes);
    }
  }
  if (debug.contains("mem_log_node_detail_every_samples") &&
      debug["mem_log_node_detail_every_samples"].is_number_integer()) {
    const int every = debug["mem_log_node_detail_every_samples"].get<int>();
    if (every >= 0) {
      cfg.nodeDetailEverySamples = static_cast<uint32_t>(every);
    }
  }
  if (debug.contains("mem_log_nodes") && debug["mem_log_nodes"].is_array()) {
    cfg.watchNodes.clear();
    for (const auto& v : debug["mem_log_nodes"]) {
      if (!v.is_number_integer()) {
        continue;
      }
      const int nodeId = v.get<int>();
      if (nodeId >= 0) {
        cfg.watchNodes.push_back(nodeId);
      }
    }
    std::sort(cfg.watchNodes.begin(), cfg.watchNodes.end());
    cfg.watchNodes.erase(std::unique(cfg.watchNodes.begin(), cfg.watchNodes.end()),
                         cfg.watchNodes.end());
  }
  if (debug.contains("mem_log_wall_peak_enable") &&
      debug["mem_log_wall_peak_enable"].is_boolean()) {
    cfg.wallPeakEnable = debug["mem_log_wall_peak_enable"].get<bool>();
  }
  if (debug.contains("mem_log_wall_peak_interval_ms") &&
      debug["mem_log_wall_peak_interval_ms"].is_number()) {
    const double intervalMs = debug["mem_log_wall_peak_interval_ms"].get<double>();
    if (intervalMs > 0) {
      cfg.wallPeakIntervalSeconds = intervalMs * 1e-3;
    }
  }
  if (debug.contains("mem_log_wall_peak_interval_s") &&
      debug["mem_log_wall_peak_interval_s"].is_number()) {
    const double intervalS = debug["mem_log_wall_peak_interval_s"].get<double>();
    if (intervalS > 0) {
      cfg.wallPeakIntervalSeconds = intervalS;
    }
  }
  if (debug.contains("mem_log_wall_peak_log_on_new_peak") &&
      debug["mem_log_wall_peak_log_on_new_peak"].is_boolean()) {
    cfg.wallPeakLogOnNewPeak = debug["mem_log_wall_peak_log_on_new_peak"].get<bool>();
  }
  if (debug.contains("mem_log_wall_peak_periodic_every_samples") &&
      debug["mem_log_wall_peak_periodic_every_samples"].is_number_integer()) {
    const int every = debug["mem_log_wall_peak_periodic_every_samples"].get<int>();
    if (every >= 0) {
      cfg.wallPeakPeriodicEverySamples = static_cast<uint32_t>(every);
    }
  }
  return cfg;
}

static bool IsNodeOwnedByCurrentRank(int nodeId) {
  auto nodeIt = nodetable.find(nodeId);
  if (nodeIt == nodetable.end() || nodeIt->second == nullptr) {
    return false;
  }
#ifdef NS3_MPI
  return nodeIt->second->GetSystemId() == MpiInterface::GetSystemId();
#else
  return true;
#endif
}

static uint32_t ResolveMemWatchEventContext(const MemWatchConfig& cfg) {
  const uint32_t nNodes = ns3::NodeList::GetNNodes();
  auto IsValidNodeId = [nNodes](int nodeId) -> bool {
    return nodeId >= 0 && static_cast<uint32_t>(nodeId) < nNodes;
  };

  for (int nodeId : cfg.watchNodes) {
    if (IsValidNodeId(nodeId) && IsNodeOwnedByCurrentRank(nodeId)) {
      return static_cast<uint32_t>(nodeId);
    }
  }

  for (const auto& [nodeId, app] : nodeapptable) {
    if (app == nullptr) {
      continue;
    }
    if (IsValidNodeId(nodeId) && IsNodeOwnedByCurrentRank(nodeId)) {
      return static_cast<uint32_t>(nodeId);
    }
  }

  if (nNodes > 0) {
    return 0;
  }
  return ns3::Simulator::NO_CONTEXT;
}

static uint64_t BytesToKb(uint64_t bytes) {
  return bytes / 1024;
}

static QueueMemStats CollectNodeQueueMemStats(int nodeId) {
  QueueMemStats stats;
  if (nodeId < 0 || static_cast<uint32_t>(nodeId) >= ns3::NodeList::GetNNodes()) {
    return stats;
  }
  if (!IsNodeOwnedByCurrentRank(nodeId)) {
    return stats;
  }

  Ptr<Node> node = ns3::NodeList::GetNode(static_cast<uint32_t>(nodeId));
  if (node == nullptr) {
    return stats;
  }

  for (uint32_t devIdx = 0; devIdx < node->GetNDevices(); ++devIdx) {
    Ptr<NetDevice> dev = node->GetDevice(devIdx);

    if (Ptr<PointToPointNetDevice> p2p = DynamicCast<PointToPointNetDevice>(dev)) {
      Ptr<Queue<Packet>> queue = p2p->GetQueue();
      if (queue != nullptr) {
        Ptr<PppRoundRobinQueue<Packet>> rrq = DynamicCast<PppRoundRobinQueue<Packet>>(queue);
        if (rrq != nullptr) {
          const uint64_t priorityPkts = rrq->GetPriorityQueueLength();
          const uint64_t subQueuePkts = rrq->GetSubQueueTotalLength();
          const uint64_t priorityBytes = rrq->GetPriorityQueueBytes();
          const uint64_t subQueueBytes = rrq->GetSubQueueTotalBytes();

          stats.p2pRrqDevices += 1;
          stats.p2pRrqTotalPackets += rrq->GetNPackets();
          stats.p2pRrqPriorityPackets += priorityPkts;
          stats.p2pRrqSubQueuePackets += subQueuePkts;
          stats.p2pRrqPriorityBytes += priorityBytes;
          stats.p2pRrqSubQueueBytes += subQueueBytes;
          stats.p2pRrqApproxBytes += (priorityBytes + subQueueBytes);
        }
      }
    }

    if (Ptr<MinHopBridgeNetDevice> bridge = DynamicCast<MinHopBridgeNetDevice>(dev)) {
      const auto snapshots = bridge->GetIngressQueueSnapshots();
      for (const auto& snapshot : snapshots) {
        stats.bridgeIngressQueues += 1;
        if (snapshot.isLocalInjectionQueue) {
          stats.bridgeIngressLocalQueues += 1;
        }
        stats.bridgeIngressTotalPackets += snapshot.totalPackets;
        stats.bridgeIngressPriorityPackets += snapshot.priorityPackets;
        stats.bridgeIngressSubQueuePackets += snapshot.subQueuePackets;
        stats.bridgeIngressPriorityBytes += snapshot.priorityBytes;
        stats.bridgeIngressSubQueueBytes += snapshot.subQueueBytes;
        stats.bridgeIngressApproxBytes += snapshot.approxBytes;
      }
    }
  }
  return stats;
}

static QueueMemStats CollectAggregateQueueMemStats() {
  QueueMemStats totals;
  const uint32_t nNodes = ns3::NodeList::GetNNodes();
  for (uint32_t nodeId = 0; nodeId < nNodes; ++nodeId) {
    const QueueMemStats nodeStats = CollectNodeQueueMemStats(static_cast<int>(nodeId));
    totals.p2pRrqDevices += nodeStats.p2pRrqDevices;
    totals.p2pRrqTotalPackets += nodeStats.p2pRrqTotalPackets;
    totals.p2pRrqPriorityPackets += nodeStats.p2pRrqPriorityPackets;
    totals.p2pRrqSubQueuePackets += nodeStats.p2pRrqSubQueuePackets;
    totals.p2pRrqPriorityBytes += nodeStats.p2pRrqPriorityBytes;
    totals.p2pRrqSubQueueBytes += nodeStats.p2pRrqSubQueueBytes;
    totals.p2pRrqApproxBytes += nodeStats.p2pRrqApproxBytes;

    totals.bridgeIngressQueues += nodeStats.bridgeIngressQueues;
    totals.bridgeIngressLocalQueues += nodeStats.bridgeIngressLocalQueues;
    totals.bridgeIngressTotalPackets += nodeStats.bridgeIngressTotalPackets;
    totals.bridgeIngressPriorityPackets += nodeStats.bridgeIngressPriorityPackets;
    totals.bridgeIngressSubQueuePackets += nodeStats.bridgeIngressSubQueuePackets;
    totals.bridgeIngressPriorityBytes += nodeStats.bridgeIngressPriorityBytes;
    totals.bridgeIngressSubQueueBytes += nodeStats.bridgeIngressSubQueueBytes;
    totals.bridgeIngressApproxBytes += nodeStats.bridgeIngressApproxBytes;
  }
  return totals;
}

static void LogNodeMemWatchStats(
    int nodeId,
    const sendflowclient::RuntimeTableStats& stats,
    const std::string& prefix = "[MemWatch][node]") {
  const QueueMemStats queueStats = CollectNodeQueueMemStats(nodeId);
  std::ostringstream oss;
  oss << prefix
      << " node=" << nodeId
      << " ioTotPkts=" << stats.flowIoTotalBufferedPackets
      << " ioTxPkts=" << stats.flowIoTxBufferedPackets
      << " ioExtIngressPkts=" << stats.flowIoExternalIngressPackets
      << " ioExtHoldPkts=" << stats.flowIoExternalHoldPackets
      << " ioTotBytes=" << stats.flowIoTotalBufferedBytes
      << " ioApproxKB=" << BytesToKb(stats.flowIoTotalBufferedApproxBytes)
      << " ioEvt=" << stats.flowIoPendingEvents
      << " p2pRrqDev=" << queueStats.p2pRrqDevices
      << " p2pRrqPkts=" << queueStats.p2pRrqTotalPackets
      << " p2pRrqPriPkts=" << queueStats.p2pRrqPriorityPackets
      << " p2pRrqSubPkts=" << queueStats.p2pRrqSubQueuePackets
      << " p2pRrqPriKB=" << BytesToKb(queueStats.p2pRrqPriorityBytes)
      << " p2pRrqSubKB=" << BytesToKb(queueStats.p2pRrqSubQueueBytes)
      << " p2pRrqApproxKB=" << BytesToKb(queueStats.p2pRrqApproxBytes)
      << " brInQ=" << queueStats.bridgeIngressQueues
      << " brInLocalQ=" << queueStats.bridgeIngressLocalQueues
      << " brInPkts=" << queueStats.bridgeIngressTotalPackets
      << " brInPriPkts=" << queueStats.bridgeIngressPriorityPackets
      << " brInSubPkts=" << queueStats.bridgeIngressSubQueuePackets
      << " brInPriKB=" << BytesToKb(queueStats.bridgeIngressPriorityBytes)
      << " brInSubKB=" << BytesToKb(queueStats.bridgeIngressSubQueueBytes)
      << " brInApproxKB=" << BytesToKb(queueStats.bridgeIngressApproxBytes);
  MemWatchLog(oss.str());
}

static sendflowclient::RuntimeTableStats CollectAggregateRuntimeTableStats() {
  sendflowclient::RuntimeTableStats totals;
  for (const auto& [nodeId, app] : nodeapptable) {
    if (app == nullptr || !IsNodeOwnedByCurrentRank(nodeId)) {
      continue;
    }
    const sendflowclient::RuntimeTableStats stats = app->GetRuntimeTableStats();
    totals.sendFuncEntries += stats.sendFuncEntries;
    totals.recvFuncEntries += stats.recvFuncEntries;
    totals.requestEntries += stats.requestEntries;
    totals.recvFuncFlagEntries += stats.recvFuncFlagEntries;
    totals.recvHashEntries += stats.recvHashEntries;
    totals.dataExpectedEntries += stats.dataExpectedEntries;
    totals.dataReceivedEntries += stats.dataReceivedEntries;
    totals.ackExpectedEntries += stats.ackExpectedEntries;
    totals.ackReceivedEntries += stats.ackReceivedEntries;
    totals.allDataSizeEntries += stats.allDataSizeEntries;
    totals.roceFlowToQpnEntries += stats.roceFlowToQpnEntries;
    totals.roceOutstandingQps += stats.roceOutstandingQps;
    totals.roceOutstandingPackets += stats.roceOutstandingPackets;
    totals.roceRetransTimerQps += stats.roceRetransTimerQps;
    totals.roceBufferedQps += stats.roceBufferedQps;
    totals.roceBufferedPackets += stats.roceBufferedPackets;
    totals.rocePendingRetransQps += stats.rocePendingRetransQps;
    totals.roceRecoveryTargetQps += stats.roceRecoveryTargetQps;
    totals.rocePendingWorkItems += stats.rocePendingWorkItems;
    totals.roceQpContextEntries += stats.roceQpContextEntries;
    totals.rocePendingRxPsnEntries += stats.rocePendingRxPsnEntries;
    totals.roceCompletionEntries += stats.roceCompletionEntries;
    totals.roceQpTxPackets += stats.roceQpTxPackets;
    totals.roceQpRxPackets += stats.roceQpRxPackets;
    totals.roceQpRxOutOfOrderPackets += stats.roceQpRxOutOfOrderPackets;
    totals.roceQpRxDuplicatePackets += stats.roceQpRxDuplicatePackets;
    totals.roceQpRxReorderOverflowPackets += stats.roceQpRxReorderOverflowPackets;
    totals.roceFsmStateEntries += stats.roceFsmStateEntries;
    totals.roceFsmCompletedEntries += stats.roceFsmCompletedEntries;
    totals.flowIoTxBufferedPackets += stats.flowIoTxBufferedPackets;
    totals.flowIoTxBufferedBytes += stats.flowIoTxBufferedBytes;
    totals.flowIoExternalIngressPackets += stats.flowIoExternalIngressPackets;
    totals.flowIoExternalIngressBytes += stats.flowIoExternalIngressBytes;
    totals.flowIoExternalHoldPackets += stats.flowIoExternalHoldPackets;
    totals.flowIoExternalHoldBytes += stats.flowIoExternalHoldBytes;
    totals.flowIoTotalBufferedPackets += stats.flowIoTotalBufferedPackets;
    totals.flowIoTotalBufferedBytes += stats.flowIoTotalBufferedBytes;
    totals.flowIoTotalBufferedApproxBytes += stats.flowIoTotalBufferedApproxBytes;
    totals.roceOutstandingPayloadBytes += stats.roceOutstandingPayloadBytes;
    totals.roceOutstandingApproxBytes += stats.roceOutstandingApproxBytes;
    totals.roceBufferedUpperPayloadBytes += stats.roceBufferedUpperPayloadBytes;
    totals.roceBufferedUpperApproxBytes += stats.roceBufferedUpperApproxBytes;
    totals.rocePendingRetransPayloadBytes += stats.rocePendingRetransPayloadBytes;
    totals.rocePendingRetransApproxBytes += stats.rocePendingRetransApproxBytes;
    totals.queuePayloadBytes += stats.queuePayloadBytes;
    totals.queueApproxBytes += stats.queueApproxBytes;
    totals.roceDispatchEventPending += stats.roceDispatchEventPending;
    totals.roceRetransTimerEventsPending += stats.roceRetransTimerEventsPending;
    totals.rocePendingEvents += stats.rocePendingEvents;
    totals.flowIoDrainEventPending += stats.flowIoDrainEventPending;
    totals.flowIoExternalDrainEventPending += stats.flowIoExternalDrainEventPending;
    totals.flowIoPendingEvents += stats.flowIoPendingEvents;
    totals.pendingEventCount += stats.pendingEventCount;
    totals.rocePendingEventApproxBytes += stats.rocePendingEventApproxBytes;
    totals.flowIoPendingEventApproxBytes += stats.flowIoPendingEventApproxBytes;
    totals.pendingEventApproxBytes += stats.pendingEventApproxBytes;
    totals.totalTrackedEntries += stats.totalTrackedEntries;
  }
  return totals;
}

static void LogAggregateRuntimeTableStats(
    const sendflowclient::RuntimeTableStats& stats,
    const sendflowclient::RuntimeTableStats* lastStats,
    const QueueMemStats& queueStats,
    const QueueMemStats* lastQueueStats) {
  const auto delta = [lastStats](uint64_t now,
                                 uint64_t sendflowclient::RuntimeTableStats::*field) -> int64_t {
    if (lastStats == nullptr) {
      return 0;
    }
    return static_cast<int64_t>(now) - static_cast<int64_t>(lastStats->*field);
  };
  const auto deltaKb = [&delta](uint64_t now,
                                uint64_t sendflowclient::RuntimeTableStats::*field) -> int64_t {
    return delta(now, field) / 1024;
  };
  const auto queueDelta = [lastQueueStats](uint64_t now,
                                           uint64_t QueueMemStats::*field) -> int64_t {
    if (lastQueueStats == nullptr) {
      return 0;
    }
    return static_cast<int64_t>(now) - static_cast<int64_t>(lastQueueStats->*field);
  };
  const auto queueDeltaKb = [&queueDelta](uint64_t now,
                                          uint64_t QueueMemStats::*field) -> int64_t {
    return queueDelta(now, field) / 1024;
  };

  std::ostringstream totalOss;
  totalOss << "[MemWatch][focus][total]"
           << " ioTotPkts=" << stats.flowIoTotalBufferedPackets
           << " ioTxPkts=" << stats.flowIoTxBufferedPackets
           << " ioExtIngressPkts=" << stats.flowIoExternalIngressPackets
           << " ioExtHoldPkts=" << stats.flowIoExternalHoldPackets
           << " ioTotBytes=" << stats.flowIoTotalBufferedBytes
           << " ioApproxKB=" << BytesToKb(stats.flowIoTotalBufferedApproxBytes)
           << " ioEvt=" << stats.flowIoPendingEvents
           << " ioEvtApproxKB=" << BytesToKb(stats.flowIoPendingEventApproxBytes)
           << " p2pRrqDev=" << queueStats.p2pRrqDevices
           << " p2pRrqPkts=" << queueStats.p2pRrqTotalPackets
           << " p2pRrqPriPkts=" << queueStats.p2pRrqPriorityPackets
           << " p2pRrqSubPkts=" << queueStats.p2pRrqSubQueuePackets
           << " p2pRrqPriKB=" << BytesToKb(queueStats.p2pRrqPriorityBytes)
           << " p2pRrqSubKB=" << BytesToKb(queueStats.p2pRrqSubQueueBytes)
           << " p2pRrqApproxKB=" << BytesToKb(queueStats.p2pRrqApproxBytes)
           << " brInQ=" << queueStats.bridgeIngressQueues
           << " brInLocalQ=" << queueStats.bridgeIngressLocalQueues
           << " brInPkts=" << queueStats.bridgeIngressTotalPackets
           << " brInPriPkts=" << queueStats.bridgeIngressPriorityPackets
           << " brInSubPkts=" << queueStats.bridgeIngressSubQueuePackets
           << " brInPriKB=" << BytesToKb(queueStats.bridgeIngressPriorityBytes)
           << " brInSubKB=" << BytesToKb(queueStats.bridgeIngressSubQueueBytes)
           << " brInApproxKB=" << BytesToKb(queueStats.bridgeIngressApproxBytes);
  MemWatchLog(totalOss.str());

  std::ostringstream deltaOss;
  deltaOss << "[MemWatch][focus][delta]"
           << " dIoTotPkts="
           << (delta(stats.flowIoTotalBufferedPackets,
                     &sendflowclient::RuntimeTableStats::flowIoTotalBufferedPackets) >= 0
                   ? "+"
                   : "")
           << delta(stats.flowIoTotalBufferedPackets,
                    &sendflowclient::RuntimeTableStats::flowIoTotalBufferedPackets)
           << " dIoTotBytes="
           << (delta(stats.flowIoTotalBufferedBytes,
                     &sendflowclient::RuntimeTableStats::flowIoTotalBufferedBytes) >= 0
                   ? "+"
                   : "")
           << delta(stats.flowIoTotalBufferedBytes,
                    &sendflowclient::RuntimeTableStats::flowIoTotalBufferedBytes)
           << " dIoApproxKB="
           << (deltaKb(stats.flowIoTotalBufferedApproxBytes,
                       &sendflowclient::RuntimeTableStats::flowIoTotalBufferedApproxBytes) >= 0
                   ? "+"
                   : "")
           << deltaKb(stats.flowIoTotalBufferedApproxBytes,
                      &sendflowclient::RuntimeTableStats::flowIoTotalBufferedApproxBytes)
           << " dIoEvt="
           << (delta(stats.flowIoPendingEvents,
                     &sendflowclient::RuntimeTableStats::flowIoPendingEvents) >= 0
                   ? "+"
                   : "")
           << delta(stats.flowIoPendingEvents,
                    &sendflowclient::RuntimeTableStats::flowIoPendingEvents)
           << " dP2pRrqPkts="
           << (queueDelta(queueStats.p2pRrqTotalPackets, &QueueMemStats::p2pRrqTotalPackets) >= 0
                   ? "+"
                   : "")
           << queueDelta(queueStats.p2pRrqTotalPackets, &QueueMemStats::p2pRrqTotalPackets)
           << " dP2pRrqPriPkts="
           << (queueDelta(queueStats.p2pRrqPriorityPackets, &QueueMemStats::p2pRrqPriorityPackets) >= 0
                   ? "+"
                   : "")
           << queueDelta(queueStats.p2pRrqPriorityPackets, &QueueMemStats::p2pRrqPriorityPackets)
           << " dP2pRrqSubPkts="
           << (queueDelta(queueStats.p2pRrqSubQueuePackets, &QueueMemStats::p2pRrqSubQueuePackets) >= 0
                   ? "+"
                   : "")
           << queueDelta(queueStats.p2pRrqSubQueuePackets, &QueueMemStats::p2pRrqSubQueuePackets)
           << " dP2pRrqPriKB="
           << (queueDeltaKb(queueStats.p2pRrqPriorityBytes, &QueueMemStats::p2pRrqPriorityBytes) >= 0
                   ? "+"
                   : "")
           << queueDeltaKb(queueStats.p2pRrqPriorityBytes, &QueueMemStats::p2pRrqPriorityBytes)
           << " dP2pRrqSubKB="
           << (queueDeltaKb(queueStats.p2pRrqSubQueueBytes, &QueueMemStats::p2pRrqSubQueueBytes) >= 0
                   ? "+"
                   : "")
           << queueDeltaKb(queueStats.p2pRrqSubQueueBytes, &QueueMemStats::p2pRrqSubQueueBytes)
           << " dP2pRrqApproxKB="
           << (queueDeltaKb(queueStats.p2pRrqApproxBytes, &QueueMemStats::p2pRrqApproxBytes) >= 0
                   ? "+"
                   : "")
           << queueDeltaKb(queueStats.p2pRrqApproxBytes, &QueueMemStats::p2pRrqApproxBytes)
           << " dBrInPkts="
           << (queueDelta(queueStats.bridgeIngressTotalPackets, &QueueMemStats::bridgeIngressTotalPackets) >= 0
                   ? "+"
                   : "")
           << queueDelta(queueStats.bridgeIngressTotalPackets, &QueueMemStats::bridgeIngressTotalPackets)
           << " dBrInPriPkts="
           << (queueDelta(queueStats.bridgeIngressPriorityPackets, &QueueMemStats::bridgeIngressPriorityPackets) >= 0
                   ? "+"
                   : "")
           << queueDelta(queueStats.bridgeIngressPriorityPackets, &QueueMemStats::bridgeIngressPriorityPackets)
           << " dBrInSubPkts="
           << (queueDelta(queueStats.bridgeIngressSubQueuePackets, &QueueMemStats::bridgeIngressSubQueuePackets) >= 0
                   ? "+"
                   : "")
           << queueDelta(queueStats.bridgeIngressSubQueuePackets, &QueueMemStats::bridgeIngressSubQueuePackets)
           << " dBrInPriKB="
           << (queueDeltaKb(queueStats.bridgeIngressPriorityBytes, &QueueMemStats::bridgeIngressPriorityBytes) >= 0
                   ? "+"
                   : "")
           << queueDeltaKb(queueStats.bridgeIngressPriorityBytes, &QueueMemStats::bridgeIngressPriorityBytes)
           << " dBrInSubKB="
           << (queueDeltaKb(queueStats.bridgeIngressSubQueueBytes, &QueueMemStats::bridgeIngressSubQueueBytes) >= 0
                   ? "+"
                   : "")
           << queueDeltaKb(queueStats.bridgeIngressSubQueueBytes, &QueueMemStats::bridgeIngressSubQueueBytes)
           << " dBrInApproxKB="
           << (queueDeltaKb(queueStats.bridgeIngressApproxBytes, &QueueMemStats::bridgeIngressApproxBytes) >= 0
                   ? "+"
                   : "")
           << queueDeltaKb(queueStats.bridgeIngressApproxBytes, &QueueMemStats::bridgeIngressApproxBytes);
  MemWatchLog(deltaOss.str());
}

static void MaybeLogTopNodesByFocusMemory(
    const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr || state->cfg.topNodes == 0) {
    return;
  }
  struct RankedNode {
    int nodeId = -1;
    sendflowclient::RuntimeTableStats stats;
    QueueMemStats queueStats;
    uint64_t focusBytes = 0;
  };

  std::vector<RankedNode> ranked;
  ranked.reserve(nodeapptable.size());
  for (const auto& [nodeId, app] : nodeapptable) {
    if (app == nullptr || !IsNodeOwnedByCurrentRank(nodeId)) {
      continue;
    }
    sendflowclient::RuntimeTableStats stats = app->GetRuntimeTableStats();
    QueueMemStats queueStats = CollectNodeQueueMemStats(nodeId);
    const uint64_t focusBytes =
        stats.flowIoTotalBufferedApproxBytes + queueStats.p2pRrqApproxBytes +
        queueStats.bridgeIngressApproxBytes;
    if (focusBytes == 0 && stats.flowIoTotalBufferedPackets == 0 &&
        queueStats.p2pRrqTotalPackets == 0 && queueStats.bridgeIngressTotalPackets == 0) {
      continue;
    }
    ranked.push_back(RankedNode{nodeId, stats, queueStats, focusBytes});
  }
  if (ranked.empty()) {
    return;
  }

  std::sort(ranked.begin(),
            ranked.end(),
            [](const RankedNode& a, const RankedNode& b) {
              return a.focusBytes > b.focusBytes;
            });
  const size_t maxCount = std::min(ranked.size(), static_cast<size_t>(state->cfg.topNodes));
  for (size_t i = 0; i < maxCount; ++i) {
    LogNodeMemWatchStats(
        ranked[i].nodeId,
        ranked[i].stats,
        "[MemWatch][topFocus" + std::to_string(i + 1) + "]");
  }

  std::vector<RankedNode> ioRanked = ranked;
  std::sort(ioRanked.begin(),
            ioRanked.end(),
            [](const RankedNode& a, const RankedNode& b) {
              return a.stats.flowIoTotalBufferedApproxBytes > b.stats.flowIoTotalBufferedApproxBytes;
            });
  if (!ioRanked.empty() && ioRanked.front().stats.flowIoTotalBufferedApproxBytes > 0) {
    for (size_t i = 0; i < maxCount; ++i) {
      LogNodeMemWatchStats(
          ioRanked[i].nodeId,
          ioRanked[i].stats,
          "[MemWatch][topFlowIo" + std::to_string(i + 1) + "]");
    }
  }

  std::vector<RankedNode> p2pRrqRanked = ranked;
  std::sort(p2pRrqRanked.begin(),
            p2pRrqRanked.end(),
            [](const RankedNode& a, const RankedNode& b) {
              return a.queueStats.p2pRrqApproxBytes > b.queueStats.p2pRrqApproxBytes;
            });
  if (!p2pRrqRanked.empty() && p2pRrqRanked.front().queueStats.p2pRrqApproxBytes > 0) {
    for (size_t i = 0; i < maxCount; ++i) {
      LogNodeMemWatchStats(
          p2pRrqRanked[i].nodeId,
          p2pRrqRanked[i].stats,
          "[MemWatch][topP2pRrq" + std::to_string(i + 1) + "]");
    }
  }

  std::vector<RankedNode> bridgeIngressRanked = ranked;
  std::sort(bridgeIngressRanked.begin(),
            bridgeIngressRanked.end(),
            [](const RankedNode& a, const RankedNode& b) {
              return a.queueStats.bridgeIngressApproxBytes > b.queueStats.bridgeIngressApproxBytes;
            });
  if (!bridgeIngressRanked.empty() &&
      bridgeIngressRanked.front().queueStats.bridgeIngressApproxBytes > 0) {
    for (size_t i = 0; i < maxCount; ++i) {
      LogNodeMemWatchStats(
          bridgeIngressRanked[i].nodeId,
          bridgeIngressRanked[i].stats,
          "[MemWatch][topBridgeIngress" + std::to_string(i + 1) + "]");
    }
  }
}

static void EmitMemWatchNodeDetails(const std::shared_ptr<MemWatchState>& state,
                                    const std::string& reasonTag) {
  if (state == nullptr || (!state->cfg.watchNodes.size() && state->cfg.topNodes == 0)) {
    return;
  }
  std::ostringstream oss;
  oss << "[MemWatch][node-snapshot]"
      << " rank=" << conf::rank
      << " sample=" << state->sampleIndex
      << " reason=" << reasonTag
      << " watchNodeCount=" << state->cfg.watchNodes.size()
      << " topNodes=" << state->cfg.topNodes;
  MemWatchLog(oss.str());

  for (int nodeId : state->cfg.watchNodes) {
    auto appIt = nodeapptable.find(nodeId);
    if (appIt == nodeapptable.end() || appIt->second == nullptr ||
        !IsNodeOwnedByCurrentRank(nodeId)) {
      continue;
    }
    LogNodeMemWatchStats(nodeId,
                         appIt->second->GetRuntimeTableStats(),
                         "[MemWatch][watch]");
  }
  MaybeLogTopNodesByFocusMemory(state);
}

static bool ShouldEmitPeriodicNodeDetails(const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr) {
    return false;
  }
  if (state->cfg.nodeDetailEverySamples == 0) {
    return false;
  }
  return (state->sampleIndex % state->cfg.nodeDetailEverySamples) == 0;
}

static void EmitMemWatchSample(const std::shared_ptr<MemWatchState>& state,
                               bool scheduleNext) {
  if (state == nullptr || !state->cfg.enable) {
    return;
  }

  state->sampleIndex++;
  const uint64_t eventCount = Simulator::GetEventCount();
  const ProcessMemSnapshot mem = ReadProcessMemSnapshot();
  const AllocatorMemSnapshot heap = ReadAllocatorMemSnapshot();

  const bool hadLast = state->hasLast;
  int64_t deltaEvents = 0;
  int64_t deltaRssKb = 0;
  int64_t deltaSwapKb = 0;
  if (hadLast) {
    deltaEvents = static_cast<int64_t>(eventCount) -
                  static_cast<int64_t>(state->lastEventCount);
    deltaRssKb = static_cast<int64_t>(mem.vmRssKb) -
                 static_cast<int64_t>(state->lastVmRssKb);
    deltaSwapKb = static_cast<int64_t>(mem.vmSwapKb) -
                  static_cast<int64_t>(state->lastVmSwapKb);
  }
  state->lastEventCount = eventCount;
  state->lastVmRssKb = mem.vmRssKb;
  state->lastVmSwapKb = mem.vmSwapKb;
  state->hasLast = true;

  std::ostringstream oss;
  oss << "[MemWatch][sample=" << state->sampleIndex << "]"
      << " rank=" << conf::rank
      << " simTime=" << Simulator::Now().GetSeconds() << "s"
      << " eventCount=" << eventCount
      << " dEvent=" << (deltaEvents >= 0 ? "+" : "") << deltaEvents
      << " VmRSS=" << mem.vmRssKb << "kB"
      << " dRSS=" << (deltaRssKb >= 0 ? "+" : "") << deltaRssKb << "kB"
      << " VmSwap=" << mem.vmSwapKb << "kB"
      << " dSwap=" << (deltaSwapKb >= 0 ? "+" : "") << deltaSwapKb << "kB"
      << " VmPeak=" << mem.vmPeakKb << "kB"
      << " VmSize=" << mem.vmSizeKb << "kB"
      << " VmData=" << mem.vmDataKb << "kB"
      << " RssAnon=" << mem.rssAnonKb << "kB"
      << " RssFile=" << mem.rssFileKb << "kB"
      << " RssShmem=" << mem.rssShmemKb << "kB"
      << " VmHWM=" << mem.vmHwmKb << "kB"
      << " Threads=" << mem.threads
      << " heapOK=" << heap.valid
      << " heapArenaKB=" << BytesToKb(heap.arenaBytes)
      << " heapMmapKB=" << BytesToKb(heap.mmapBytes)
      << " heapInUseKB=" << BytesToKb(heap.inUseBytes)
      << " heapFreeKB=" << BytesToKb(heap.freeBytes)
      << " heapKeepKB=" << BytesToKb(heap.releasableBytes)
      << " procStatusOK=" << mem.valid;
  MemWatchLog(oss.str());

  const sendflowclient::RuntimeTableStats aggStats = CollectAggregateRuntimeTableStats();
  const QueueMemStats queueStats = CollectAggregateQueueMemStats();
  sendflowclient::RuntimeTableStats prevAggStats;
  const sendflowclient::RuntimeTableStats* lastAggStatsPtr = nullptr;
  if (state->hasLastAggStats) {
    prevAggStats = state->lastAggStats;
    lastAggStatsPtr = &prevAggStats;
  }
  QueueMemStats prevQueueStats;
  const QueueMemStats* lastQueueStatsPtr = nullptr;
  if (state->hasLastQueueStats) {
    prevQueueStats = state->lastQueueStats;
    lastQueueStatsPtr = &prevQueueStats;
  }
  state->lastTrackedEntries = aggStats.totalTrackedEntries;
  LogAggregateRuntimeTableStats(aggStats, lastAggStatsPtr, queueStats, lastQueueStatsPtr);
  state->lastAggStats = aggStats;
  state->hasLastAggStats = true;
  state->lastQueueStats = queueStats;
  state->hasLastQueueStats = true;
  if (ShouldEmitPeriodicNodeDetails(state)) {
    EmitMemWatchNodeDetails(state, "periodic");
  }

  if (!scheduleNext) {
    return;
  }
  if (state->cfg.maxSamples > 0 && state->sampleIndex >= state->cfg.maxSamples) {
    std::ostringstream stopOss;
    stopOss << "[MemWatch] stop sampling: reached max samples="
            << state->cfg.maxSamples;
    MemWatchLog(stopOss.str());
    return;
  }
  Simulator::Schedule(Seconds(state->cfg.intervalSeconds),
                      &EmitMemWatchSample,
                      state,
                      true);
}

[[maybe_unused]] static void EmitMemWatchWallSample(
    const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr || !state->cfg.enable) {
    return;
  }
  if (state->cfg.maxSamples > 0 && state->sampleIndex >= state->cfg.maxSamples) {
    if (!state->maxSamplesLogged.exchange(true, std::memory_order_relaxed)) {
      std::ostringstream stopOss;
      stopOss << "[MemWatch] stop sampling: reached max samples="
              << state->cfg.maxSamples;
      MemWatchLog(stopOss.str());
    }
    state->stopRequested.store(true, std::memory_order_relaxed);
    return;
  }

  state->sampleIndex++;
  const ProcessMemSnapshot mem = ReadProcessMemSnapshot();
  const AllocatorMemSnapshot heap = ReadAllocatorMemSnapshot();
  const bool hadLast = state->hasLast;
  int64_t deltaRssKb = 0;
  int64_t deltaSwapKb = 0;
  if (hadLast) {
    deltaRssKb = static_cast<int64_t>(mem.vmRssKb) -
                 static_cast<int64_t>(state->lastVmRssKb);
    deltaSwapKb = static_cast<int64_t>(mem.vmSwapKb) -
                  static_cast<int64_t>(state->lastVmSwapKb);
  }
  state->lastVmRssKb = mem.vmRssKb;
  state->lastVmSwapKb = mem.vmSwapKb;
  state->hasLast = true;

  const uint64_t wallElapsedMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - state->wallStartTime)
          .count());
  std::ostringstream oss;
  oss << "[MemWatch][sample=" << state->sampleIndex << "]"
      << " rank=" << conf::rank
      << " wallTimeMs=" << wallElapsedMs
      << " VmRSS=" << mem.vmRssKb << "kB"
      << " dRSS=" << (deltaRssKb >= 0 ? "+" : "") << deltaRssKb << "kB"
      << " VmSwap=" << mem.vmSwapKb << "kB"
      << " dSwap=" << (deltaSwapKb >= 0 ? "+" : "") << deltaSwapKb << "kB"
      << " VmPeak=" << mem.vmPeakKb << "kB"
      << " VmSize=" << mem.vmSizeKb << "kB"
      << " VmData=" << mem.vmDataKb << "kB"
      << " RssAnon=" << mem.rssAnonKb << "kB"
      << " RssFile=" << mem.rssFileKb << "kB"
      << " RssShmem=" << mem.rssShmemKb << "kB"
      << " VmHWM=" << mem.vmHwmKb << "kB"
      << " Threads=" << mem.threads
      << " heapOK=" << heap.valid
      << " heapArenaKB=" << BytesToKb(heap.arenaBytes)
      << " heapMmapKB=" << BytesToKb(heap.mmapBytes)
      << " heapInUseKB=" << BytesToKb(heap.inUseBytes)
      << " heapFreeKB=" << BytesToKb(heap.freeBytes)
      << " heapKeepKB=" << BytesToKb(heap.releasableBytes)
      << " procStatusOK=" << mem.valid;
  MemWatchLog(oss.str());

  const sendflowclient::RuntimeTableStats aggStats = CollectAggregateRuntimeTableStats();
  const QueueMemStats queueStats = CollectAggregateQueueMemStats();
  sendflowclient::RuntimeTableStats prevAggStats;
  const sendflowclient::RuntimeTableStats* lastAggStatsPtr = nullptr;
  if (state->hasLastAggStats) {
    prevAggStats = state->lastAggStats;
    lastAggStatsPtr = &prevAggStats;
  }
  QueueMemStats prevQueueStats;
  const QueueMemStats* lastQueueStatsPtr = nullptr;
  if (state->hasLastQueueStats) {
    prevQueueStats = state->lastQueueStats;
    lastQueueStatsPtr = &prevQueueStats;
  }
  state->lastTrackedEntries = aggStats.totalTrackedEntries;
  LogAggregateRuntimeTableStats(aggStats, lastAggStatsPtr, queueStats, lastQueueStatsPtr);
  state->lastAggStats = aggStats;
  state->hasLastAggStats = true;
  state->lastQueueStats = queueStats;
  state->hasLastQueueStats = true;
  if (ShouldEmitPeriodicNodeDetails(state)) {
    EmitMemWatchNodeDetails(state, "periodic");
  }
}

static void EmitMemWatchWallMemOnlySample(const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr || !state->cfg.enable) {
    return;
  }
  if (state->cfg.maxSamples > 0 && state->sampleIndex >= state->cfg.maxSamples) {
    if (!state->maxSamplesLogged.exchange(true, std::memory_order_relaxed)) {
      std::ostringstream stopOss;
      stopOss << "[MemWatch] stop sampling: reached max samples="
              << state->cfg.maxSamples;
      MemWatchLog(stopOss.str());
    }
    state->stopRequested.store(true, std::memory_order_relaxed);
    return;
  }

  state->sampleIndex++;
  const ProcessMemSnapshot mem = ReadProcessMemSnapshot();
  const AllocatorMemSnapshot heap = ReadAllocatorMemSnapshot();
  const bool hadLast = state->hasLast;
  int64_t deltaRssKb = 0;
  int64_t deltaSwapKb = 0;
  if (hadLast) {
    deltaRssKb = static_cast<int64_t>(mem.vmRssKb) -
                 static_cast<int64_t>(state->lastVmRssKb);
    deltaSwapKb = static_cast<int64_t>(mem.vmSwapKb) -
                  static_cast<int64_t>(state->lastVmSwapKb);
  }
  state->lastVmRssKb = mem.vmRssKb;
  state->lastVmSwapKb = mem.vmSwapKb;
  state->hasLast = true;

  const uint64_t wallElapsedMs = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - state->wallStartTime)
          .count());
  std::ostringstream oss;
  oss << "[MemWatch][sample=" << state->sampleIndex << "]"
      << " rank=" << conf::rank
      << " wallTimeMs=" << wallElapsedMs
      << " VmRSS=" << mem.vmRssKb << "kB"
      << " dRSS=" << (deltaRssKb >= 0 ? "+" : "") << deltaRssKb << "kB"
      << " VmSwap=" << mem.vmSwapKb << "kB"
      << " dSwap=" << (deltaSwapKb >= 0 ? "+" : "") << deltaSwapKb << "kB"
      << " VmPeak=" << mem.vmPeakKb << "kB"
      << " VmSize=" << mem.vmSizeKb << "kB"
      << " VmData=" << mem.vmDataKb << "kB"
      << " RssAnon=" << mem.rssAnonKb << "kB"
      << " RssFile=" << mem.rssFileKb << "kB"
      << " RssShmem=" << mem.rssShmemKb << "kB"
      << " VmHWM=" << mem.vmHwmKb << "kB"
      << " Threads=" << mem.threads
      << " heapOK=" << heap.valid
      << " heapArenaKB=" << BytesToKb(heap.arenaBytes)
      << " heapMmapKB=" << BytesToKb(heap.mmapBytes)
      << " heapInUseKB=" << BytesToKb(heap.inUseBytes)
      << " heapFreeKB=" << BytesToKb(heap.freeBytes)
      << " heapKeepKB=" << BytesToKb(heap.releasableBytes)
      << " procStatusOK=" << mem.valid
      << " runtimeTables=skipped_mtp_wall";
  MemWatchLog(oss.str());
}

#ifndef NS3_MTP
static void RunMemWatchWallSampleEvent(const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr) {
    return;
  }
  state->wallSampleScheduled.store(false, std::memory_order_release);
  EmitMemWatchWallSample(state);
}

static void RequestMemWatchWallSample(const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr || !state->cfg.enable ||
      state->stopRequested.load(std::memory_order_relaxed)) {
    return;
  }
  bool expected = false;
  if (!state->wallSampleScheduled.compare_exchange_strong(expected,
                                                          true,
                                                          std::memory_order_acq_rel)) {
    return;
  }
  const uint32_t context = state->wallEventContext;
  if (context == ns3::Simulator::NO_CONTEXT ||
      context >= ns3::NodeList::GetNNodes()) {
    state->wallSampleScheduled.store(false, std::memory_order_release);
    if (!state->invalidContextLogged.exchange(true, std::memory_order_relaxed)) {
      std::ostringstream oss;
      oss << "[MemWatch] wall sampling disabled: invalid event context="
          << context
          << " nNodes=" << ns3::NodeList::GetNNodes();
      MemWatchLog(oss.str());
    }
    state->stopRequested.store(true, std::memory_order_relaxed);
    return;
  }
  ns3::Simulator::ScheduleWithContext(context,
                                      ns3::Seconds(0),
                                      &RunMemWatchWallSampleEvent,
                                      state);
}
#endif

static void StartMemWatchWallPeakSampler(const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr || !state->cfg.enable || !state->cfg.wallPeakEnable) {
    return;
  }
  state->wallPeakStopRequested.store(false, std::memory_order_relaxed);
  state->wallPeakStartTime = std::chrono::steady_clock::now();
  const auto interval = std::chrono::milliseconds(
      static_cast<int64_t>(std::max(1.0, state->cfg.wallPeakIntervalSeconds * 1000.0)));
  state->wallPeakSamplerThread = std::thread([state, interval]() {
    while (!state->wallPeakStopRequested.load(std::memory_order_relaxed)) {
      const ProcessMemSnapshot mem = ReadProcessMemSnapshot();
      const AllocatorMemSnapshot heap = ReadAllocatorMemSnapshot();
      const uint64_t wallElapsedMs = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - state->wallPeakStartTime)
              .count());

      bool shouldLog = false;
      bool isNewPeak = false;
      uint64_t sampleIndex = 0;
      uint64_t peakRssKb = 0;
      uint64_t peakAtMs = 0;
      {
        std::lock_guard<std::mutex> guard(state->wallPeakMutex);
        state->wallPeakSampleIndex++;
        sampleIndex = state->wallPeakSampleIndex;
        if (!state->hasWallPeak || mem.vmRssKb > state->wallPeakVmRssKb) {
          state->hasWallPeak = true;
          state->wallPeakAtMs = wallElapsedMs;
          state->wallPeakVmRssKb = mem.vmRssKb;
          state->wallPeakVmHwmKb = mem.vmHwmKb;
          state->wallPeakVmSizeKb = mem.vmSizeKb;
          state->wallPeakRssAnonKb = mem.rssAnonKb;
          state->wallPeakThreads = mem.threads;
          state->wallPeakHeapArenaKb = BytesToKb(heap.arenaBytes);
          state->wallPeakHeapMmapKb = BytesToKb(heap.mmapBytes);
          state->wallPeakHeapInUseKb = BytesToKb(heap.inUseBytes);
          state->wallPeakHeapFreeKb = BytesToKb(heap.freeBytes);
          isNewPeak = true;
          shouldLog = state->cfg.wallPeakLogOnNewPeak;
        } else if (state->cfg.wallPeakPeriodicEverySamples > 0 &&
                   (sampleIndex % state->cfg.wallPeakPeriodicEverySamples) == 0) {
          shouldLog = true;
        }
        peakRssKb = state->wallPeakVmRssKb;
        peakAtMs = state->wallPeakAtMs;
      }

      if (shouldLog) {
        std::ostringstream oss;
        oss << "[MemWatch][wall-peak]"
            << " rank=" << conf::rank
            << " sample=" << sampleIndex
            << " wallTimeMs=" << wallElapsedMs
            << " VmRSS=" << mem.vmRssKb << "kB"
            << " VmHWM=" << mem.vmHwmKb << "kB"
            << " VmPeak=" << mem.vmPeakKb << "kB"
            << " VmSize=" << mem.vmSizeKb << "kB"
            << " RssAnon=" << mem.rssAnonKb << "kB"
            << " Threads=" << mem.threads
            << " heapOK=" << heap.valid
            << " heapArenaKB=" << BytesToKb(heap.arenaBytes)
            << " heapMmapKB=" << BytesToKb(heap.mmapBytes)
            << " heapInUseKB=" << BytesToKb(heap.inUseBytes)
            << " heapFreeKB=" << BytesToKb(heap.freeBytes)
            << " peakVmRSS=" << peakRssKb << "kB"
            << " peakAtMs=" << peakAtMs
            << " reason=" << (isNewPeak ? "new_peak" : "periodic");
        MemWatchLog(oss.str());
      }

      std::this_thread::sleep_for(interval);
    }
  });
}

static void StopMemWatchWallPeakSampler(const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr || !state->cfg.wallPeakEnable) {
    return;
  }
  state->wallPeakStopRequested.store(true, std::memory_order_relaxed);
  if (state->wallPeakSamplerThread.joinable()) {
    state->wallPeakSamplerThread.join();
  }
  std::lock_guard<std::mutex> guard(state->wallPeakMutex);
  if (!state->hasWallPeak) {
    MemWatchLog("[MemWatch][wall-peak][summary] status=no_data");
    return;
  }
  std::ostringstream oss;
  oss << "[MemWatch][wall-peak][summary]"
      << " rank=" << conf::rank
      << " samples=" << state->wallPeakSampleIndex
      << " peakAtMs=" << state->wallPeakAtMs
      << " peakVmRSS=" << state->wallPeakVmRssKb << "kB"
      << " peakVmHWM=" << state->wallPeakVmHwmKb << "kB"
      << " peakVmSize=" << state->wallPeakVmSizeKb << "kB"
      << " peakRssAnon=" << state->wallPeakRssAnonKb << "kB"
      << " peakThreads=" << state->wallPeakThreads
      << " peakHeapArenaKB=" << state->wallPeakHeapArenaKb
      << " peakHeapMmapKB=" << state->wallPeakHeapMmapKb
      << " peakHeapInUseKB=" << state->wallPeakHeapInUseKb
      << " peakHeapFreeKB=" << state->wallPeakHeapFreeKb;
  MemWatchLog(oss.str());
}

static void StartMemWatchWallSampler(const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr || !state->cfg.enable) {
    return;
  }
  state->wallStartTime = std::chrono::steady_clock::now();
  const auto interval = std::chrono::milliseconds(
      static_cast<int64_t>(std::max(1.0, state->cfg.intervalSeconds * 1000.0)));
#ifndef NS3_MTP
  EmitMemWatchWallSample(state);
#endif
  state->wallSamplerThread = std::thread([state, interval]() {
#ifdef NS3_MTP
    EmitMemWatchWallMemOnlySample(state);
    while (!state->stopRequested.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(interval);
      if (state->stopRequested.load(std::memory_order_relaxed)) {
        break;
      }
      EmitMemWatchWallMemOnlySample(state);
    }
#else
    RequestMemWatchWallSample(state);
    while (!state->stopRequested.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(interval);
      if (state->stopRequested.load(std::memory_order_relaxed)) {
        break;
      }
      RequestMemWatchWallSample(state);
    }
#endif
  });
}

static void StopMemWatchWallSampler(const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr) {
    return;
  }
  state->stopRequested.store(true, std::memory_order_relaxed);
  if (state->wallSamplerThread.joinable()) {
    state->wallSamplerThread.join();
  }
}
*/

static int user_param_prase(int argc,char * argv[],struct user_param* user_param){
  int opt;
  while ((opt = getopt(argc,argv,"ht:w:g:s:n:c:"))!=-1){
    switch (opt)
    {
    case 'h':
      /* code */
      std::cout<<"-t    number of threads,default 1"<<std::endl;
      std::cout<<"-w    workloads default none "<<std::endl;
      std::cout<<"-n    network topo"<<std::endl;
      std::cout<<"-c    network_conf"<<std::endl;
      return 1;
      break;
    case 't':
      user_param->thread = stoi(optarg);
      break;
    case 'w':
      user_param->workload = optarg;
      break;
    case 'n':
      user_param->network_topo = optarg;
      break;
    case 'c':
      user_param->network_conf = optarg;
      break;
    default:
      std::cerr<<"-h    help message"<<std::endl;
      return 1;
    }
  }
  return 0 ;
}

void Initialize(int argc, char * argv[])
{
    #ifdef NS3_MPI
    std::string networkConfCliArg;
    CommandLine cmd;
    // parse mtp/mpi options
    cmd.AddValue("system", "Number of logical processes in MTP manual partition", conf::system);
    cmd.AddValue("nullmsg", "Enable null message algorithm", conf::nullmsg);
    // Keep custom config args visible to ns-3 CommandLine so -c/--network_conf
    // does not fail before user_param_prase() handles it.
    cmd.AddValue("network_conf", "Path to network config JSON", networkConfCliArg);
    cmd.AddValue("c", "Alias for network_conf", networkConfCliArg);
    cmd.Parse(argc, argv);

    MtpInterface::Enable(conf::thread);
    MpiInterface::Enable(&argc, &argv);
    conf::rank = MpiInterface::GetSystemId();
    conf::system = MpiInterface::GetSize();
    #endif
}

std::chrono::high_resolution_clock::time_point global_start, global_end;

void GetGlobalStart()
{
  global_start = std::chrono::high_resolution_clock::now();
}

/*
mpirun -np 2 --map-by ppr:2:node:PE=4 --bind-to core  \
  build/scratch/ns3.44-my_AstraSimNetwork-default

mpirun --hostfile /mnt/nfs/cloud/hybrid/hostfile.txt -np 4 \
  --wdir /mnt/nfs/cloud/hybrid \
  --launch-agent "/usr/local/bin/mpi-docker-agent /usr/bin/orted" \
  --map-by ppr:2:node:PE=16 \
  --bind-to core \
  --mca btl_tcp_if_include 10.201.0.0/16 \
  /mnt/nfs/cloud/hybrid/build/scratch/ns3.44-my_AstraSimNetwork-default
*/

int main(int argc, char *argv[]) {
  const std::string startup_config = ResolveNetworkConfigPathFromArgv(argc, argv);
  conf::thread = ResolveThreadFromConfig(startup_config, conf::thread);

  #ifdef NS3_MPI
  Initialize(argc, argv);
  #elif NS3_MTP
  MtpInterface::Enable(conf::thread);
  #endif

  struct user_param user_param;
  // MockNcclLog::set_log_name("SimAI.log");
  // MockNcclLog* NcclLog = MockNcclLog::getInstance();
  // NcclLog->writeLog(NcclLogLevel::INFO," init SimAI.log ");
  if(user_param_prase(argc,argv,&user_param)){
    return 0;
  }
  
  network_config = user_param.network_conf.empty() ? "simconfig/config.json"
                                                   : user_param.network_conf;
  // const MemWatchConfig memWatchCfg = ResolveMemWatchConfigFromConfig(network_config);
  // Enable SendFlowClient logs before network/app construction so [ROCE][CFG]
  // emitted during SetRoceConfig is visible in simulation.log.
  // LogComponentEnable("SendFlowClient",LOG_LEVEL_INFO);
  // AsciiTraceHelper ascii;
  // std::string simulationLogPath = "simulation.log";
  // #ifdef NS3_MPI
  // if (conf::rank != 0) {
  //   simulationLogPath = "simulation.rank" + std::to_string(conf::rank) + ".log";
  // }
  // #endif
  // Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream(simulationLogPath);
  // std::streambuf* oldbuf = std::clog.rdbuf(stream->GetStream()->rdbuf());
  // const std::ios_base::fmtflags oldClogFlags = std::clog.flags();
  // std::clog << std::unitbuf;

  AstraSim::Workload::SetDefaultPPScheduleByString(
      ResolvePpScheduleFromConfig(network_config));
  main1(network_config);
  if (mactable_load_enable)
  {
    if (!LoadBridgeMacTablesFromDirectory(mactable_load_dir))
    {
      std::cerr << "[MACTABLE] load failed from directory: " << mactable_load_dir
                << ", aborting to avoid running with incomplete preloaded state.\n";
      return 1;
    }
  }
  const int warmupSeconds = GetEffectiveFloodTime();
  int gpu_num = node_num - nvswitch_num - switch_num;
  int nodes_num = gpu_num + nvswitch_num;

  int workload_all_gpus = ParseAllGpusFromWorkloadHeader(network_workload);
  if (workload_all_gpus > 0 && workload_all_gpus != gpu_num) {
    std::cerr << "[ConfigMismatch] workload all_gpus=" << workload_all_gpus
              << " but topology GPUs=" << gpu_num << ".\n";
    std::cerr << "[Hint] For FatTree k=4: GPUs = 16 * gpus_per_server.\n";
    return 1;
  }

  NVswitchs.clear();
  std::map<int, int> node2nvswitch; 
  for(int i = 0; i < gpu_num; ++ i) {
    node2nvswitch[i] = gpu_num + i;
  }
  for(int i = gpu_num; i < gpu_num*2 ; ++ i){
    node2nvswitch[i] = i;
    NVswitchs.push_back(i);
  }

  // LogComponentEnable("OnOffApplication", LOG_LEVEL_INFO);
  // LogComponentEnable("PacketSink", LOG_LEVEL_INFO);
  // LogComponentEnable("GENERIC_SIMULATION", LOG_LEVEL_ERROR);
  // LogComponentEnable("MinHopBridgeNetDevice", LOG_LEVEL_INFO);
  // LogComponentEnable("PointToPointNetDevice", LOG_LEVEL_INFO);
  // LogComponentEnable("PppRoundRobinQueue", LOG_LEVEL_INFO);
  // LogComponentEnable("HybridSimulatorImpl",LOG_LEVEL_INFO);
  // LogComponentEnable("MtpInterface",LOG_LEVEL_INFO);
  // LogComponentEnable("GrantedTimeWindowMpiInterface",LOG_LEVEL_INFO);
  // LogComponentEnable("LogicalProcess", LOG_LEVEL_INFO);

  std::vector<ASTRASimNetwork *> networks(nodes_num, nullptr);
  std::vector<AstraSim::Sys *> systems(nodes_num, nullptr);
  std::vector<AstraSim::OverlayCollectiveInjector *> overlayInjectors(
      nodes_num, nullptr);

  // Overlay bypass parameters (adjust only these fields).
  AstraSim::OverlayCollectiveInjector::Config overlayCfg;
  overlayCfg.enable = false;
  overlayCfg.start_delay_ticks = 0;
  overlayCfg.collective = AstraSim::ComType::All_to_All;
  overlayCfg.mock_nccl_group = MockNccl::GroupType::DP;
  overlayCfg.bytes = 65534ULL;
  overlayCfg.scheduling = AstraSim::SchedulingPolicy::FIFO;
  overlayCfg.layer = 1000;
  overlayCfg.name = "overlay_a2a";
  // Leave empty to involve all physical dimensions by default.
  // overlayCfg.involved_dims = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const int queuePerDim = overlayCfg.enable ? 2 : 1;

  for (int j = 0; j < gpu_num; j++) {
    if(nodetable[j]->GetSystemId() == conf::rank)
    {
      // std::cout<<"[main] node="<<nodetable[j]->GetId()<<" set sys in rank="<<nodetable[j]->GetSystemId()<<"\n";
      networks[j] =
          new ASTRASimNetwork(j ,0);
      systems[j ] = new AstraSim::Sys(
          networks[j], 
          nullptr,                  
          j,                        
          0,               
          1,                        
          {nodes_num},      
          {queuePerDim},
          "", 
          network_workload,
          1, 
          1,          
          1,          
          1,
          0,                 
          RESULT_PATH, 
          "test1",            
          true,               
          true,               
          gpu_type,
          {gpu_num},
          NVswitchs,
          1
      );
      systems[j ]->nvswitch_id = node2nvswitch[j];
      systems[j ]->num_gpus = nodes_num - nvswitch_num;
      if (overlayCfg.enable) {
        overlayInjectors[j] =
            new AstraSim::OverlayCollectiveInjector(systems[j], overlayCfg);
      }
    }
  }

  ns3::Simulator::Schedule(
      ns3::Seconds(warmupSeconds),
      [gpu_num, systems, overlayInjectors]() {
      for (int i = 0; i < gpu_num; i++) {
        if(systems[i] != nullptr)
        {
          systems[i]->workload->fire();
          if (overlayInjectors[i] != nullptr) {
            overlayInjectors[i]->Arm();
          }
        }
      }
  });

  if (!mactable_load_enable)
  {
    ns3::Time dumpTime = ns3::Seconds(warmupSeconds) - ns3::NanoSeconds(1);
    if (dumpTime < ns3::Seconds(0))
    {
      dumpTime = ns3::Seconds(0);
    }
    ns3::Simulator::Schedule(dumpTime, []() {
      DumpBridgeMacTablesToDirectory(mactable_load_dir);
    });
  }

  #ifdef NS3_MPI
  if(MpiInterface::GetSystemId() == 0){
    ns3::Simulator::Schedule(ns3::Seconds(warmupSeconds-1),[](){
      GetGlobalStart();
    });    
  }
  #elif NS3_MTP
  ns3::Simulator::Schedule(ns3::Seconds(warmupSeconds-1),[](){
    GetGlobalStart();
  });  
  #endif

  // std::shared_ptr<MemWatchState> memWatchState;
  // if (memWatchCfg.enable) {
  //   memWatchState = std::make_shared<MemWatchState>();
  //   memWatchState->cfg = memWatchCfg;
  //   memWatchState->wallEventContext = ResolveMemWatchEventContext(memWatchState->cfg);
  //   const double monitorStartSeconds =
  //       std::max(0.0, static_cast<double>(warmupSeconds) - 1.0);
  //   std::ostringstream oss;
  //   oss << "[MemWatch] enabled=1"
  //       << " rank=" << conf::rank
  //       << " timeMode=" << (memWatchState->cfg.useWallClock ? "wall" : "sim")
  //       << " startTime=" << monitorStartSeconds << "s"
  //       << " interval=" << memWatchState->cfg.intervalSeconds << "s"
  //       << " maxSamples=" << memWatchState->cfg.maxSamples
  //       << " nodeDetailEverySamples=" << memWatchState->cfg.nodeDetailEverySamples
  //       << " wallEventContext=" << memWatchState->wallEventContext
  //       << " watchNodeCount=" << memWatchState->cfg.watchNodes.size()
  //       << " topNodes=" << memWatchState->cfg.topNodes
  //       << " wallPeakEnable=" << memWatchState->cfg.wallPeakEnable
  //       << " wallPeakInterval=" << memWatchState->cfg.wallPeakIntervalSeconds << "s"
  //       << " wallPeakPeriodicEverySamples="
  //       << memWatchState->cfg.wallPeakPeriodicEverySamples;
  //   MemWatchLog(oss.str());
  // #ifdef NS3_MTP
  //   if (memWatchState->cfg.useWallClock) {
  //     MemWatchLog("[MemWatch] NS3_MTP + wall mode: runtime table/node details are disabled for thread safety; use sim mode for detailed queue/event analysis.");
  //   }
  // #endif
  //   if (memWatchState->cfg.useWallClock) {
  //     ns3::Simulator::Schedule(ns3::Seconds(monitorStartSeconds),
  //                              &StartMemWatchWallSampler,
  //                              memWatchState);
  //   } else {
  //     ns3::Simulator::Schedule(ns3::Seconds(monitorStartSeconds),
  //                              &EmitMemWatchSample,
  //                              memWatchState,
  //                              true);
  //   }
  //   if (memWatchState->cfg.wallPeakEnable) {
  //     ns3::Simulator::Schedule(ns3::Seconds(monitorStartSeconds),
  //                              &StartMemWatchWallPeakSampler,
  //                              memWatchState);
  //   }
  // }
  // ns3::Simulator::Schedule(ns3::Seconds(190), [nodes_num,systems](){
  //     printlearnstate();
  // });

  // std::cout << "simulator run " << std::endl;
  Simulator::Stop(Seconds(2000000000));
  Simulator::Run(); 

  // if (memWatchState != nullptr && memWatchState->cfg.enable) {
  //   if (memWatchState->cfg.wallPeakEnable) {
  //     StopMemWatchWallPeakSampler(memWatchState);
  //   }
  //   if (memWatchState->cfg.useWallClock) {
  //     StopMemWatchWallSampler(memWatchState);
  // #ifdef NS3_MTP
  //     std::ostringstream endOss;
  //     endOss << "[MemWatch] wall sampler stopped after Run; skip direct final sample on main thread in MTP mode";
  //     MemWatchLog(endOss.str());
  // #else
  //     EmitMemWatchWallSample(memWatchState);
  //     const sendflowclient::RuntimeTableStats finalAggStats =
  //         CollectAggregateRuntimeTableStats();
  //     const QueueMemStats finalQueueStats = CollectAggregateQueueMemStats();
  //     sendflowclient::RuntimeTableStats prevAggStats;
  //     const sendflowclient::RuntimeTableStats* lastAggStatsPtr = nullptr;
  //     if (memWatchState->hasLastAggStats) {
  //       prevAggStats = memWatchState->lastAggStats;
  //       lastAggStatsPtr = &prevAggStats;
  //     }
  //     QueueMemStats prevQueueStats;
  //     const QueueMemStats* lastQueueStatsPtr = nullptr;
  //     if (memWatchState->hasLastQueueStats) {
  //       prevQueueStats = memWatchState->lastQueueStats;
  //       lastQueueStatsPtr = &prevQueueStats;
  //     }
  //     memWatchState->lastTrackedEntries = finalAggStats.totalTrackedEntries;
  //     LogAggregateRuntimeTableStats(finalAggStats,
  //                                   lastAggStatsPtr,
  //                                   finalQueueStats,
  //                                   lastQueueStatsPtr);
  //     memWatchState->lastAggStats = finalAggStats;
  //     memWatchState->hasLastAggStats = true;
  //     memWatchState->lastQueueStats = finalQueueStats;
  //     memWatchState->hasLastQueueStats = true;
  //     EmitMemWatchNodeDetails(memWatchState, "final");
  // #endif
  //   } else {
  //     EmitMemWatchSample(memWatchState, false);
  //   }
  // }

  uint64_t eventCount = Simulator::GetEventCount();
  // NS_LOG_INFO(" * * * total events : " << eventCount );
  // NS_LOG_INFO("[Rendezvous]: recv="<<totRendezvousRecv<<" send="<<totRendezvousSend);

  #ifdef NS3_MPI
  uint64_t eventCounts[mx_sys], sendCounts[mx_sys];
  MPI_Gather(&eventCount,
          1,
          MPI_UNSIGNED_LONG_LONG,
          eventCounts,
          1,
          MPI_UNSIGNED_LONG_LONG,
          0,
          MpiInterface::GetCommunicator());
  MPI_Gather(&totRendezvousSend,
          1,
          MPI_UNSIGNED_LONG_LONG,
          sendCounts,
          1,
          MPI_UNSIGNED_LONG_LONG,
          0,
          MpiInterface::GetCommunicator());

  if(MpiInterface::GetSystemId() == 0){
    // NS_LOG_INFO("[main] total send count="<<std::accumulate(sendCounts, sendCounts + conf::system, 0ULL));
    // NS_LOG_INFO("[main] total events="<<std::accumulate(eventCounts, eventCounts + conf::system, 0ULL));
  }
  #endif
  (void)eventCount;

  global_end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(global_end - global_start);
  (void)duration;
  #ifdef NS3_MPI
  if(MpiInterface::GetSystemId() == 0)
  {
    std::cout<<"total runtime="<<duration.count()<<"ms"<<"\n";
  }
  #elif NS3_MTP
  std::cout<<"total runtime="<<duration.count()<<"ms"<<"\n";
  #endif

  Simulator::Destroy();
  for (auto*& injector : overlayInjectors) {
    if (injector != nullptr) {
      delete injector;
      injector = nullptr;
    }
  }
  #ifdef NS3_MPI
  MpiInterface::Disable();
  #elif NS3_MTP
  // NS_LOG_INFO("[main] total send count="<<totRendezvousSend);
  // NS_LOG_INFO("[main] total events="<<eventCount);
  #endif
  
  // std::clog.flush();
  // std::clog.flags(oldClogFlags);
  // std::clog.rdbuf(oldbuf);   // 恢复到原始 buffer（非常关键）
  // stream = nullptr;          // 释放 OutputStreamWrapper（可选，确保先恢复再销毁）
  
  return 0;
}
