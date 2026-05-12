#!/usr/bin/env python3
"""Generate MinHopBridgeNetDevice MAC tables for FatTree + fullmesh subnet.

The generator mirrors the topology construction order in scratch/my_common.h
for:
  topology.model = fattree
  topology.subnet_model = 2  # per-server fullmesh

For large runs, GPU external bridge tables are omitted by default. Those
bridges have exactly one bridge port in this topology, and the runtime disables
learning on them, so local injection still resolves to the only external port
without requiring a table.

Script-generated tables are written under a scenario directory with a
"-python" suffix, so they can be compared with flood-learned dumps that use
the runtime "-flood" suffix.
"""

from __future__ import annotations

import argparse
import filecmp
import json
import os
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Optional


@dataclass(frozen=True)
class TopologyConfig:
    k: int
    subnet_k: int
    output_root: Path

    @property
    def half(self) -> int:
        return self.k // 2

    @property
    def gpus_per_server(self) -> int:
        return self.subnet_k

    @property
    def nvswitch_per_server(self) -> int:
        return 1

    @property
    def num_hosts(self) -> int:
        return self.k * self.k * self.k // 4

    @property
    def num_cores(self) -> int:
        return self.half * self.half

    @property
    def num_aggs(self) -> int:
        return self.k * self.half

    @property
    def num_edges(self) -> int:
        return self.k * self.half

    @property
    def num_gpus(self) -> int:
        return self.num_hosts * self.gpus_per_server

    @property
    def switch_count(self) -> int:
        return self.num_cores + self.num_aggs + self.num_edges

    @property
    def scenario_tag(self) -> str:
        return (
            f"topo-fattree_k-{self.k}_subnet-2_subnetk-{self.subnet_k}"
            f"_gpusper-{self.gpus_per_server}_nvsper-{self.nvswitch_per_server}"
        )

    @property
    def output_tag(self) -> str:
        return f"{self.scenario_tag}-python"

    @property
    def output_dir(self) -> Path:
        if self.output_root.name == self.output_tag:
            return self.output_root
        if self.output_root.name == self.scenario_tag or self.output_root.name.startswith(f"{self.scenario_tag}-"):
            return self.output_root.with_name(self.output_tag)
        return self.output_root / self.output_tag


@dataclass(frozen=True)
class GpuLocation:
    server: int
    local_gpu: int
    pod: int
    edge: int
    host_col: int


@dataclass(frozen=True)
class BridgeFile:
    node_id: int
    if_index: int
    mac: int
    kind: str
    index: int


def mac_colon(value: int) -> str:
    return ":".join(f"{(value >> shift) & 0xFF:02x}" for shift in (40, 32, 24, 16, 8, 0))


def mac_dash(value: int) -> str:
    return mac_colon(value).replace(":", "-")


def bridge_filename(bridge: BridgeFile) -> str:
    return f"bridge-node-{bridge.node_id}-if-{bridge.if_index}-mac-{mac_dash(bridge.mac)}.tbl"


