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

#ifndef __COMMON_H__
#define __COMMON_H__

#undef PGO_TRAINING
#define PATH_TO_PGO_CONFIG "path_to_pgo_config"

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/packet-socket-helper.h"
#include "ns3/mac48-address.h"
#include <vector>
#include "ns3/mobility-module.h"     // 用于设置节点位置
#include "ns3/point-to-point-module.h"

// 包含我们自定义的头文件
#include "ns3/hop-count-header.h"
#include "ns3/hop-count-client.h"
#include "ns3/min-hop-bridge-net-device.h"
#include "ns3/pause-header.h"
#include "ns3/three-phase-protocol.h"
#include "ns3/mtp-module.h"
#include "ns3/my_sendflowclient.h"
#ifdef NS3_MPI
#include "ns3/mpi-interface.h"
#endif

#include <mutex>
#include <map>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <cmath>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <nlohmann/json.hpp>

using namespace ns3;
using namespace std;

NS_LOG_COMPONENT_DEFINE("GENERIC_SIMULATION");

GPUType gpu_type;
int ecmpmodel,max_jump,floadtime,m_mtu;
int effective_floadtime;
bool mactable_load_enable;
std::string mactable_load_dir;
double pfc_high_watermark;
double pfc_low_watermark;
uint32_t node_num, switch_num, link_num, trace_num, nvswitch_num, gpus_per_server,
            nvswitch_per_server,numGPUs;
uint32_t Topology_model,subnet_model,subnet_k,numHosts,switch_delay;
string normallink_rate,nvlink_rate,normallink_delay,nvlink_delay,DropTailQueue_size,DropTailQueue_num;
uint32_t k_fattree,numCores,numAggregations,numEdges,k_all;
uint32_t k_clos,numSpine,numLeaf,hostperleaf;
uint32_t k_torus,torus_x,torus_y,num_torusswitch;
uint32_t k_mesh,num_meshswitch;
uint32_t k_test,num_testswitch;
uint32_t m_thread;
string network_workload;

std::vector<int>NVswitchs;

std::map<int,PacketSocketAddress> nodeaddresstable;
std::map<int,Ptr<sendflowclient> > nodeapptable;
std::map<int,Ptr<Node> > nodetable;
RoceConfig g_roce_cfg;

std::vector<Ptr<MinHopBridgeNetDevice>> coreDevices;
std::vector<Ptr<MinHopBridgeNetDevice>> aggregationDevices; 
std::vector<Ptr<MinHopBridgeNetDevice>> edgeDevices;
std::vector<Ptr<MinHopBridgeNetDevice>> nvswitchDevices;

std::vector<Ptr<MinHopBridgeNetDevice>> SpineDevices;
std::vector<Ptr<MinHopBridgeNetDevice>> LeafDevices;

std::vector<Ptr<MinHopBridgeNetDevice>> TorusDevices;

std::vector<Ptr<MinHopBridgeNetDevice>> FullmeshDevices;

std::vector<Ptr<MinHopBridgeNetDevice>> TestSwitchDevices;

std::vector<Ptr<MinHopBridgeNetDevice>> SubNetDevices;

static inline void
ApplyBridgePfcWatermarks(const Ptr<MinHopBridgeNetDevice>& dev)
{
    dev->SetAttribute("PfcHighWatermark", DoubleValue(pfc_high_watermark));
    dev->SetAttribute("PfcLowWatermark", DoubleValue(pfc_low_watermark));
}

static inline Ptr<MinHopBridgeNetDevice>
CreateGpuSubnetBridge(Ptr<Node> gpu, uint32_t intraMaxJump)
{
    Ptr<MinHopBridgeNetDevice> dev = CreateObject<MinHopBridgeNetDevice>();
    dev->set_maxjump(intraMaxJump);
    dev->set_ecmp_model(ecmpmodel);
    dev->Set_switchDelay(NanoSeconds(0));
    dev->SetMtu(m_mtu);
    ApplyBridgePfcWatermarks(dev);
    gpu->AddDevice(dev);
    return dev;
}
static inline Mac48Address
GetBridgeMac(const Ptr<MinHopBridgeNetDevice>& bridge)
{
    return Mac48Address::ConvertFrom(bridge->GetAddress());
}
static inline void
SetNetDeviceMac(const Ptr<NetDevice>& dev, const Mac48Address& mac)
{
    dev->SetAddress(mac);
}

static inline void
NormalizeGpuMacsToBridge(Ptr<Node> gpu, const Ptr<MinHopBridgeNetDevice>& bridge)
{
    const Mac48Address mac = GetBridgeMac(bridge);
    for (uint32_t di = 1; di < gpu->GetNDevices(); ++di) {
        gpu->GetDevice(di)->SetAddress(mac);
    }
}
static inline void
BuildServerSubnet_NvSwitch(uint32_t serverId, NodeContainer& hosts, NodeContainer& nvswitch,
                           PointToPointHelper& nvlinkhelper)
{
    //计算偏移量，否则因为重新编号的原因会把第一个子网反复构建，后面啥都没有
    const uint32_t baseGpu = serverId * gpus_per_server;
    for (uint32_t k = 0; k < nvswitch_per_server; ++k) {
        const uint32_t swIdx = serverId * nvswitch_per_server + k;
        Ptr<Node> swNode = nvswitch.Get(swIdx);
        Ptr<MinHopBridgeNetDevice> swBridge = nvswitchDevices.at(swIdx);

        for (uint32_t g = 0; g < gpus_per_server; ++g) {
            const uint32_t gpuIdx = baseGpu + g;
            Ptr<Node> gpuNode = hosts.Get(gpuIdx);
            Ptr<MinHopBridgeNetDevice> gpuBridge = SubNetDevices.at(gpuIdx);
            NS_ASSERT_MSG(gpuBridge != nullptr, "GPU subnet bridge not initialized");

            NodeContainer link(swNode, gpuNode);
            NetDeviceContainer devs = nvlinkhelper.Install(link);

            swBridge->AddBridgePort(devs.Get(0));
            gpuBridge->AddBridgePort(devs.Get(1));
        }
    }
    for (uint32_t g=0;g<gpus_per_server;g++){
        uint32_t gpuindex=serverId*gpus_per_server+g;
        NormalizeGpuMacsToBridge(hosts.Get(gpuindex),SubNetDevices[gpuindex]);
    }
}
static inline void
BuildServerSubnet_FullMesh(uint32_t serverId, NodeContainer& hosts,
                           PointToPointHelper& nvlinkhelper)
{
    const uint32_t baseGpu = serverId * gpus_per_server;//计算偏移量
    for (uint32_t i=0;i<gpus_per_server-1;i++){
        for (uint32_t j=i+1;j<gpus_per_server;j++){
            if (i==j) continue;
            NodeContainer link(hosts.Get(i+baseGpu), hosts.Get(j+baseGpu));
            NetDeviceContainer devs = nvlinkhelper.Install(link);
            SubNetDevices[i+baseGpu]->AddBridgePort(devs.Get(0));
            SubNetDevices[j+baseGpu]->AddBridgePort(devs.Get(1));
        }
    }
    //统一节点地址防止过长的mac表出现
    for (uint32_t i=0;i<gpus_per_server;i++){
        uint32_t gpuindex=serverId*gpus_per_server+i;
        NormalizeGpuMacsToBridge(hosts.Get(gpuindex),SubNetDevices[gpuindex]);
    }
}
static inline void
BuildServerSubnet_Torus(uint32_t serverId, NodeContainer& hosts,
                           PointToPointHelper& nvlinkhelper)
{
    const uint32_t baseGpu = serverId * gpus_per_server;//计算偏移量
    for (uint32_t x=0;x<subnet_k;x++){
        for (uint32_t y=0;y<subnet_k;y++){
            uint32_t idnow=x*subnet_k+y+baseGpu;
            uint32_t id_right=x*subnet_k+((y+1)%subnet_k)+baseGpu;
            uint32_t id_down=((x+1)%subnet_k)*subnet_k+y+baseGpu;

            NodeContainer link_x(hosts.Get(idnow), hosts.Get(id_right));
            NetDeviceContainer devs_x = nvlinkhelper.Install(link_x);
            SubNetDevices[idnow]->AddBridgePort(devs_x.Get(0));
            SubNetDevices[id_right]->AddBridgePort(devs_x.Get(1));

            NodeContainer link_y(hosts.Get(idnow), hosts.Get(id_down));
            NetDeviceContainer devs_y = nvlinkhelper.Install(link_y);
            SubNetDevices[idnow]->AddBridgePort(devs_y.Get(0));
            SubNetDevices[id_down]->AddBridgePort(devs_y.Get(1));
        }
    }
    //统一节点地址防止过长的mac表出现
    for (uint32_t i=0;i<gpus_per_server;i++){
        uint32_t gpuindex=serverId*gpus_per_server+i;
        NormalizeGpuMacsToBridge(hosts.Get(gpuindex),SubNetDevices[gpuindex]);
    }
}

static inline int
GetEffectiveFloodTime()
{
    return std::max(1, effective_floadtime);
}

struct RoceAckTimeoutPathBudget
{
    uint32_t oneWayLinks = 0;
    uint32_t oneWaySwitches = 0;
};

static inline uint64_t
ParseTimeStringToNs(const std::string& raw)
{
    std::string s;
    s.reserve(raw.size());
    for (char c : raw)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
        {
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (s.empty())
    {
        return 0;
    }

    std::size_t unitPos = 0;
    while (unitPos < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[unitPos])) || s[unitPos] == '.'))
    {
        ++unitPos;
    }

    const std::string valueStr = s.substr(0, unitPos);
    const std::string unit = s.substr(unitPos);
    if (valueStr.empty())
    {
        return 0;
    }

    long double value = 0.0L;
    try
    {
        value = std::stold(valueStr);
    }
    catch (...)
    {
        return 0;
    }

    long double scale = 1.0L;
    if (unit == "ns")
    {
        scale = 1.0L;
    }
    else if (unit == "us")
    {
        scale = 1000.0L;
    }
    else if (unit == "ms")
    {
        scale = 1000.0L * 1000.0L;
    }
    else if (unit == "s" || unit.empty())
    {
        scale = 1000.0L * 1000.0L * 1000.0L;
    }
    else
    {
        return 0;
    }

    return static_cast<uint64_t>(std::ceil(value * scale));
}

static inline uint64_t
ComputeSerializationNs(uint64_t bytes, const std::string& rateStr)
{
    if (bytes == 0)
    {
        return 0;
    }
    DataRate rate(rateStr);
    const uint64_t bitRate = rate.GetBitRate();
    if (bitRate == 0)
    {
        return 0;
    }
    const long double ns =
        (static_cast<long double>(bytes) * 8.0L * 1000.0L * 1000.0L * 1000.0L) /
        static_cast<long double>(bitRate);
    return static_cast<uint64_t>(std::ceil(ns));
}

