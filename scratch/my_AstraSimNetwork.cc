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
#include <unistd.h>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
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
  uint32_t thread = 1;
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
        std::cout << "sim_finish on sent, " << " Thread id: " << pthread_self() << std::endl;
        cout << "All data sent from node " << p.first << " is " << it->second
             << "\n";
      } else {
        std::cout << "sim_finish on received, " << " Thread id: " << pthread_self() << std::endl;
        cout << "All data received by node " << p.first << " is " << it->second
             << "\n";
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
    MockNcclLog* NcclLog = MockNcclLog::getInstance();
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

struct ProcessMemSnapshot {
  uint64_t vmRssKb = 0;
  uint64_t vmSizeKb = 0;
  uint64_t vmHwmKb = 0;
  uint64_t vmDataKb = 0;
  bool valid = false;
};

struct MemWatchConfig {
  bool enable = false;
  double intervalSeconds = 100.0;
  uint32_t maxSamples = 120;
  uint32_t topNodes = 0;
  std::vector<int> watchNodes{0};
};

struct MemWatchState {
  MemWatchConfig cfg;
  uint32_t sampleIndex = 0;
  uint64_t lastEventCount = 0;
  uint64_t lastVmRssKb = 0;
  bool hasLast = false;
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
    ParseStatusValueKb(line, "VmSize:", &snapshot.vmSizeKb);
    ParseStatusValueKb(line, "VmHWM:", &snapshot.vmHwmKb);
    ParseStatusValueKb(line, "VmData:", &snapshot.vmDataKb);
  }
  snapshot.valid = true;
  return snapshot;
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
  if (debug.contains("mem_log_interval_s") && debug["mem_log_interval_s"].is_number()) {
    const double interval = debug["mem_log_interval_s"].get<double>();
    if (interval > 0) {
      cfg.intervalSeconds = interval;
    }
  }
  if (debug.contains("mem_log_max_samples") && debug["mem_log_max_samples"].is_number_integer()) {
    const int samples = debug["mem_log_max_samples"].get<int>();
    if (samples > 0) {
      cfg.maxSamples = static_cast<uint32_t>(samples);
    }
  }
  if (debug.contains("mem_log_top_nodes") && debug["mem_log_top_nodes"].is_number_integer()) {
    const int topNodes = debug["mem_log_top_nodes"].get<int>();
    if (topNodes >= 0) {
      cfg.topNodes = static_cast<uint32_t>(topNodes);
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

static void LogNodeMemWatchStats(
    int nodeId,
    const sendflowclient::RuntimeTableStats& stats,
    const std::string& prefix = "[MemWatch][node]") {
  std::ostringstream oss;
  oss << prefix
      << " node=" << nodeId
      << " tracked=" << stats.totalTrackedEntries
      << " sendCb=" << stats.sendFuncEntries
      << " recvCb=" << stats.recvFuncEntries
      << " req=" << stats.requestEntries
      << " recvHash=" << stats.recvHashEntries
      << " dataExp=" << stats.dataExpectedEntries
      << " ackExp=" << stats.ackExpectedEntries
      << " qpn=" << stats.roceFlowToQpnEntries
      << " roceOutPkts=" << stats.roceOutstandingPackets
      << " roceBufPkts=" << stats.roceBufferedPackets
      << " rocePendingWork=" << stats.rocePendingWorkItems
      << " roceRtxQps=" << stats.rocePendingRetransQps;
  std::clog << oss.str() << '\n';
}

static void MaybeLogTopNodesByTrackedEntries(
    const std::shared_ptr<MemWatchState>& state) {
  if (state == nullptr || state->cfg.topNodes == 0) {
    return;
  }
  struct RankedNode {
    int nodeId = -1;
    sendflowclient::RuntimeTableStats stats;
  };

  std::vector<RankedNode> ranked;
  ranked.reserve(nodeapptable.size());
  for (const auto& [nodeId, app] : nodeapptable) {
    if (app == nullptr || !IsNodeOwnedByCurrentRank(nodeId)) {
      continue;
    }
    sendflowclient::RuntimeTableStats stats = app->GetRuntimeTableStats();
    if (stats.totalTrackedEntries == 0) {
      continue;
    }
    ranked.push_back(RankedNode{nodeId, stats});
  }
  if (ranked.empty()) {
    return;
  }

  std::sort(ranked.begin(),
            ranked.end(),
            [](const RankedNode& a, const RankedNode& b) {
              return a.stats.totalTrackedEntries > b.stats.totalTrackedEntries;
            });
  const size_t maxCount = std::min(ranked.size(), static_cast<size_t>(state->cfg.topNodes));
  for (size_t i = 0; i < maxCount; ++i) {
    LogNodeMemWatchStats(
        ranked[i].nodeId,
        ranked[i].stats,
        "[MemWatch][top" + std::to_string(i + 1) + "]");
  }
}

static void EmitMemWatchSample(const std::shared_ptr<MemWatchState>& state,
                               bool scheduleNext) {
  if (state == nullptr || !state->cfg.enable) {
    return;
  }

  state->sampleIndex++;
  const uint64_t eventCount = Simulator::GetEventCount();
  const ProcessMemSnapshot mem = ReadProcessMemSnapshot();

  int64_t deltaEvents = 0;
  int64_t deltaRssKb = 0;
  if (state->hasLast) {
    deltaEvents = static_cast<int64_t>(eventCount) -
                  static_cast<int64_t>(state->lastEventCount);
    deltaRssKb = static_cast<int64_t>(mem.vmRssKb) -
                 static_cast<int64_t>(state->lastVmRssKb);
  }
  state->lastEventCount = eventCount;
  state->lastVmRssKb = mem.vmRssKb;
  state->hasLast = true;

  std::clog << "[MemWatch][sample=" << state->sampleIndex << "]"
            << " rank=" << conf::rank
            << " simTime=" << Simulator::Now().GetSeconds() << "s"
            << " eventCount=" << eventCount
            << " dEvent=" << (deltaEvents >= 0 ? "+" : "") << deltaEvents
            << " VmRSS=" << mem.vmRssKb << "kB"
            << " dRSS=" << (deltaRssKb >= 0 ? "+" : "") << deltaRssKb << "kB"
            << " VmSize=" << mem.vmSizeKb << "kB"
            << " VmData=" << mem.vmDataKb << "kB"
            << " VmHWM=" << mem.vmHwmKb << "kB"
            << " procStatusOK=" << mem.valid
            << '\n';

  for (int nodeId : state->cfg.watchNodes) {
    auto appIt = nodeapptable.find(nodeId);
    if (appIt == nodeapptable.end() || appIt->second == nullptr ||
        !IsNodeOwnedByCurrentRank(nodeId)) {
      continue;
    }
    LogNodeMemWatchStats(nodeId, appIt->second->GetRuntimeTableStats());
  }
  MaybeLogTopNodesByTrackedEntries(state);
  std::clog.flush();

  if (!scheduleNext) {
    return;
  }
  if (state->sampleIndex >= state->cfg.maxSamples) {
    std::clog << "[MemWatch] stop sampling: reached max samples="
              << state->cfg.maxSamples << '\n';
    std::clog.flush();
    return;
  }
  Simulator::Schedule(Seconds(state->cfg.intervalSeconds),
                      &EmitMemWatchSample,
                      state,
                      true);
}

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
    CommandLine cmd;
    // parse mtp/mpi options
    cmd.AddValue("system", "Number of logical processes in MTP manual partition", conf::system);
    cmd.AddValue("nullmsg", "Enable null message algorithm", conf::nullmsg);
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

int main(int argc, char *argv[]) {

  #ifdef NS3_MPI
  Initialize(argc, argv);
  #elif NS3_MTP
  MtpInterface::Enable(conf::thread);
  #endif

  struct user_param user_param;
  MockNcclLog::set_log_name("SimAI.log");
  MockNcclLog* NcclLog = MockNcclLog::getInstance();
  NcclLog->writeLog(NcclLogLevel::INFO," init SimAI.log ");
  if(user_param_prase(argc,argv,&user_param)){
    return 0;
  }
  
  network_config = user_param.network_conf.empty() ? "simconfig/config.json"
                                                   : user_param.network_conf;
  const MemWatchConfig memWatchCfg = ResolveMemWatchConfigFromConfig(network_config);
  // Enable SendFlowClient logs before network/app construction so [ROCE][CFG]
  // emitted during SetRoceConfig is visible in simulation.log.
  // LogComponentEnable("SendFlowClient",LOG_LEVEL_INFO);
  AsciiTraceHelper ascii;
  Ptr<OutputStreamWrapper> stream = ascii.CreateFileStream("simulation.log");
  std::streambuf* oldbuf = std::clog.rdbuf(stream->GetStream()->rdbuf());
  const std::ios_base::fmtflags oldClogFlags = std::clog.flags();
  std::clog << std::unitbuf;

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
  LogComponentEnable("GENERIC_SIMULATION", LOG_LEVEL_INFO);
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

  std::shared_ptr<MemWatchState> memWatchState;
  if (memWatchCfg.enable) {
    memWatchState = std::make_shared<MemWatchState>();
    memWatchState->cfg = memWatchCfg;
    const double monitorStartSeconds =
        std::max(0.0, static_cast<double>(warmupSeconds) - 1.0);
    std::clog << "[MemWatch] enabled=1"
              << " rank=" << conf::rank
              << " startTime=" << monitorStartSeconds << "s"
              << " interval=" << memWatchState->cfg.intervalSeconds << "s"
              << " maxSamples=" << memWatchState->cfg.maxSamples
              << " watchNodeCount=" << memWatchState->cfg.watchNodes.size()
              << " topNodes=" << memWatchState->cfg.topNodes
              << '\n';
    ns3::Simulator::Schedule(ns3::Seconds(monitorStartSeconds),
                             &EmitMemWatchSample,
                             memWatchState,
                             true);
  }
  // ns3::Simulator::Schedule(ns3::Seconds(190), [nodes_num,systems](){
  //     printlearnstate();
  // });

  std::cout << "simulator run " << std::endl;
  Simulator::Stop(Seconds(2000000000));
  Simulator::Run(); 

  if (memWatchState != nullptr && memWatchState->cfg.enable) {
    EmitMemWatchSample(memWatchState, false);
  }

  uint64_t eventCount = Simulator::GetEventCount();
  // NS_LOG_INFO(" * * * total events : " << eventCount );
  // NS_LOG_INFO("[Rendezvous]: recv="<<totRendezvousRecv<<" send="<<totRendezvousSend);

  uint64_t eventCounts[mx_sys], sendCounts[mx_sys];
  #ifdef NS3_MPI
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
    NS_LOG_INFO("[main] total send count="<<std::accumulate(sendCounts, sendCounts + conf::system, 0ULL));
    NS_LOG_INFO("[main] total events="<<std::accumulate(eventCounts, eventCounts + conf::system, 0ULL));
  }
  #endif

  global_end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(global_end - global_start);
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
  NS_LOG_INFO("[main] total send count="<<totRendezvousSend);
  NS_LOG_INFO("[main] total events="<<eventCount);
  #endif
  
  std::clog.flush();
  std::clog.flags(oldClogFlags);
  std::clog.rdbuf(oldbuf);   // 恢复到原始 buffer（非常关键）
  stream = nullptr;          // 释放 OutputStreamWrapper（可选，确保先恢复再销毁）
  
  return 0;
}