class FatTreeFullmeshModel:
    def __init__(self, cfg: TopologyConfig) -> None:
        if cfg.k <= 0 or cfg.k % 2 != 0:
            raise ValueError("FatTree k must be a positive even integer")
        if cfg.subnet_k <= 1:
            raise ValueError("subnet_k must be greater than 1 for fullmesh subnet_model=2")

        self.cfg = cfg
        self.core_mac: list[Optional[int]] = [None] * cfg.num_cores
        self.agg_mac: list[Optional[int]] = [None] * cfg.num_aggs
        self.edge_mac: list[Optional[int]] = [None] * cfg.num_edges
        self.gpu_mac: list[Optional[int]] = [None] * cfg.num_gpus
        self._allocation_index = 0
        self._allocate_addresses()

    def _alloc(self) -> int:
        self._allocation_index += 1
        return self._allocation_index

    def _set_first(self, table: list[Optional[int]], index: int, value: int) -> None:
        if table[index] is None:
            table[index] = value

    def _allocate_addresses(self) -> None:
        cfg = self.cfg
        half = cfg.half
        gpus = cfg.gpus_per_server

        # 4. core <-> aggregation
        for row in range(half):
            for col in range(half):
                core = row * half + col
                for pod in range(cfg.k):
                    agg = pod * half + row
                    core_port_mac = self._alloc()
                    agg_port_mac = self._alloc()
                    self._set_first(self.core_mac, core, core_port_mac)
                    self._set_first(self.agg_mac, agg, agg_port_mac)

        # 5. aggregation <-> edge
        for pod in range(cfg.k):
            for row in range(half):
                for edge_in_pod in range(half):
                    agg = pod * half + row
                    edge = pod * half + edge_in_pod
                    agg_port_mac = self._alloc()
                    edge_port_mac = self._alloc()
                    self._set_first(self.agg_mac, agg, agg_port_mac)
                    self._set_first(self.edge_mac, edge, edge_port_mac)

        # 6. edge <-> GPU external bridge, then per-server GPU fullmesh.
        for pod in range(cfg.k):
            for edge_in_pod in range(half):
                edge = pod * half + edge_in_pod
                for host_col in range(half):
                    server = pod * half * half + edge_in_pod * half + host_col
                    for local_gpu in range(gpus):
                        self._alloc()  # edge-side port
                        self._alloc()  # GPU external rail port; normalized later

                    base_gpu = server * gpus
                    for i in range(gpus - 1):
                        for j in range(i + 1, gpus):
                            mac_i = self._alloc()
                            mac_j = self._alloc()
                            self._set_first(self.gpu_mac, base_gpu + i, mac_i)
                            self._set_first(self.gpu_mac, base_gpu + j, mac_j)

        self._assert_all_macs()

    def _assert_all_macs(self) -> None:
        missing = [
            ("core", i) for i, mac in enumerate(self.core_mac) if mac is None
        ] + [
            ("agg", i) for i, mac in enumerate(self.agg_mac) if mac is None
        ] + [
            ("edge", i) for i, mac in enumerate(self.edge_mac) if mac is None
        ] + [
            ("gpu", i) for i, mac in enumerate(self.gpu_mac) if mac is None
        ]
        if missing:
            preview = ", ".join(f"{kind}:{idx}" for kind, idx in missing[:8])
            raise RuntimeError(f"internal error: missing bridge MAC(s): {preview}")

    def gpu_location(self, gpu: int) -> GpuLocation:
        cfg = self.cfg
        half = cfg.half
        gpus = cfg.gpus_per_server
        server = gpu // gpus
        local_gpu = gpu % gpus
        pod = server // (half * half)
        server_in_pod = server % (half * half)
        edge = server_in_pod // half
        host_col = server_in_pod % half
        return GpuLocation(server, local_gpu, pod, edge, host_col)

    def source_macs(self, source_limit: int) -> Iterator[tuple[int, int, GpuLocation]]:
        for gpu in range(source_limit):
            mac = self.gpu_mac[gpu]
            if mac is None:
                raise RuntimeError(f"missing GPU MAC for gpu={gpu}")
            yield gpu, mac, self.gpu_location(gpu)

    def bridge_files(self) -> Iterator[BridgeFile]:
        cfg = self.cfg

        for core, mac in enumerate(self.core_mac):
            yield BridgeFile(core, 0, self._mac_value(mac), "core", core)

        agg_base = cfg.num_cores
        for agg, mac in enumerate(self.agg_mac):
            yield BridgeFile(agg_base + agg, 0, self._mac_value(mac), "agg", agg)

        edge_base = cfg.num_cores + cfg.num_aggs
        for edge, mac in enumerate(self.edge_mac):
            yield BridgeFile(edge_base + edge, 0, self._mac_value(mac), "edge", edge)

        gpu_base = cfg.switch_count
        for gpu, mac in enumerate(self.gpu_mac):
            value = self._mac_value(mac)
            yield BridgeFile(gpu_base + gpu, 0, value, "gpu-inner", gpu)
            yield BridgeFile(gpu_base + gpu, 1, value, "gpu-external", gpu)

    def _mac_value(self, mac: Optional[int]) -> int:
        if mac is None:
            raise RuntimeError("internal error: missing MAC")
        return mac

    def paths_for(self, bridge: BridgeFile, source_gpu: int, loc: GpuLocation) -> list[tuple[int, int]]:
        cfg = self.cfg
        half = cfg.half
        gpus = cfg.gpus_per_server

        if bridge.kind == "core":
            return [(loc.pod, 3)]

        if bridge.kind == "agg":
            agg_pod = bridge.index // half
            if loc.pod == agg_pod:
                return [(half + loc.edge, 2)]
            return [(port, 4) for port in range(half)]

        if bridge.kind == "edge":
            edge_pod = bridge.index // half
            edge_in_pod = bridge.index % half
            if loc.pod == edge_pod and loc.edge == edge_in_pod:
                port = half + loc.host_col * gpus + loc.local_gpu
                return [(port, 1)]
            if loc.pod == edge_pod:
                return [(port, 3) for port in range(half)]
            return [(port, 5) for port in range(half)]

        if bridge.kind == "gpu-inner":
            owner = self.gpu_location(bridge.index)
            if loc.server != owner.server or source_gpu == bridge.index:
                return []
            port = loc.local_gpu if loc.local_gpu < owner.local_gpu else loc.local_gpu - 1
            return [(port, 2)]

        if bridge.kind == "gpu-external":
            if source_gpu == bridge.index:
                return []
            owner = self.gpu_location(bridge.index)
            if loc.pod == owner.pod and loc.edge == owner.edge:
                hop = 2
            elif loc.pod == owner.pod:
                hop = 4
            else:
                hop = 6
            return [(0, hop)]

        raise RuntimeError(f"unknown bridge kind: {bridge.kind}")