static inline RoceAckTimeoutPathBudget
GetWorstCaseRocePathBudget()
{
    switch (Topology_model)
    {
    case 0: // FatTree: host-edge-agg-core-agg-edge-host
        return {6, 5};
    case 1: // Clos: host-leaf-spine-leaf-host
        return {4, 3};
    case 2: // FullMesh: direct host-to-host, conservative count one forwarding stage
        return {1, 1};
    case 3: // Torus: diameter of a k x k torus shortest path
    {
        const uint32_t diameter = std::max<uint32_t>(1, (torus_x / 2) + (torus_y / 2));
        return {diameter, diameter};
    }
    case 4: // Test: small star-like fallback
        return {2, 1};
    default:
        return {6, 5};
    }
}

static inline uint64_t
ComputeAutoRoceAckTimeoutBaseNs()
{
    const RoceAckTimeoutPathBudget budget = GetWorstCaseRocePathBudget();
    const uint64_t oneWayPropagationNs =
        static_cast<uint64_t>(budget.oneWayLinks) * ParseTimeStringToNs(normallink_delay);
    const uint64_t oneWaySwitchNs =
        static_cast<uint64_t>(budget.oneWaySwitches) * static_cast<uint64_t>(switch_delay);
    const uint64_t oneWaySerializationNs =
        static_cast<uint64_t>(budget.oneWayLinks) * ComputeSerializationNs(m_mtu, normallink_rate);
    return 2ULL * (oneWayPropagationNs + oneWaySwitchNs + oneWaySerializationNs);
}

static inline void
RefreshRoceAckTimeoutFromNetworkConfig()
{
    if (!g_roce_cfg.enableTransportAck)
    {
        g_roce_cfg.ackTimeoutBaseNs = 0;
        return;
    }
    if (!g_roce_cfg.autoAckTimeout)
    {
        g_roce_cfg.ackTimeoutBaseNs = 0;
        return;
    }

    g_roce_cfg.ackTimeoutBaseNs = ComputeAutoRoceAckTimeoutBaseNs();
    const long double factor =
        std::max<long double>(1.0L, static_cast<long double>(g_roce_cfg.ackTimeoutFactor));
    const long double derived =
        static_cast<long double>(g_roce_cfg.ackTimeoutBaseNs) * factor;
    g_roce_cfg.ackTimeoutNs =
        std::max<uint64_t>(1ULL, static_cast<uint64_t>(std::ceil(derived)));
}

static inline std::string
JoinPath(const std::string& dir, const std::string& file)
{
    if (dir.empty())
    {
        return file;
    }
    if (dir.back() == '/')
    {
        return dir + file;
    }
    return dir + "/" + file;
}

static inline std::string
BasenameOnly(const std::string& path)
{
    if (path.empty())
    {
        return path;
    }
    const std::string::size_type pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

static inline std::string
SanitizeDirToken(std::string s)
{
    for (char& c : s)
    {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '-' || c == '_';
        if (!ok)
        {
            c = '-';
        }
    }
    while (s.find("--") != std::string::npos)
    {
        s.replace(s.find("--"), 2, "-");
    }
    if (!s.empty() && s.front() == '-')
    {
        s.erase(s.begin());
    }
    if (!s.empty() && s.back() == '-')
    {
        s.pop_back();
    }
    if (s.empty())
    {
        s = "na";
    }
    return s;
}

static inline std::string
TopologyModelName(uint32_t model)
{
    switch (model)
    {
    case 0:
        return "fattree";
    case 1:
        return "clos";
    case 2:
        return "fullmesh";
    case 3:
        return "torus";
    case 4:
        return "test";
    default:
        return "unknown";
    }
}

static inline std::string
BuildMacTableScenarioTag()
{
    std::ostringstream oss;
    oss << "topo-" << TopologyModelName(Topology_model);
    switch (Topology_model)
    {
    case 0:
        oss << "_k-" << k_fattree;
        break;
    case 1:
        oss << "_k-" << k_clos << "_hostperleaf-" << hostperleaf;
        break;
    case 2:
        oss << "_k-" << k_mesh;
        break;
    case 3:
        oss << "_k-" << k_torus;
        break;
    case 4:
        oss << "_k-" << k_test;
        break;
    default:
        break;
    }

    oss << "_subnet-" << subnet_model
        << "_subnetk-" << subnet_k
        << "_gpusper-" << gpus_per_server
        << "_nvsper-" << nvswitch_per_server;
    return oss.str();
}

static inline std::string
AppendScenarioTagIfMissing(const std::string& baseDir, const std::string& tag)
{
    if (baseDir.empty())
    {
        return JoinPath("mactable", tag);
    }

    std::string normalized = baseDir;
    while (normalized.size() > 1 && normalized.back() == '/')
    {
        normalized.pop_back();
    }
    if (normalized.size() >= tag.size() &&
        normalized.compare(normalized.size() - tag.size(), tag.size(), tag) == 0)
    {
        return normalized;
    }
    return JoinPath(normalized, tag);
}

static inline bool
PathExists(const std::string& path)
{
    std::ifstream in(path);
    return in.good();
}

static inline bool
EnsureDirectoryRecursive(const std::string& dir)
{
    if (dir.empty())
    {
        return false;
    }

    std::string normalized = dir;
    while (normalized.size() > 1 && normalized.back() == '/')
    {
        normalized.pop_back();
    }

    std::string current;
    if (!normalized.empty() && normalized[0] == '/')
    {
        current = "/";
    }

    std::stringstream ss(normalized);
    std::string part;
    while (std::getline(ss, part, '/'))
    {
        if (part.empty())
        {
            continue;
        }
        if (!current.empty() && current.back() != '/')
        {
            current.push_back('/');
        }
        current += part;

        struct stat st;
        if (stat(current.c_str(), &st) == 0)
        {
            if (!S_ISDIR(st.st_mode))
            {
                return false;
            }
            continue;
        }
        if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST)
        {
            return false;
        }
    }
    return true;
}

static inline std::string
SanitizeMacForFilename(const Mac48Address& mac)
{
    std::ostringstream oss;
    oss << mac;
    std::string out = oss.str();
    std::replace(out.begin(), out.end(), ':', '-');
    return out;
}

static inline bool
IsBridgeEligibleForMacTableIo(const Ptr<MinHopBridgeNetDevice>& dev)
{
    if (!dev)
    {
        return false;
    }
    if (dev->GetNBridgePorts() == 0)
    {
        return false;
    }
    return Mac48Address::IsMatchingType(dev->GetAddress());
}

static inline std::string
GetBridgeTableFilename(const Ptr<MinHopBridgeNetDevice>& dev)
{
    const uint32_t nodeId = dev && dev->GetNode() ? dev->GetNode()->GetId() : 0;
    const uint32_t ifIndex = dev ? dev->GetIfIndex() : 0;
    Mac48Address mac("00:00:00:00:00:00");
    if (dev && Mac48Address::IsMatchingType(dev->GetAddress()))
    {
        mac = Mac48Address::ConvertFrom(dev->GetAddress());
    }
    std::ostringstream oss;
    oss << "bridge-node-" << nodeId
        << "-if-" << ifIndex
        << "-mac-" << SanitizeMacForFilename(mac)
        << ".tbl";
    return oss.str();
}

static inline void
AppendBridgeVectorUnique(const std::vector<Ptr<MinHopBridgeNetDevice>>& src,
                         std::vector<Ptr<MinHopBridgeNetDevice>>& dst)
{
    for (const auto& dev : src)
    {
        if (!dev)
        {
            continue;
        }
        bool exists = false;
        for (const auto& cur : dst)
        {
            if (cur == dev)
            {
                exists = true;
                break;
            }
        }
        if (!exists)
        {
            dst.push_back(dev);
        }
    }
}

static inline std::vector<Ptr<MinHopBridgeNetDevice>>
CollectAllBridgeDevices()
{
    std::vector<Ptr<MinHopBridgeNetDevice>> devices;
    AppendBridgeVectorUnique(coreDevices, devices);
    AppendBridgeVectorUnique(aggregationDevices, devices);
    AppendBridgeVectorUnique(edgeDevices, devices);
    AppendBridgeVectorUnique(nvswitchDevices, devices);
    AppendBridgeVectorUnique(SpineDevices, devices);
    AppendBridgeVectorUnique(LeafDevices, devices);
    AppendBridgeVectorUnique(TorusDevices, devices);
    AppendBridgeVectorUnique(FullmeshDevices, devices);
    AppendBridgeVectorUnique(TestSwitchDevices, devices);
    AppendBridgeVectorUnique(SubNetDevices, devices);
    return devices;
}

static inline bool
DumpBridgeMacTablesToDirectory(const std::string& dir)
{
    std::vector<Ptr<MinHopBridgeNetDevice>> bridges = CollectAllBridgeDevices();
    if (bridges.empty())
    {
        NS_LOG_UNCOND("[MACTABLE][DUMP] no bridge devices found.");
        return false;
    }
    if (!EnsureDirectoryRecursive(dir))
    {
        NS_LOG_UNCOND("[MACTABLE][DUMP] failed to create/access directory: " << dir);
        return false;
    }

    uint32_t ok = 0;
    uint32_t fail = 0;
    uint32_t skip = 0;
    for (const auto& dev : bridges)
    {
        if (!IsBridgeEligibleForMacTableIo(dev))
        {
            ++skip;
            continue;
        }
        const std::string file = JoinPath(dir, GetBridgeTableFilename(dev));
        if (dev->writemactable(file))
        {
            ++ok;
        }
        else
        {
            ++fail;
        }
    }

    NS_LOG_UNCOND("[MACTABLE][DUMP] dir=" << dir << " ok=" << ok
                                          << " fail=" << fail
                                          << " skip=" << skip);
    return fail == 0;
}

static inline bool
LoadBridgeMacTablesFromDirectory(const std::string& dir)
{
    std::vector<Ptr<MinHopBridgeNetDevice>> bridges = CollectAllBridgeDevices();
    if (bridges.empty())
    {
        NS_LOG_UNCOND("[MACTABLE][LOAD] no bridge devices found.");
        return false;
    }

    uint32_t ok = 0;
    uint32_t fail = 0;
    uint32_t skip = 0;
    for (const auto& dev : bridges)
    {
        if (!IsBridgeEligibleForMacTableIo(dev))
        {
            ++skip;
            continue;
        }
        const std::string file = JoinPath(dir, GetBridgeTableFilename(dev));
        if (!PathExists(file))
        {
            ++fail;
            NS_LOG_UNCOND("[MACTABLE][LOAD] missing file: " << file);
            continue;
        }
        if (dev->readmactable(file))
        {
            ++ok;
        }
        else
        {
            ++fail;
        }
    }

    NS_LOG_UNCOND("[MACTABLE][LOAD] dir=" << dir << " ok=" << ok
                                          << " fail=" << fail
                                          << " skip=" << skip);
    return ok > 0 && fail == 0;
}