def load_config(path: Path) -> tuple[int, int, Path]:
    if not path.exists():
        return 32, 8, Path("mactable")

    with path.open("r", encoding="utf-8") as fp:
        data = json.load(fp)

    topology = data.get("topology", {})
    if topology.get("model", "fattree") != "fattree" and topology.get("model") != 0:
        raise ValueError("this generator only supports topology.model=fattree")
    if int(topology.get("subnet_model", 2)) != 2:
        raise ValueError("this generator only supports topology.subnet_model=2 (fullmesh)")

    mactable = data.get("mactable", {})
    output_root = Path(mactable.get("dir") or mactable.get("load_dir") or mactable.get("dump_dir") or "mactable")
    return int(topology.get("k", 32)), int(topology.get("subnet_k", 8)), output_root


def write_table(
    path: Path,
    bridge_mac: int,
    entries: Iterable[tuple[int, list[tuple[int, int]]]],
    entry_count: int,
) -> None:
    tmp_path = path.with_suffix(path.suffix + ".tmp")
    with tmp_path.open("w", encoding="ascii", newline="\n") as fp:
        fp.write("MINHOP_MACTABLE_V1\n")
        fp.write(f"bridge_mac {mac_colon(bridge_mac)}\n")
        fp.write(f"entries {entry_count}\n")
        for source_mac, paths in entries:
            fp.write(f"mac {mac_colon(source_mac)} paths {len(paths)}\n")
            for port, hop in paths:
                fp.write(f"port {port} hop {hop}\n")
    os.replace(tmp_path, path)


def count_entries_for_bridge(
    cfg: TopologyConfig,
    bridge: BridgeFile,
    source_indices: list[int],
    source_index_set: set[int],
    external_mode: str,
) -> int:
    if bridge.kind == "gpu-external" and external_mode == "none":
        return 0
    if bridge.kind == "gpu-external" and external_mode == "empty":
        return 0
    if bridge.kind == "gpu-external":
        return len(source_indices) - (1 if bridge.index in source_index_set else 0)
    if bridge.kind == "gpu-inner":
        base = (bridge.index // cfg.gpus_per_server) * cfg.gpus_per_server
        return sum(1 for gpu in range(base, base + cfg.gpus_per_server)
                   if gpu != bridge.index and gpu in source_index_set)
    return len(source_indices)


def entries_for_bridge(
    model: FatTreeFullmeshModel,
    bridge: BridgeFile,
    sources: list[tuple[int, int, GpuLocation]],
    source_index_set: set[int],
    external_mode: str,
) -> Iterator[tuple[int, list[tuple[int, int]]]]:
    cfg = model.cfg

    if bridge.kind == "gpu-external" and external_mode == "none":
        return

    if bridge.kind == "gpu-external" and external_mode == "empty":
        return

    if bridge.kind == "gpu-inner":
        base = (bridge.index // cfg.gpus_per_server) * cfg.gpus_per_server
        for source_gpu in range(base, base + cfg.gpus_per_server):
            if source_gpu == bridge.index or source_gpu not in source_index_set:
                continue
            source_mac = model.gpu_mac[source_gpu]
            if source_mac is None:
                raise RuntimeError(f"missing GPU MAC for gpu={source_gpu}")
            paths = model.paths_for(bridge, source_gpu, model.gpu_location(source_gpu))
            if paths:
                yield source_mac, paths
        return

    for source_gpu, source_mac, loc in sources:
        if bridge.kind == "gpu-external" and source_gpu == bridge.index:
            continue
        paths = model.paths_for(bridge, source_gpu, loc)
        if paths:
            yield source_mac, paths


def generate_tables(
    cfg: TopologyConfig,
    source_indices: list[int],
    external_mode: str,
    overwrite: bool,
    progress_every: int,
) -> tuple[int, int, int]:
    model = FatTreeFullmeshModel(cfg)
    out_dir = cfg.output_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    written = 0
    skipped = 0
    removed = 0
    source_index_set = set(source_indices)
    sources = []
    for source_gpu in source_indices:
        if source_gpu < 0 or source_gpu >= cfg.num_gpus:
            raise ValueError(f"source GPU index out of range: {source_gpu}")
        source_mac = model.gpu_mac[source_gpu]
        if source_mac is None:
            raise RuntimeError(f"missing GPU MAC for gpu={source_gpu}")
        sources.append((source_gpu, source_mac, model.gpu_location(source_gpu)))

    for bridge in model.bridge_files():
        filename = out_dir / bridge_filename(bridge)
        if bridge.kind == "gpu-external" and external_mode == "none":
            if filename.exists():
                filename.unlink()
                removed += 1
            continue
        if filename.exists() and not overwrite:
            skipped += 1
            continue

        entry_count = count_entries_for_bridge(
            cfg, bridge, source_indices, source_index_set, external_mode
        )
        write_table(
            filename,
            bridge.mac,
            entries_for_bridge(model, bridge, sources, source_index_set, external_mode),
            entry_count,
        )

        written += 1
        if progress_every and written % progress_every == 0:
            print(f"[progress] written={written} skipped={skipped} dir={out_dir}", file=sys.stderr)

    return written, skipped, removed


def compare_against_directory(
    cfg: TopologyConfig,
    source_indices: list[int],
    external_mode: str,
    compare_dir: Path,
) -> int:
    with tempfile.TemporaryDirectory(prefix="mactable-compare-") as tmp:
        tmp_root = Path(tmp)
        temp_cfg = TopologyConfig(cfg.k, cfg.subnet_k, tmp_root)
        generate_tables(temp_cfg, source_indices, external_mode, overwrite=True, progress_every=0)
        generated_dir = temp_cfg.output_dir

        generated = {p.name: p for p in generated_dir.iterdir() if p.is_file()}
        expected = {p.name: p for p in compare_dir.iterdir() if p.is_file()}

        missing = sorted(expected.keys() - generated.keys())
        extra = sorted(generated.keys() - expected.keys())
        mismatched = sorted(name for name in expected.keys() & generated.keys() if not filecmp.cmp(expected[name], generated[name], shallow=False))

        if missing or extra or mismatched:
            print("[compare] mismatch", file=sys.stderr)
            for label, names in (("missing", missing), ("extra", extra), ("different", mismatched)):
                if names:
                    print(f"  {label}: {len(names)}", file=sys.stderr)
                    for name in names[:10]:
                        print(f"    {name}", file=sys.stderr)
            if mismatched:
                left = generated[mismatched[0]]
                right = expected[mismatched[0]]
                print(f"  first different generated={left}", file=sys.stderr)
                print(f"  first different expected={right}", file=sys.stderr)
            return 1

        print(f"[compare] ok files={len(expected)} dir={compare_dir}")
        return 0


def source_indices_from_dir(cfg: TopologyConfig, source_dir: Path) -> list[int]:
    model = FatTreeFullmeshModel(cfg)
    mac_to_gpu = {mac: gpu for gpu, mac in enumerate(model.gpu_mac) if mac is not None}
    indices: set[int] = set()
    unknown: set[int] = set()

    for file_path in source_dir.glob("bridge-node-*-if-*-mac-*.tbl"):
        with file_path.open("r", encoding="ascii", errors="strict") as fp:
            for line in fp:
                if not line.startswith("mac "):
                    continue
                mac_text = line.split()[1]
                mac_value = int(mac_text.replace(":", ""), 16)
                gpu = mac_to_gpu.get(mac_value)
                if gpu is None:
                    unknown.add(mac_value)
                else:
                    indices.add(gpu)

    if unknown:
        preview = ", ".join(mac_colon(value) for value in sorted(unknown)[:8])
        raise ValueError(f"{source_dir} contains MACs that do not match this topology: {preview}")
    return sorted(indices)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate FatTree k + fullmesh-subnet MinHopBridgeNetDevice MAC tables."
    )
    parser.add_argument("--config", type=Path, default=Path("simconfig/config.json"), help="config.json to read defaults from")
    parser.add_argument("--k", type=int, help="FatTree k; overrides config")
    parser.add_argument("--subnet-k", type=int, help="fullmesh subnet_k / GPUs per server; overrides config")
    parser.add_argument("--output-root", type=Path, help="base mactable dir; scenario tag is appended unless already present")
    parser.add_argument("--source-limit", type=int, help="only emit entries for the first N GPU endpoint MACs")
    parser.add_argument(
        "--source-set-from-dir",
        type=Path,
        help="emit entries only for GPU source MACs present in an existing mactable dump",
    )
    parser.add_argument(
        "--external-mode",
        choices=("none", "empty", "full"),
        default="none",
        help="GPU external bridge tables: none omits/removes them; empty writes compact empty files; full matches flood-learned dumps",
    )
    parser.add_argument("--overwrite", action="store_true", help="overwrite existing .tbl files")
    parser.add_argument("--dry-run", action="store_true", help="print derived sizes and exit")
    parser.add_argument("--compare-dir", type=Path, help="generate to a temp dir and compare with an existing dump directory")
    parser.add_argument("--progress-every", type=int, default=1000, help="progress interval in files; 0 disables")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    k, subnet_k, output_root = load_config(args.config)
    if args.k is not None:
        k = args.k
    if args.subnet_k is not None:
        subnet_k = args.subnet_k
    if args.output_root is not None:
        output_root = args.output_root

    cfg = TopologyConfig(k=k, subnet_k=subnet_k, output_root=output_root)
    source_set_dir: Optional[Path] = args.source_set_from_dir
    if source_set_dir is None and args.compare_dir is not None and args.source_limit is None:
        source_set_dir = args.compare_dir

    if source_set_dir is not None:
        source_indices = source_indices_from_dir(cfg, source_set_dir)
        source_desc = f"{len(source_indices)} from {source_set_dir}"
    else:
        source_limit = args.source_limit if args.source_limit is not None else cfg.num_gpus
        if source_limit < 0 or source_limit > cfg.num_gpus:
            raise ValueError(f"--source-limit must be in [0, {cfg.num_gpus}]")
        source_indices = list(range(source_limit))
        source_desc = str(source_limit)

    total_bridge_files = cfg.switch_count + 2 * cfg.num_gpus
    emitted_bridge_files = cfg.switch_count + cfg.num_gpus
    if args.external_mode != "none":
        emitted_bridge_files += cfg.num_gpus
    print(
        "[config] "
        f"k={cfg.k} subnet_k={cfg.subnet_k} gpus={cfg.num_gpus} "
        f"switches={cfg.switch_count} bridge_files={total_bridge_files} "
        f"emitted_bridge_files={emitted_bridge_files} "
        f"sources={source_desc} external_mode={args.external_mode} "
        f"output_dir={cfg.output_dir}"
    )

    if args.dry_run:
        return 0

    if args.compare_dir:
        return compare_against_directory(cfg, source_indices, args.external_mode, args.compare_dir)

    if cfg.output_dir.exists() and args.overwrite:
        for stale in cfg.output_dir.glob("bridge-node-*-if-*-mac-*.tbl"):
            stale.unlink()

    written, skipped, removed = generate_tables(
        cfg,
        source_indices=source_indices,
        external_mode=args.external_mode,
        overwrite=args.overwrite,
        progress_every=args.progress_every,
    )
    print(f"[done] written={written} skipped={skipped} removed={removed} dir={cfg.output_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BrokenPipeError:
        raise SystemExit(1)