bool ReadConf(std::string network_conf)
{
  using std::string;

  // ---------- 依赖：nlohmann/json ----------
  // 如果你是 <nlohmann/json.hpp>，把下一行改成 nlohmann/json.hpp
  // 并在文件顶部 #include。这里只写函数体，不重复 include。
  using nlohmann::json;

  // ---------- 打开并解析 JSON ----------
  std::ifstream in(network_conf);
  if (!in) {
    std::cerr << "[Config] cannot open: " << network_conf << "\n";
    return false;
  }
  json j;
  try {
    in >> j;
  } catch (const std::exception& e) {
    std::cerr << "[Config] parse error: " << e.what() << "\n";
    return false;
  }

  // ---------- 小工具 ----------
  auto JHas = [&](const json& x, const char* k){ return x.contains(k) && !x.at(k).is_null(); };
  auto JGetStr = [&](const json& x, const char* k, std::string& out){
    if (JHas(x,k)) out = x.at(k).get<std::string>();
  };
  auto JGetBool = [&](const json& x, const char* k, bool& out){
    if (JHas(x,k)) out = x.at(k).get<bool>();
  };
  auto JGetDouble = [&](const json& x, const char* k, double& out){
    if (JHas(x,k)) out = x.at(k).get<double>();
  };
  auto to_upper = [](std::string s){ for(char& c:s) c = (char)std::toupper((unsigned char)c); return s; };

  auto ParseGpuType = [&](const std::string& sIn)->GPUType {
    std::string s = to_upper(sIn);
    if (s=="A100") return GPUType::A100;
    if (s=="H100") return GPUType::H100;
    // 需要其它类型在此扩展
    return GPUType::A100; // 默认
  };
  auto ParseTopoModel = [&](const json& top)->int {
    if (JHas(top,"model")) {
      if (top["model"].is_number_integer()) {
        int v = top["model"].get<int>();
        if (v>=0 && v<=4) return v;
      } else if (top["model"].is_string()) {
        std::string s = top["model"].get<std::string>(); 
        for (auto& c:s) c=(char)std::tolower((unsigned char)c);
        if (s=="fattree")  return 0;
        if (s=="clos")     return 1;
        if (s=="fullmesh") return 2;
        if (s=="torus")    return 3;
        if (s=="test")     return 4;
      }
    }
    return 0; // 默认 FatTree
  };
  auto ParseRoceLogLevel = [&](const json& v)->RoceLogLevel {
    if (v.is_number_integer() || v.is_number_unsigned()) {
      const int level = v.get<int>();
      if (level <= 0) {
        return RoceLogLevel::Off;
      }
      if (level == 1) {
        return RoceLogLevel::Basic;
      }
      return RoceLogLevel::Full;
    }
    if (v.is_string()) {
      std::string s = v.get<std::string>();
      for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (s == "off" || s == "none" || s == "quiet") {
        return RoceLogLevel::Off;
      }
      if (s == "basic" || s == "partial" || s == "summary") {
        return RoceLogLevel::Basic;
      }
      if (s == "full" || s == "all" || s == "verbose") {
        return RoceLogLevel::Full;
      }
    }
    return g_roce_cfg.logLevel;
  };
  auto ParsePfcLogLevel = [&](const json& v)->PfcLogLevel {
    if (v.is_number_integer() || v.is_number_unsigned()) {
      const int level = v.get<int>();
      if (level <= 0) {
        return PfcLogLevel::Off;
      }
      if (level == 1) {
        return PfcLogLevel::Basic;
      }
      return PfcLogLevel::Full;
    }
    if (v.is_string()) {
      std::string s = v.get<std::string>();
      for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (s == "off" || s == "none" || s == "quiet") {
        return PfcLogLevel::Off;
      }
      if (s == "basic" || s == "partial" || s == "summary") {
        return PfcLogLevel::Basic;
      }
      if (s == "full" || s == "all" || s == "verbose") {
        return PfcLogLevel::Full;
      }
    }
    return g_pfcLogLevel;
  };
  auto ParseRoceAckTimeoutMode = [&](const json& v)->bool {
    if (v.is_boolean())
    {
      return v.get<bool>();
    }
    if (v.is_number_integer() || v.is_number_unsigned())
    {
      return v.get<int>() != 0;
    }
    if (v.is_string())
    {
      std::string s = v.get<std::string>();
      for (auto& c : s)
      {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (s == "auto" || s == "automatic" || s == "derived")
      {
        return true;
      }
      if (s == "manual" || s == "fixed" || s == "off")
      {
        return false;
      }
    }
    return g_roce_cfg.autoAckTimeout;
  };

  // ---------- 备份旧值（失败时不改动） ----------
  auto gpu_type_bak             = gpu_type;
  auto gpus_per_server_bak      = gpus_per_server;
  auto nvswitch_per_server_bak  = nvswitch_per_server;
  auto ecmpmodel_bak            = ecmpmodel;
  auto max_jump_bak             = max_jump;
  auto floadtime_bak            = floadtime;
  auto m_mtu_bak                = m_mtu;
  auto normallink_rate_bak      = normallink_rate;
  auto nvlink_rate_bak          = nvlink_rate;
  auto normallink_delay_bak     = normallink_delay;
  auto nvlink_delay_bak         = nvlink_delay;
  auto DropTailQueue_size_bak   = DropTailQueue_size;
  auto DropTailQueue_num_bak    = DropTailQueue_num;
  auto Topology_model_bak       = Topology_model;
  auto k_bak                    = k_all;
  auto hostperleaf_bak          = hostperleaf;
  auto roce_cfg_bak             = g_roce_cfg;
  auto pfc_log_level_bak        = g_pfcLogLevel;
  auto pfc_high_global_bak      = g_pfcHighWatermark;
  auto pfc_low_global_bak       = g_pfcLowWatermark;
  auto mactable_load_enable_bak = mactable_load_enable;
  auto mactable_load_dir_bak    = mactable_load_dir;
  auto effective_floadtime_bak  = effective_floadtime;
  auto pfc_high_watermark_bak   = pfc_high_watermark;
  auto pfc_low_watermark_bak    = pfc_low_watermark;
  bool mactable_load_switch_seen = false;


  // ---------- 1) 顶层通用 ----------
  if (j.contains("workloadpath")) network_workload = j["workloadpath"].get<std::string>();
  if (j.contains("thread")) m_thread = j["thread"].get<int>();
  if (j.contains("gpu_type")) gpu_type = ParseGpuType(j["gpu_type"].get<std::string>());
  if (j.contains("gpus_per_server"))     gpus_per_server     = j["gpus_per_server"].get<uint32_t>();
  if (j.contains("nvswitch_per_server")) nvswitch_per_server = j["nvswitch_per_server"].get<uint32_t>();

  // ---------- 2) network ----------
  if (j.contains("network")) {
    const auto& nw = j["network"];
    if (JHas(nw,"ecmpmodel"))            ecmpmodel        = nw["ecmpmodel"].get<int>();
    if (JHas(nw,"max_jump"))             max_jump         = nw["max_jump"].get<int>();
    if (JHas(nw,"mtu"))                  m_mtu            = nw["mtu"].get<uint32_t>();
    JGetStr(nw, "normallink_rate",       normallink_rate);
    JGetStr(nw, "nvlink_rate",           nvlink_rate);
    JGetStr(nw, "normallink_delay",      normallink_delay);
    JGetStr(nw, "nvlink_delay",          nvlink_delay);
    JGetStr(nw, "drop_tail_queue_size",  DropTailQueue_size);
    JGetStr(nw, "drop_tail_queue_num",  DropTailQueue_num);
    if (JHas(nw, "pfc_high_watermark"))
    {
      JGetDouble(nw, "pfc_high_watermark", pfc_high_watermark);
      g_pfcHighWatermark = pfc_high_watermark;
    }
    if (JHas(nw, "pfc_low_watermark"))
    {
      JGetDouble(nw, "pfc_low_watermark", pfc_low_watermark);
      g_pfcLowWatermark = pfc_low_watermark;
    }
    if (JHas(nw, "pfc_log_level"))
    {
      g_pfcLogLevel = ParsePfcLogLevel(nw["pfc_log_level"]);
    }
    if (JHas(nw, "mactable_load_enable"))
    {
      JGetBool(nw, "mactable_load_enable", mactable_load_enable);
      mactable_load_switch_seen = true;
    }
    JGetStr(nw, "mactable_load_dir", mactable_load_dir);
    if (mactable_load_dir.empty())
    {
      JGetStr(nw, "mactable_dump_dir", mactable_load_dir);
    }
  }

  // ---------- 2.5) mactable ----------
  if (j.contains("mactable"))
  {
    const auto& mt = j["mactable"];
    if (JHas(mt, "load_enable"))
    {
      JGetBool(mt, "load_enable", mactable_load_enable);
      mactable_load_switch_seen = true;
    }
    JGetStr(mt, "dir", mactable_load_dir);
    JGetStr(mt, "load_dir", mactable_load_dir);
    if (mactable_load_dir.empty())
    {
      JGetStr(mt, "dump_dir", mactable_load_dir);
    }
  }

  if (j.contains("pfc") && j["pfc"].is_object())
  {
    const auto& pf = j["pfc"];
    JGetStr(pf, "drop_tail_queue_size", DropTailQueue_size);
    JGetStr(pf, "drop_tail_queue_num", DropTailQueue_num);
    if (JHas(pf, "high_watermark"))
    {
      JGetDouble(pf, "high_watermark", pfc_high_watermark);
      g_pfcHighWatermark = pfc_high_watermark;
    }
    if (JHas(pf, "low_watermark"))
    {
      JGetDouble(pf, "low_watermark", pfc_low_watermark);
      g_pfcLowWatermark = pfc_low_watermark;
    }
    if (JHas(pf, "log_level"))
    {
      g_pfcLogLevel = ParsePfcLogLevel(pf["log_level"]);
    }
  }

  // ---------- 3) workload ----------
  if (j.contains("workload")) {
    const auto& wl = j["workload"];
    if (JHas(wl,"floadtime_s")) floadtime = wl["floadtime_s"].get<uint64_t>();
  }

  // ---------- 3.1) roce ----------
  if (j.contains("roce") && j["roce"].is_object()) {
    const auto& rc = j["roce"];
    if (JHas(rc, "enable")) {
      g_roce_cfg.enable = rc["enable"].get<bool>();
    }
    if (JHas(rc, "strict_completion")) {
      g_roce_cfg.strictCompletion = rc["strict_completion"].get<bool>();
    }
    if (JHas(rc, "data_use_write")) {
      g_roce_cfg.dataUseWrite = rc["data_use_write"].get<bool>();
    }
    if (JHas(rc, "sq_depth")) {
      g_roce_cfg.defaultSqDepth = rc["sq_depth"].get<uint32_t>();
    }
    if (JHas(rc, "rq_depth")) {
      g_roce_cfg.defaultRqDepth = rc["rq_depth"].get<uint32_t>();
    }
    if (JHas(rc, "transport_ack_enable")) {
      g_roce_cfg.enableTransportAck = rc["transport_ack_enable"].get<bool>();
    }
    if (JHas(rc, "transport_window_packets")) {
      g_roce_cfg.transportWindowPackets = rc["transport_window_packets"].get<uint32_t>();
    }
    if (JHas(rc, "reorder_window_packets")) {
      g_roce_cfg.reorderWindowPackets = rc["reorder_window_packets"].get<uint32_t>();
    }
    if (JHas(rc, "ack_timeout_mode")) {
      g_roce_cfg.autoAckTimeout = ParseRoceAckTimeoutMode(rc["ack_timeout_mode"]);
    }
    if (JHas(rc, "ack_timeout_factor")) {
      g_roce_cfg.ackTimeoutFactor = rc["ack_timeout_factor"].get<double>();
    }
    if (JHas(rc, "ack_timeout_ns")) {
      g_roce_cfg.ackTimeoutNs = rc["ack_timeout_ns"].get<uint64_t>();
    }
    if (JHas(rc, "max_retransmissions")) {
      g_roce_cfg.maxRetransmissions = rc["max_retransmissions"].get<uint32_t>();
    }
    if (JHas(rc, "log_level")) {
      g_roce_cfg.logLevel = ParseRoceLogLevel(rc["log_level"]);
    }
    if (JHas(rc, "mode") && rc["mode"].is_string()) {
      g_roce_cfg.modeName = rc["mode"].get<std::string>();
    }
    if (JHas(rc, "ether_type")) {
      if (rc["ether_type"].is_number_unsigned() || rc["ether_type"].is_number_integer()) {
        g_roce_cfg.etherType = static_cast<uint16_t>(rc["ether_type"].get<uint32_t>());
      } else if (rc["ether_type"].is_string()) {
        const std::string s = rc["ether_type"].get<std::string>();
        try {
          g_roce_cfg.etherType = static_cast<uint16_t>(std::stoul(s, nullptr, 0));
        } catch (...) {
          // Keep current ether type on parse failure.
        }
      }
    }
  }

  // ---------- 4) topology + 派生量 ----------
  uint64_t switch_delay_override = 0; // 0 表示不覆盖
  if (j.contains("topology")) {
    const auto& top = j["topology"];
    Topology_model = ParseTopoModel(top);
    if (JHas(top,"k"))   k_all   = top["k"].get<int>();
    if (JHas(top,"subnet_model"))   subnet_model   = top["subnet_model"].get<int>();
    if (JHas(top,"subnet_k"))   subnet_k   = top["subnet_k"].get<int>();
    if (JHas(top,"hostperleaf")) hostperleaf = top["hostperleaf"].get<int>();
    if (JHas(top,"switch_delay_ns_override"))
      switch_delay_override = top["switch_delay_ns_override"].is_null() ? 0ULL
                                 : top["switch_delay_ns_override"].get<uint64_t>();
  }

  // 计算派生量（与SetConfig一致）
    if (subnet_model==0){//nosubnet
        gpus_per_server=1;
        nvswitch_per_server=1;
    }
    if (subnet_model==1){//nvswitch
        gpus_per_server=subnet_k;
    }
    if (subnet_model==2){//fullmesh
        gpus_per_server=subnet_k;
        nvswitch_per_server=1;
    }
    if (subnet_model==3){//torus
        gpus_per_server=subnet_k*subnet_k;
        nvswitch_per_server=1;
    }
  switch (Topology_model) {
    case 0: { // FatTree
      k_fattree=k_all;
      numHosts        = k_fattree * k_fattree * k_fattree / 4;
      numCores        = (k_fattree/2) * (k_fattree/2);
      numAggregations = k_fattree * k_fattree / 2;
      numEdges        = k_fattree * k_fattree / 2;

      numGPUs      = numHosts * (int)gpus_per_server;
      nvswitch_num = numHosts * (int)nvswitch_per_server;
      switch_num   = numCores + numAggregations + numEdges;
      node_num     = numGPUs + switch_num + nvswitch_num;

      switch_delay = switch_delay_override ? switch_delay_override : 100; // ns
    } break;

    case 1: { // Clos
      k_clos=k_all;
      numSpine = k_clos / 2;  // （与你原注释一致）
      numLeaf  = k_clos;
      numHosts     = numLeaf * hostperleaf;

      numGPUs      = numHosts * (int)gpus_per_server;
      nvswitch_num = numHosts * (int)nvswitch_per_server;
      switch_num   = numSpine + numLeaf;
      node_num     = numGPUs + switch_num + nvswitch_num;

      switch_delay = switch_delay_override ? switch_delay_override : 5000; // ns
    } break;

    case 2: { // FullMesh
      k_mesh=k_all;
      num_meshswitch = k_mesh;
      numHosts     = k_mesh;

      numGPUs      = numHosts * (int)gpus_per_server;
      nvswitch_num = numHosts * (int)nvswitch_per_server;
      switch_num   = num_meshswitch;
      node_num     = numGPUs + switch_num + nvswitch_num;

      switch_delay = switch_delay_override ? switch_delay_override : 100; // ns
    } break;

    case 3: { // Torus
      k_torus=k_all;
      torus_x = k_torus, torus_y = k_torus;
      num_torusswitch = torus_x * torus_y;
      numHosts     = num_torusswitch;

      numGPUs      = numHosts * (int)gpus_per_server;
      nvswitch_num = numHosts * (int)nvswitch_per_server;
      switch_num   = num_torusswitch;
      node_num     = numGPUs + switch_num + nvswitch_num;

      switch_delay = switch_delay_override ? switch_delay_override : 100; // ns
    } break;

    case 4: { // Test
      num_testswitch = 1;
      numHosts     = k_test;

      numGPUs      = numHosts * (int)gpus_per_server;
      nvswitch_num = numHosts * (int)nvswitch_per_server;
      switch_num   = num_testswitch;
      node_num     = numGPUs + switch_num + nvswitch_num;

      switch_delay = switch_delay_override ? switch_delay_override : 100; // ns
    } break;

    default: {
      std::cerr << "[Config] unknown Topology_model: " << Topology_model << "\n";
      // 回滚备份
      gpu_type = gpu_type_bak; gpus_per_server = gpus_per_server_bak; nvswitch_per_server = nvswitch_per_server_bak;
      ecmpmodel = ecmpmodel_bak; max_jump = max_jump_bak; floadtime = floadtime_bak; m_mtu = m_mtu_bak;
      normallink_rate = normallink_rate_bak; nvlink_rate = nvlink_rate_bak;
      normallink_delay = normallink_delay_bak; nvlink_delay = nvlink_delay_bak;
      DropTailQueue_size = DropTailQueue_size_bak; DropTailQueue_num = DropTailQueue_num_bak;
      Topology_model = Topology_model_bak;
      k_fattree = k_bak; k_clos = k_bak; hostperleaf = hostperleaf_bak;
      k_mesh = k_bak; k_torus = k_bak; k_test = k_bak;
      g_roce_cfg = roce_cfg_bak;
      g_pfcLogLevel = pfc_log_level_bak;
      g_pfcHighWatermark = pfc_high_global_bak;
      g_pfcLowWatermark = pfc_low_global_bak;
      mactable_load_enable = mactable_load_enable_bak;
      mactable_load_dir = mactable_load_dir_bak;
      effective_floadtime = effective_floadtime_bak;
      pfc_high_watermark = pfc_high_watermark_bak;
      pfc_low_watermark = pfc_low_watermark_bak;
      return false;
    }
  }

  if (!mactable_load_switch_seen)
  {
    mactable_load_enable = false;
  }
  if (mactable_load_dir.empty())
  {
    mactable_load_dir = "mactable";
  }
  const std::string mactableScenarioTag = BuildMacTableScenarioTag();
  mactable_load_dir = AppendScenarioTagIfMissing(mactable_load_dir, mactableScenarioTag);

  effective_floadtime = mactable_load_enable ? 1 : floadtime;
  effective_floadtime = std::max(1, effective_floadtime);
  g_roce_cfg.transportWindowPackets = std::max<uint32_t>(1, g_roce_cfg.transportWindowPackets);
  g_roce_cfg.reorderWindowPackets = std::max<uint32_t>(1, g_roce_cfg.reorderWindowPackets);
  g_roce_cfg.reorderWindowPackets =
      std::min(g_roce_cfg.reorderWindowPackets, g_roce_cfg.transportWindowPackets);
  g_pfcHighWatermark = pfc_high_watermark;
  g_pfcLowWatermark = pfc_low_watermark;
  RefreshRoceAckTimeoutFromNetworkConfig();
  return true;
}

void SetConfig(){
    g_roce_cfg.enable = false;
    g_roce_cfg.dataUseWrite = true;
    g_roce_cfg.strictCompletion = false;
    g_roce_cfg.etherType = 0x9999;
    g_roce_cfg.defaultSqDepth = 1024;
    g_roce_cfg.defaultRqDepth = 1024;
    g_roce_cfg.enableTransportAck = true;
    g_roce_cfg.transportWindowPackets = 32;
    g_roce_cfg.reorderWindowPackets = 32;
    g_roce_cfg.autoAckTimeout = false;
    g_roce_cfg.ackTimeoutFactor = 2.0;
    g_roce_cfg.ackTimeoutBaseNs = 0;
    g_roce_cfg.ackTimeoutNs = 5000;
    g_roce_cfg.maxRetransmissions = 8;
    g_roce_cfg.logLevel = RoceLogLevel::Full;
    g_roce_cfg.modeName = "legacy";
    g_pfcLogLevel = PfcLogLevel::Full;
    g_pfcHighWatermark = 0.8;
    g_pfcLowWatermark = 0.2;

    gpu_type = GPUType::A100;
    gpus_per_server=1;
    nvswitch_per_server=1;
    ecmpmodel=1;//ECMP模式；0随机转发，1按包hash
    max_jump=5;//最大5跳
    floadtime=200;//泛红时间200秒
    effective_floadtime=floadtime;
    m_mtu=4096;
    normallink_rate="1600Gbps";
    nvlink_rate="2400Gbps";
    normallink_delay="5ns";
    nvlink_delay="15ns";
    DropTailQueue_size="1000000";
    DropTailQueue_num="16";
    mactable_load_enable=false;
    mactable_load_dir="mactable";
    pfc_high_watermark=0.8;
    pfc_low_watermark=0.2;
    g_pfcHighWatermark = pfc_high_watermark;
    g_pfcLowWatermark = pfc_low_watermark;

    Topology_model=0;//0--FatTree;1--Clos;2--FullMesh;3--Torus;4--Test
    subnet_model=0;//0--nosubnet;1--nvswitch;2--FullMesh;3--Torus;
    subnet_k=4;
    //-----------------subnet_set----------------
    //nosubnet
    if (subnet_model==0){
        gpus_per_server=1;
        nvswitch_per_server=1;
    }
    //nvswitch
    if (subnet_model==1){
        //do nothing
    }
    //FullMesh
    if (subnet_model==2){
        gpus_per_server=subnet_k;
        nvswitch_per_server=1;
    }
    //Torus
    if (subnet_model==3){
        gpus_per_server=subnet_k*subnet_k;
        nvswitch_per_server=1;
    }

    //-----------------mainnet_set----------------
    //fattree
    if (Topology_model==0){
        k_fattree=4;//FatTree规模
        numHosts = k_fattree*k_fattree*k_fattree/4;//终端数量
        numCores = (k_fattree/2)*(k_fattree/2);//核心层交换机数量
        numAggregations = k_fattree*k_fattree/2;//汇聚层交换机数量
        numEdges = k_fattree*k_fattree/2;//边缘层交换机数量

        numGPUs=numHosts*gpus_per_server;
        nvswitch_num=numHosts*nvswitch_per_server;

        switch_num=numCores+numAggregations+numEdges;
        node_num=numGPUs+switch_num+nvswitch_num;
        
        switch_delay=100;//单位纳秒
    }
    //clos
    if (Topology_model==1){
        k_clos=8;
        hostperleaf=2;
        numSpine=k_clos/2;
        // numSpine=k_clos;
        numLeaf=k_clos;
        numHosts=numLeaf*hostperleaf;

        numGPUs=numHosts*gpus_per_server;
        nvswitch_num=numHosts*nvswitch_per_server;
        switch_num=numSpine+numLeaf;
        node_num=numGPUs+switch_num+nvswitch_num;

        switch_delay=5000;//单位纳秒
    }
    //fullmesh
    if (Topology_model==2){
        k_mesh=16;
        num_meshswitch=k_mesh;
        numHosts=k_mesh;
        numGPUs=numHosts*gpus_per_server;
        nvswitch_num=numHosts*nvswitch_per_server;
        switch_num=num_meshswitch;
        node_num=numGPUs+switch_num+nvswitch_num;

        switch_delay=100;//单位纳秒
    }
    //torus
    if (Topology_model==3){
        k_torus=4;
        torus_x=k_torus;
        torus_y=k_torus;
        
        num_torusswitch=torus_x*torus_y;
        numHosts=num_torusswitch;

        numGPUs=numHosts*gpus_per_server;
        nvswitch_num=numHosts*nvswitch_per_server;
        switch_num=num_torusswitch;
        node_num=numGPUs+switch_num+nvswitch_num;

        switch_delay=100;//单位纳秒
    }
    
    //test(仅调试用)
    if (Topology_model==4){
        k_test=16;
        num_testswitch=1;
        numHosts=k_test;

        numGPUs=numHosts*gpus_per_server;
        nvswitch_num=numHosts*nvswitch_per_server;
        switch_num=num_testswitch;
        node_num=numGPUs+switch_num+nvswitch_num;

        switch_delay=100;//单位纳秒
    }
    RefreshRoceAckTimeoutFromNetworkConfig();
}

void SetupNetwork(){
    NS_LOG_INFO("common now");
    
    // LogComponentEnable("MultithreadedSimulatorImpl", LOG_LEVEL_INFO);
    // LogComponentEnable("MtpInterface",LOG_LEVEL_INFO);
    // LogComponentEnable("LogicalProcess", LOG_LEVEL_INFO);
    // LogComponentEnable("MinHopBridgeNetDevice", LOG_LEVEL_INFO);
    // LogComponentEnable("SendFlowClient", LOG_LEVEL_INFO);
    // LogComponentEnable("HopCountClient", LOG_LEVEL_INFO);
    // LogComponentEnable("ThreePhaseProtocol", LOG_LEVEL_INFO);
    //创建整个网络的节点

    //FatTree
    if (Topology_model==0){
        NodeContainer coreSwitches;
        coreSwitches.Create(numCores);
        NodeContainer aggregationSwitches;
        aggregationSwitches.Create(numAggregations);
        NodeContainer edgeSwitches;
        edgeSwitches.Create(numEdges);
        NodeContainer hosts;
        hosts.Create(numGPUs);
        NodeContainer nvswitch;
        nvswitch.Create(nvswitch_num);

        #ifdef NS3_MPI
        uint32_t numRanks = MpiInterface::GetSize();
        for (uint32_t p = 0; p < k_fattree; ++p) { // 遍历所有 Pods (0 到 k-1)
            // 计算当前 Pod 应该分配给哪个 Rank (SystemId)
            uint32_t targetRank = p % numRanks;
            
            // 1.1 汇聚层交换机 (Aggregation Switches)
            for (uint32_t i = 0; i < k_fattree / 2; ++i) { 
                uint32_t aggIndex = p * (k_fattree / 2) + i;
                aggregationSwitches.Get(aggIndex)->SetSystemId(targetRank);
            }
            
            // 1.2 边缘层交换机 (Edge Switches)
            for (uint32_t i = 0; i < k_fattree / 2; ++i) { 
                uint32_t edgeIndex = p * (k_fattree / 2) + i;
                edgeSwitches.Get(edgeIndex)->SetSystemId(targetRank);
            }
            
            // 1.3 主机 (Hosts)
            for (uint32_t i = 0; i < k_fattree * k_fattree / 4; ++i) { 
                for(uint32_t j = 0; j < gpus_per_server ; ++j)
                {
                    uint32_t hostIndex = (p * (k_fattree * k_fattree / 4) + i) * gpus_per_server + j;
                    hosts.Get(hostIndex)->SetSystemId(targetRank);                
                }
                for(uint32_t j = 0; j < nvswitch_per_server ; ++j)
                {
                    uint32_t nvswitchIndex = (p * (k_fattree * k_fattree / 4) + i) * nvswitch_per_server +  j;
                    nvswitch.Get(nvswitchIndex)->SetSystemId(targetRank);
                }
            }
        }
        // 2. 核心层交换机分区 (Core Switches) - 轮询分配 (Round-Robin)
        for (uint32_t i = 0; i < numCores; ++i) {
            uint32_t targetRank = i % numRanks; // 核心交换机按索引轮询分配
            coreSwitches.Get(i)->SetSystemId(targetRank);
        }
        #endif


        //为所有交换机创建 MinHopBridgeNetDevice
        for (uint32_t i = 0; i < coreSwitches.GetN(); ++i) {
            Ptr<MinHopBridgeNetDevice> dev = CreateObject<MinHopBridgeNetDevice>();
            dev->set_maxjump(max_jump);
            dev->set_ecmp_model(ecmpmodel);
            dev->Set_switchDelay(NanoSeconds(switch_delay));
            dev->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(dev);
            coreSwitches.Get(i)->AddDevice(dev);
            coreDevices.push_back(dev);
        }
        for (uint32_t i = 0; i < aggregationSwitches.GetN(); ++i) {
            Ptr<MinHopBridgeNetDevice> dev = CreateObject<MinHopBridgeNetDevice>();
            dev->set_maxjump(max_jump);
            dev->set_ecmp_model(ecmpmodel);
            dev->Set_switchDelay(NanoSeconds(switch_delay));
            dev->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(dev);
            aggregationSwitches.Get(i)->AddDevice(dev);
            aggregationDevices.push_back(dev);
        }
        for (uint32_t i = 0; i < edgeSwitches.GetN(); ++i) {
            Ptr<MinHopBridgeNetDevice> dev = CreateObject<MinHopBridgeNetDevice>();
            dev->set_maxjump(max_jump);
            dev->set_ecmp_model(ecmpmodel);
            dev->Set_switchDelay(NanoSeconds(switch_delay));
            dev->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(dev);
            edgeSwitches.Get(i)->AddDevice(dev);
            edgeDevices.push_back(dev);
        }
        //为所有nvswitch安装特化的minhopbridge
        for (uint32_t i = 0; i < nvswitch.GetN(); ++i) {
            Ptr<MinHopBridgeNetDevice> nvs = CreateObject<MinHopBridgeNetDevice>();
            nvs->set_ecmp_model(ecmpmodel);
            nvs->Set_switchDelay(Seconds(0));
            nvs->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(nvs);
            nvswitch.Get(i)->AddDevice(nvs);
            nvswitchDevices.push_back(nvs);
        }

        // --- 3. 配置 P2P 链路 ---
        PointToPointHelper p2pHelper;
        p2pHelper.SetDeviceAttribute("DataRate", StringValue(normallink_rate));//链路带宽
        p2pHelper.SetChannelAttribute("Delay", StringValue(normallink_delay));
        // p2pHelper.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(DropTailQueue_size)); 
        p2pHelper.SetQueue("ns3::PppRoundRobinQueue", 
                       "MaxPacketsPerQueue", StringValue(DropTailQueue_size),
                       "NumQueues", StringValue(DropTailQueue_num));
        p2pHelper.SetDeviceAttribute("Mtu", UintegerValue(m_mtu)); // 支持 30KB 帧
        PointToPointHelper nvlinkhelper;
        nvlinkhelper.SetDeviceAttribute("DataRate", StringValue(nvlink_rate));
        nvlinkhelper.SetChannelAttribute("Delay", StringValue(nvlink_delay));//存疑
        // nvlinkhelper.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(DropTailQueue_size)); 
        nvlinkhelper.SetQueue("ns3::PppRoundRobinQueue", 
                       "MaxPacketsPerQueue", StringValue(DropTailQueue_size),
                       "NumQueues", StringValue(DropTailQueue_num));
        nvlinkhelper.SetDeviceAttribute("Mtu", UintegerValue(m_mtu)); 

        // --- 4. 连接：核心层 ↔ 汇聚层 ---
        for (uint32_t i = 0; i < k_fattree / 2; ++i) { // 核心层第 i 行
            for (uint32_t j = 0; j < k_fattree / 2; ++j) { // 核心层第 j 列
                uint32_t coreIndex = i * (k_fattree / 2) + j;
                for (uint32_t p = 0; p < k_fattree; ++p) { // 每个 Pod
                    uint32_t aggIndex = p * (k_fattree / 2) + i; // Pod p 的第 i 个 Aggregation Switch
                    NodeContainer link(coreSwitches.Get(coreIndex), aggregationSwitches.Get(aggIndex));
                    NetDeviceContainer devs = p2pHelper.Install(link);
                    coreDevices[coreIndex]->AddBridgePort(devs.Get(0));
                    aggregationDevices[aggIndex]->AddBridgePort(devs.Get(1));
                }
            }
        }
        // --- 5. 连接：汇聚层 ↔ 边缘层 ---
        for (uint32_t p = 0; p < k_fattree; ++p) {
            for (uint32_t i = 0; i < k_fattree / 2; ++i) {
                for (uint32_t j = 0; j < k_fattree / 2; ++j) {
                    uint32_t aggIndex = p * (k_fattree / 2) + i;
                    uint32_t edgeIndex = p * (k_fattree / 2) + j;
                    NodeContainer link(aggregationSwitches.Get(aggIndex), edgeSwitches.Get(edgeIndex));
                    NetDeviceContainer devs = p2pHelper.Install(link);
                    aggregationDevices[aggIndex]->AddBridgePort(devs.Get(0));
                    edgeDevices[edgeIndex]->AddBridgePort(devs.Get(1));
                }
            }
        }
        // --- 6. 连接：边缘层 ↔ 主机 ---
        std::vector<Ptr<NetDevice>> hostDevices; // 保存主机设备用于后续应用
        std::vector<ns3::Address> gpuaddress;
        
        //没有子网或者用nvswitch
        if (subnet_model==0 or subnet_model==1){
            for (uint32_t p = 0; p < k_fattree; ++p) {
                for (uint32_t i = 0; i < k_fattree / 2; ++i) {
                    for (uint32_t j = 0; j < k_fattree / 2; ++j) {
                        uint32_t edgeIndex = p * (k_fattree / 2) + i;
                        uint32_t hostIndex = p * (k_fattree * k_fattree / 4) + i * (k_fattree / 2) + j;
                        
                        for (uint32_t g=0;g<gpus_per_server;g++){//首先连接外部
                            uint32_t gpuindex=hostIndex*gpus_per_server+g;
                            //创建节点交换器
                            Ptr<MinHopBridgeNetDevice> br = CreateGpuSubnetBridge(hosts.Get(gpuindex),1);
                            SubNetDevices.push_back(br);

                            NodeContainer link(edgeSwitches.Get(edgeIndex), hosts.Get(gpuindex));
                            NetDeviceContainer devs = p2pHelper.Install(link);
                            edgeDevices[edgeIndex]->AddBridgePort(devs.Get(0));
                            hostDevices.push_back(devs.Get(1)); // 主机侧设备
                            gpuaddress.push_back(devs.Get(1)->GetAddress());
                        }
                        //然后创建子网拓扑
                        BuildServerSubnet_NvSwitch(hostIndex,hosts,nvswitch,nvlinkhelper);
                    }
                }
            }
            
        }
        //子网用fullmesh
        if (subnet_model==2){
           for (uint32_t p = 0; p < k_fattree; ++p) {
                for (uint32_t i = 0; i < k_fattree / 2; ++i) {
                    for (uint32_t j = 0; j < k_fattree / 2; ++j) {
                        uint32_t edgeIndex = p * (k_fattree / 2) + i;
                        uint32_t hostIndex = p * (k_fattree * k_fattree / 4) + i * (k_fattree / 2) + j;
                        //计算偏移量，否则因为重新编号的原因会把第一个子网反复构建，后面啥都没有
                        uint32_t subnet_offset=hostIndex*gpus_per_server;
                        for (uint32_t g=0;g<gpus_per_server;g++){//首先连接外部
                            uint32_t gpuindex=hostIndex*gpus_per_server+g;
                            //创建节点交换器
                            Ptr<MinHopBridgeNetDevice> br = CreateGpuSubnetBridge(hosts.Get(gpuindex),1);
                            SubNetDevices.push_back(br);

                            NodeContainer link(edgeSwitches.Get(edgeIndex), hosts.Get(gpuindex));
                            NetDeviceContainer devs = p2pHelper.Install(link);
                            edgeDevices[edgeIndex]->AddBridgePort(devs.Get(0));
                            hostDevices.push_back(devs.Get(1)); // 主机侧设备
                            gpuaddress.push_back(devs.Get(1)->GetAddress());
                        }
                        //然后创建子网拓扑
                        BuildServerSubnet_FullMesh(hostIndex,hosts,nvlinkhelper);
                    }
                }
           }           
        }
        //子网用torus
        if (subnet_model==3){
           for (uint32_t p = 0; p < k_fattree; ++p) {
                for (uint32_t i = 0; i < k_fattree / 2; ++i) {
                    for (uint32_t j = 0; j < k_fattree / 2; ++j) {
                        uint32_t edgeIndex = p * (k_fattree / 2) + i;
                        uint32_t hostIndex = p * (k_fattree * k_fattree / 4) + i * (k_fattree / 2) + j;
                        for (uint32_t g=0;g<gpus_per_server;g++){//首先连接外部
                            uint32_t gpuindex=hostIndex*gpus_per_server+g;
                            //创建节点交换器
                            Ptr<MinHopBridgeNetDevice> br = CreateGpuSubnetBridge(hosts.Get(gpuindex),1);
                            SubNetDevices.push_back(br);

                            NodeContainer link(edgeSwitches.Get(edgeIndex), hosts.Get(gpuindex));
                            NetDeviceContainer devs = p2pHelper.Install(link);
                            edgeDevices[edgeIndex]->AddBridgePort(devs.Get(0));
                            hostDevices.push_back(devs.Get(1)); // 主机侧设备
                            gpuaddress.push_back(devs.Get(1)->GetAddress());
                        }
                        //然后创建子网拓扑
                        BuildServerSubnet_Torus(hostIndex,hosts,nvlinkhelper);
                    }
                }
           }        
        }
        
        // 获取所有主机的 MAC 地址 ---
        std::vector<Mac48Address> hostMacs;
        for (uint32_t i = 0; i < hostDevices.size(); ++i) {
            Mac48Address mac = Mac48Address::ConvertFrom(hostDevices[i]->GetAddress());
            hostMacs.push_back(mac);
        }
        // 配置 PacketSocket 应用层 ---
        PacketSocketHelper packetSocketHelper;
        packetSocketHelper.Install(hosts); // 为所有主机安装 PacketSocket

        if (!mactable_load_enable)
        {
            // 为每个主机安装发往其他主机的 HopCountClient
            uint16_t customEtherType = 0x88B5;
            for (uint32_t i = 0; i < numGPUs; ++i){
                Ptr<Node> clientNode = hosts.Get(i);
                #ifdef NS3_MPI
                if(clientNode->GetSystemId() != MpiInterface::GetSystemId())
                {
                    continue;
                }
                #endif
                for (uint32_t k=0;k<=1;k++){
                    PacketSocketAddress clientSocketAddr;
                    clientSocketAddr.SetSingleDevice(k);
                    clientSocketAddr.SetPhysicalAddress(hostMacs[i]);
                    clientSocketAddr.SetProtocol(customEtherType);

                    Ptr<HopCountClient> client = CreateObject<HopCountClient>();
                    client->SetRemote(clientSocketAddr);
                    client->SetAttribute("PacketSize", UintegerValue(100));
                    client->SetAttribute("Interval", TimeValue(Seconds(1.5)));
                    client->SetAttribute("MaxPackets", UintegerValue(1));
                    hosts.Get(i)->AddApplication(client);

                    double startTime = 1.0 + (i * 0.2);
                    ApplicationContainer clientApps(client);
                    clientApps.Start(Seconds(startTime));
                    clientApps.Stop(Seconds(GetEffectiveFloodTime()-1));
                }
            }
        }
        for (uint32_t i = 0; i < numGPUs; ++i) {
            Ptr<Node> clientNode = hosts.Get(i);
            uint16_t proto = g_roce_cfg.enable ? g_roce_cfg.etherType : 0x9999;
            Ptr<sendflowclient> App = CreateObject<sendflowclient>();
            App->SetRoceConfig(g_roce_cfg);
            App->SetStartTime(Seconds(GetEffectiveFloodTime()-1));
            App->SetStopTime(Seconds(2000000000));
            hosts.Get(i)->AddApplication(App);

            App->initialization(proto,1,0,SubNetDevices[i]);
            
            PacketSocketAddress clientSocketAddr;
            //这个没问题，如果有子网也会统一到本地交换机的地址，也是0号地址
            clientSocketAddr.SetPhysicalAddress(hosts.Get(i)->GetDevice(0)->GetAddress());
            clientSocketAddr.SetProtocol(proto);

            nodeaddresstable[i]=clientSocketAddr;
            nodeapptable[i]=App;
            nodetable[i] = clientNode;
        }
    }
    
    //Clos
    if (Topology_model==1){
        NodeContainer SpineSwitchs;
        SpineSwitchs.Create(numSpine);
        NodeContainer LeafSwitchs;
        LeafSwitchs.Create(numLeaf);
        NodeContainer hosts;
        hosts.Create(numGPUs);
        NodeContainer nvswitch;
        nvswitch.Create(nvswitch_num);
        
        for (uint32_t i = 0; i < numSpine; ++i) {
            Ptr<MinHopBridgeNetDevice> dev = CreateObject<MinHopBridgeNetDevice>();
            dev->set_maxjump(max_jump);
            dev->set_ecmp_model(ecmpmodel);
            dev->Set_switchDelay(NanoSeconds(switch_delay));
            dev->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(dev);
            SpineSwitchs.Get(i)->AddDevice(dev);
            SpineDevices.push_back(dev);
        }
        for (uint32_t i = 0; i < numLeaf; ++i) {
            Ptr<MinHopBridgeNetDevice> dev = CreateObject<MinHopBridgeNetDevice>();
            dev->set_maxjump(max_jump);
            dev->set_ecmp_model(ecmpmodel);
            dev->Set_switchDelay(NanoSeconds(switch_delay));
            dev->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(dev);
            LeafSwitchs.Get(i)->AddDevice(dev);
            LeafDevices.push_back(dev);
        }
        for (uint32_t i = 0; i < nvswitch.GetN(); ++i) {
            Ptr<MinHopBridgeNetDevice> nvs = CreateObject<MinHopBridgeNetDevice>();
            nvs->set_ecmp_model(ecmpmodel);
            nvs->Set_switchDelay(Seconds(0));
            nvs->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(nvs);
            nvswitch.Get(i)->AddDevice(nvs);
            nvswitchDevices.push_back(nvs);
        }

        PointToPointHelper p2pHelper;
        p2pHelper.SetDeviceAttribute("DataRate", StringValue(normallink_rate));//链路带宽
        p2pHelper.SetChannelAttribute("Delay", StringValue(normallink_delay));
        // p2pHelper.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(DropTailQueue_size)); 
        p2pHelper.SetQueue("ns3::PppRoundRobinQueue", 
                       "MaxPacketsPerQueue", StringValue(DropTailQueue_size),
                       "NumQueues", StringValue(DropTailQueue_num));
        p2pHelper.SetDeviceAttribute("Mtu", UintegerValue(m_mtu)); // 支持 30KB 帧
        PointToPointHelper nvlinkhelper;
        nvlinkhelper.SetDeviceAttribute("DataRate", StringValue(nvlink_rate));
        nvlinkhelper.SetChannelAttribute("Delay", StringValue(nvlink_delay));//存疑
        // nvlinkhelper.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(DropTailQueue_size)); 
        nvlinkhelper.SetQueue("ns3::PppRoundRobinQueue", 
                       "MaxPacketsPerQueue", StringValue(DropTailQueue_size),
                       "NumQueues", StringValue(DropTailQueue_num));
        nvlinkhelper.SetDeviceAttribute("Mtu", UintegerValue(m_mtu)); 

        for (uint32_t i=0;i<numSpine;i++){
            uint32_t spineindex=i;
            for (uint32_t j=0;j<numLeaf;j++){
                uint32_t leafindex=j;
                NodeContainer link(SpineSwitchs.Get(spineindex), LeafSwitchs.Get(leafindex));
                NetDeviceContainer devs = p2pHelper.Install(link);
                SpineDevices[spineindex]->AddBridgePort(devs.Get(0));
                LeafDevices[leafindex]->AddBridgePort(devs.Get(1));
            }
        }
        
        std::vector<Ptr<NetDevice>> hostDevices; // 保存主机设备用于后续应用
        std::vector<ns3::Address> gpuaddress;
        //没有子网或者用nvswitch
        if (subnet_model==0 or subnet_model==1){
            for (uint32_t i=0;i<numLeaf;i++){
                uint32_t leafindex=i;
                for (uint32_t j=0;j<hostperleaf;j++){
                    uint32_t hostIndex=leafindex*hostperleaf+j;
                    for (uint32_t g=0;g<gpus_per_server;g++){//首先连接外部
                        uint32_t gpuindex=hostIndex*gpus_per_server+g;
                        //创建节点交换器
                        Ptr<MinHopBridgeNetDevice> br = CreateGpuSubnetBridge(hosts.Get(gpuindex),1);
                        SubNetDevices.push_back(br);
                        NodeContainer link(LeafSwitchs.Get(leafindex), hosts.Get(gpuindex));
                        NetDeviceContainer devs = p2pHelper.Install(link);
                        LeafDevices[leafindex]->AddBridgePort(devs.Get(0));
                        hostDevices.push_back(devs.Get(1)); // 主机侧设备
                        gpuaddress.push_back(devs.Get(1)->GetAddress());
                    }
                    //然后创建子网拓扑
                    BuildServerSubnet_NvSwitch(hostIndex,hosts,nvswitch,nvlinkhelper);
                }
            }
        }
        //子网用fullmesh
        if (subnet_model==2){
            for (uint32_t i=0;i<numLeaf;i++){
                uint32_t leafindex=i;
                for (uint32_t j=0;j<hostperleaf;j++){
                    uint32_t hostIndex=leafindex*hostperleaf+j;
                    for (uint32_t g=0;g<gpus_per_server;g++){//首先连接外部
                        uint32_t gpuindex=hostIndex*gpus_per_server+g;
                        //创建节点交换器
                        Ptr<MinHopBridgeNetDevice> br = CreateGpuSubnetBridge(hosts.Get(gpuindex),1);
                        SubNetDevices.push_back(br);

                        NodeContainer link(LeafSwitchs.Get(leafindex), hosts.Get(gpuindex));
                        NetDeviceContainer devs = p2pHelper.Install(link);
                        LeafDevices[leafindex]->AddBridgePort(devs.Get(0));
                        hostDevices.push_back(devs.Get(1)); // 主机侧设备
                        gpuaddress.push_back(devs.Get(1)->GetAddress());
                    }
                    //然后创建子网拓扑
                    BuildServerSubnet_FullMesh(hostIndex,hosts,nvlinkhelper);
                }
            }
        }
        //子网用torus
        if (subnet_model==3){
            for (uint32_t i=0;i<numLeaf;i++){
                uint32_t leafindex=i;
                for (uint32_t j=0;j<hostperleaf;j++){
                    uint32_t hostIndex=leafindex*hostperleaf+j;
                    for (uint32_t g=0;g<gpus_per_server;g++){//首先连接外部
                        uint32_t gpuindex=hostIndex*gpus_per_server+g;
                        //创建节点交换器
                        Ptr<MinHopBridgeNetDevice> br = CreateGpuSubnetBridge(hosts.Get(gpuindex),subnet_k);
                        SubNetDevices.push_back(br);

                        NodeContainer link(LeafSwitchs.Get(leafindex), hosts.Get(gpuindex));
                        NetDeviceContainer devs = p2pHelper.Install(link);
                        LeafDevices[leafindex]->AddBridgePort(devs.Get(0));
                        hostDevices.push_back(devs.Get(1)); // 主机侧设备
                        gpuaddress.push_back(devs.Get(1)->GetAddress());
                    }
                    //然后创建子网拓扑
                    BuildServerSubnet_Torus(hostIndex,hosts,nvlinkhelper);
                }
            }    
        }
        
         // 获取所有主机的 MAC 地址 ---
        std::vector<Mac48Address> hostMacs;
        for (uint32_t i = 0; i < hostDevices.size(); ++i) {
            Mac48Address mac = Mac48Address::ConvertFrom(hostDevices[i]->GetAddress());
            hostMacs.push_back(mac);
        }
        // --- 8. 配置 PacketSocket 应用层 ---
        PacketSocketHelper packetSocketHelper;
        packetSocketHelper.Install(hosts); // 为所有主机安装 PacketSocket


        if (!mactable_load_enable)
        {
            // 为每个主机安装发往其他主机的 HopCountClient
            uint16_t customEtherType = 0x88B5;
            for (uint32_t i = 0; i < numGPUs; ++i){
                Ptr<Node> clientNode = hosts.Get(i);
                #ifdef NS3_MPI
                if(clientNode->GetSystemId() != MpiInterface::GetSystemId())
                {
                    continue;
                }
                #endif
                for (uint32_t k=0;k<=1;k++){
                    PacketSocketAddress clientSocketAddr;
                    clientSocketAddr.SetSingleDevice(k);
                    clientSocketAddr.SetPhysicalAddress(hostMacs[i]);
                    clientSocketAddr.SetProtocol(customEtherType);

                    Ptr<HopCountClient> client = CreateObject<HopCountClient>();
                    client->SetRemote(clientSocketAddr);
                    client->SetAttribute("PacketSize", UintegerValue(100));
                    client->SetAttribute("Interval", TimeValue(Seconds(1.5)));
                    client->SetAttribute("MaxPackets", UintegerValue(1));
                    hosts.Get(i)->AddApplication(client);

                    double startTime = 1.0 + (i * 0.2);
                    ApplicationContainer clientApps(client);
                    clientApps.Start(Seconds(startTime));
                    clientApps.Stop(Seconds(GetEffectiveFloodTime()-1));
                }
            }
        }

        for (uint32_t i = 0; i < numGPUs; ++i) {
            Ptr<Node> clientNode = hosts.Get(i);
            uint16_t proto = g_roce_cfg.enable ? g_roce_cfg.etherType : 0x9999;
            Ptr<sendflowclient> App = CreateObject<sendflowclient>();
            App->SetRoceConfig(g_roce_cfg);
            App->SetStartTime(Seconds(GetEffectiveFloodTime()-1));
            App->SetStopTime(Seconds(2000000000));
            hosts.Get(i)->AddApplication(App);

            App->initialization(proto,1,0,SubNetDevices[i]);
            
            PacketSocketAddress clientSocketAddr;
            //这个没问题，如果有子网也会统一到本地交换机的地址，也是0号地址
            clientSocketAddr.SetPhysicalAddress(hosts.Get(i)->GetDevice(0)->GetAddress());
            clientSocketAddr.SetProtocol(proto);

            nodeaddresstable[i]=clientSocketAddr;
            nodeapptable[i]=App;
            nodetable[i] = clientNode;
        }
    }
    
    //fullmesh和torus改了没测试过
    //fullmesh
    if (Topology_model==2){
        NodeContainer hosts;
        hosts.Create(numGPUs);

        #ifdef NS3_MPI
        uint32_t numRanks = MpiInterface::GetSize();
        uint32_t gpuPerRank = numGPUs/numRanks;
        for(uint32_t i = 0; i < numGPUs ; i++){
            uint32_t targetRank = i / gpuPerRank;
            hosts.Get(i) -> SetSystemId(targetRank);
        }        
        #endif

        PointToPointHelper p2pHelper;
        p2pHelper.SetDeviceAttribute("DataRate", StringValue(normallink_rate));//链路带宽
        p2pHelper.SetChannelAttribute("Delay", StringValue(normallink_delay));
        // p2pHelper.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(DropTailQueue_size)); 
        p2pHelper.SetQueue("ns3::PppRoundRobinQueue", 
                       "MaxPacketsPerQueue", StringValue(DropTailQueue_size),
                       "NumQueues", StringValue(DropTailQueue_num));
        p2pHelper.SetDeviceAttribute("Mtu", UintegerValue(m_mtu)); // 支持 30KB 帧

        //创建内部bridge
        for (uint32_t i = 0; i < num_meshswitch; ++i) {
            Ptr<MinHopBridgeNetDevice> dev = CreateObject<MinHopBridgeNetDevice>();
            dev->set_maxjump(1);
            dev->set_ecmp_model(ecmpmodel);
            dev->Set_switchDelay(NanoSeconds(switch_delay));
            dev->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(dev);
            hosts.Get(i)->AddDevice(dev);
            FullmeshDevices.push_back(dev);
        }
        //构建拓扑
        for (uint32_t i=0;i<num_meshswitch-1;i++){
            for (uint32_t j=i+1;j<num_meshswitch;j++){
                if (i==j) continue;
                NodeContainer link(hosts.Get(i), hosts.Get(j));
                NetDeviceContainer devs = p2pHelper.Install(link);
                FullmeshDevices[i]->AddBridgePort(devs.Get(0));
                FullmeshDevices[j]->AddBridgePort(devs.Get(1));
            }
        }
        // 统一地址
        for (uint32_t g=0;g<numGPUs;g++){
            uint32_t gpuindex=g;
            NormalizeGpuMacsToBridge(hosts.Get(gpuindex),FullmeshDevices[gpuindex]);
        }
        // 获取所有主机的 MAC 地址 ---
        std::vector<Mac48Address> hostMacs;
        for (uint32_t i=0;i<numGPUs;i++){
            Mac48Address mac = Mac48Address::ConvertFrom(FullmeshDevices[i]->GetAddress());
            hostMacs.push_back(mac);
        }

        // 配置 PacketSocket 应用层 ---
        PacketSocketHelper packetSocketHelper;
        packetSocketHelper.Install(hosts); // 为所有主机安装 PacketSocket


        if (!mactable_load_enable)
        {
            // 为每个主机安装发往其他主机的 HopCountClient
            uint16_t customEtherType = 0x88B5;
            for (uint32_t i = 0; i < numGPUs; ++i){
                #ifdef NS3_MPI
                if(hosts.Get(i)->GetSystemId() != MpiInterface::GetSystemId())
                {
                    continue;
                }
                #endif
                PacketSocketAddress clientSocketAddr;
                clientSocketAddr.SetSingleDevice(0);
                clientSocketAddr.SetPhysicalAddress(hostMacs[i]);
                clientSocketAddr.SetProtocol(customEtherType);
                Ptr<HopCountClient> client = CreateObject<HopCountClient>();
                client->SetRemote(clientSocketAddr);
                client->SetAttribute("PacketSize", UintegerValue(100));
                client->SetAttribute("Interval", TimeValue(Seconds(1.5)));
                client->SetAttribute("MaxPackets", UintegerValue(1));
                hosts.Get(i)->AddApplication(client);

                double startTime = 1.0 + (i * 0.2);
                ApplicationContainer clientApps(client);
                clientApps.Start(Seconds(startTime));
                clientApps.Stop(Seconds(GetEffectiveFloodTime()-1));
            }
        }
        for (uint32_t i = 0; i < numGPUs; ++i) {
            Ptr<Node> clientNode = hosts.Get(i);
            uint16_t proto = g_roce_cfg.enable ? g_roce_cfg.etherType : 0x9999;
            Ptr<sendflowclient> App = CreateObject<sendflowclient>();
            App->SetRoceConfig(g_roce_cfg);
            App->SetStartTime(Seconds(GetEffectiveFloodTime()-1));
            App->SetStopTime(Seconds(2000000000));
            hosts.Get(i)->AddApplication(App);

            App->initialization(proto,0,SubNetDevices[i]);

            PacketSocketAddress clientSocketAddr;
            clientSocketAddr.SetPhysicalAddress(hosts.Get(i)->GetDevice(0)->GetAddress());
            clientSocketAddr.SetProtocol(proto);

            nodeaddresstable[i]=clientSocketAddr;
            nodeapptable[i]=App;
            nodetable[i] = clientNode;
        }
    }
    
    //torus
    if (Topology_model==3){
        NodeContainer TorusSwitch;
        TorusSwitch.Create(num_torusswitch);
  
        PointToPointHelper p2pHelper;
        p2pHelper.SetDeviceAttribute("DataRate", StringValue(normallink_rate));//链路带宽
        p2pHelper.SetChannelAttribute("Delay", StringValue(normallink_delay));
        // p2pHelper.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(DropTailQueue_size)); 
        p2pHelper.SetQueue("ns3::PppRoundRobinQueue", 
                       "MaxPacketsPerQueue", StringValue(DropTailQueue_size),
                       "NumQueues", StringValue(DropTailQueue_num));
        p2pHelper.SetDeviceAttribute("Mtu", UintegerValue(m_mtu)); // 支持 30KB 帧

        //创建内部bridge
        for (uint32_t i = 0; i < num_torusswitch; ++i) {
            Ptr<MinHopBridgeNetDevice> dev = CreateObject<MinHopBridgeNetDevice>();
            dev->set_maxjump(k_torus);
            dev->set_ecmp_model(ecmpmodel);
            dev->Set_switchDelay(NanoSeconds(switch_delay));
            dev->SetMtu(m_mtu);
            ApplyBridgePfcWatermarks(dev);
            TorusSwitch.Get(i)->AddDevice(dev);
            TorusDevices.push_back(dev);
        }
        //构建拓扑
        for (uint32_t x=0;x<torus_x;x++){
            for (uint32_t y=0;y<torus_y;y++){
                uint32_t idnow=x*torus_y+y;
                uint32_t id_right=x*torus_y+((y+1)%torus_y);
                uint32_t id_down=((x+1)%torus_x)*torus_y+y;

                NodeContainer link_x(TorusSwitch.Get(idnow), TorusSwitch.Get(id_right));
                NetDeviceContainer devs_x = p2pHelper.Install(link_x);
                TorusDevices[idnow]->AddBridgePort(devs_x.Get(0));
                TorusDevices[id_right]->AddBridgePort(devs_x.Get(1));

                NodeContainer link_y(TorusSwitch.Get(idnow), TorusSwitch.Get(id_down));
                NetDeviceContainer devs_y = p2pHelper.Install(link_y);
                TorusDevices[idnow]->AddBridgePort(devs_y.Get(0));
                TorusDevices[id_down]->AddBridgePort(devs_y.Get(1));
            }
        }
        // 统一地址
        for (uint32_t g=0;g<numGPUs;g++){
            uint32_t gpuindex=g;
            NormalizeGpuMacsToBridge(TorusSwitch.Get(gpuindex),TorusDevices[gpuindex]);
        }
        // 获取所有主机的 MAC 地址 ---
        std::vector<Mac48Address> hostMacs;
        for (uint32_t i=0;i<numGPUs;i++){
            Mac48Address mac = Mac48Address::ConvertFrom(TorusDevices[i]->GetAddress());
            hostMacs.push_back(mac);
        }
        
        // 配置 PacketSocket 应用层 ---
        PacketSocketHelper packetSocketHelper;
        packetSocketHelper.Install(TorusSwitch); // 为所有主机安装 PacketSocket


        if (!mactable_load_enable)
        {
            // 为每个主机安装发往其他主机的 HopCountClient
            uint16_t customEtherType = 0x88B5;
            for (uint32_t i = 0; i < numGPUs; ++i){
                PacketSocketAddress clientSocketAddr;
                clientSocketAddr.SetSingleDevice(0);
                clientSocketAddr.SetPhysicalAddress(hostMacs[i]);
                clientSocketAddr.SetProtocol(customEtherType);
                Ptr<HopCountClient> client = CreateObject<HopCountClient>();
                client->SetRemote(clientSocketAddr);
                client->SetAttribute("PacketSize", UintegerValue(100));
                client->SetAttribute("Interval", TimeValue(Seconds(1.5)));
                client->SetAttribute("MaxPackets", UintegerValue(1));
                TorusSwitch.Get(i)->AddApplication(client);

                double startTime = 1.0 + (i * 0.2);
                ApplicationContainer clientApps(client);
                clientApps.Start(Seconds(startTime));
                clientApps.Stop(Seconds(GetEffectiveFloodTime()-1));
            }
        }
        for (uint32_t i = 0; i < numGPUs; ++i) {
            Ptr<Node> clientNode = TorusSwitch.Get(i);
            uint16_t proto = g_roce_cfg.enable ? g_roce_cfg.etherType : 0x9999;
            Ptr<sendflowclient> App = CreateObject<sendflowclient>();
            App->SetRoceConfig(g_roce_cfg);
            App->SetStartTime(Seconds(GetEffectiveFloodTime()-1));
            App->SetStopTime(Seconds(2000000000));
            TorusSwitch.Get(i)->AddApplication(App);

            App->initialization(proto,0,SubNetDevices[i]);
            
            PacketSocketAddress clientSocketAddr;
            clientSocketAddr.SetPhysicalAddress(TorusSwitch.Get(i)->GetDevice(0)->GetAddress());
            clientSocketAddr.SetProtocol(proto);

            // nodeaddresstable[i]=clientSocketAddr;
            // nodeapptable[i]=App;
            // nodetable[i] = clientNode;

            if (k_torus==4){
                nodeaddresstable[i]=clientSocketAddr;
                nodeapptable[i]=App;
            }
            else {
                if (i>=0 and i<=3){
                    nodeaddresstable[i]=clientSocketAddr;
                    nodeapptable[i]=App;
                }
                if (i>=8 and i<=11){
                    nodeaddresstable[i-4]=clientSocketAddr;
                    nodeapptable[i-4]=App;
                }
                if (i>=16 and i<=19){
                    nodeaddresstable[i-8]=clientSocketAddr;
                    nodeapptable[i-8]=App;
                }
                if (i>=24 and i<=27){
                    nodeaddresstable[i-12]=clientSocketAddr;
                    nodeapptable[i-12]=App;
                }
            }
        }
    }

    //test(仅调试用)
    if (Topology_model==4){
        
    }
}


void printlearnstate(){
    NS_LOG_INFO("--------- 打印所有 主机 的地址 ---------");
    if (Topology_model==0 or Topology_model==1){
        for (auto dev : SubNetDevices) dev->Print_LocalMacAddress();
    }

    NS_LOG_INFO("--------- 打印所有 MinHopBridgeNetDevice 的学习状态 ---------");
    if (Topology_model==0){//FatTree
        for (auto dev : coreDevices) dev->PrintLearnedState();
        for (auto dev : aggregationDevices) dev->PrintLearnedState();
        for (auto dev : edgeDevices) dev->PrintLearnedState();
        // for (auto dev : SubNetDevices) dev->PrintLearnedState();
        for (auto dev : nvswitchDevices) dev->PrintLearnedState();
    }
    if (Topology_model==1){//Clos
        for (auto dev : SpineDevices) dev->PrintLearnedState();
        for (auto dev : LeafDevices) dev->PrintLearnedState();
    }
    if (Topology_model==2){//FullMesh
        for (auto dev : FullmeshDevices) dev->PrintLearnedState();
    }
    if (Topology_model==3){//Torus
        for (auto dev : TorusDevices) dev->PrintLearnedState();
    }
    if (Topology_model==4){//Test
        
    }
    
    
    NS_LOG_INFO("--------------------------------------------------");
}





#endif
