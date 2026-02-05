#!/usr/bin/env python3
"""Generate a self-contained HTML traffic animation report.

Default layout for 64 nodes:
- each row has 16 nodes (one DP group)
- each chunk of 4 nodes in a row is one PP group

Flow-class coloring:
- size 2048 -> FWD ALLREDUCE
- size 4096 -> FWD ALLTOALL
- size 128  -> P2P cross-stage
- size 6144 -> BWD ALLREDUCE
- size 8192 -> BWD ALLTOALL
- size 1023 is always ignored
"""

from __future__ import annotations

import argparse
import html
import json
import math
import re
from collections import Counter
from pathlib import Path
from typing import Dict, List, Tuple


# Parsed flow tuple:
# (time_ns, src, dst, tag, size, class_id)
FlowRecord = Tuple[int, int, int, int, int, int]

LINE_RE = re.compile(
    r"time\s*=\s*(\d+)\s+src=(\d+)\s+dst=(\d+)\s+tag=(\d+)\s+size=(\d+)"
)


# Class IDs used both in Python and browser JS.
CLASS_FWD_ALLREDUCE = 0
CLASS_FWD_ALLTOALL = 1
CLASS_P2P = 2
CLASS_BWD_ALLREDUCE = 3
CLASS_BWD_ALLTOALL = 4
CLASS_OTHER = 5


FLOW_CLASS_META = [
    {
        "id": CLASS_FWD_ALLREDUCE,
        "key": "fwd_allreduce",
        "label": "FWD ALLREDUCE (size=2048)",
        "color": "#2563eb",
        "default_on": True,
    },
    {
        "id": CLASS_FWD_ALLTOALL,
        "key": "fwd_alltoall",
        "label": "FWD ALLTOALL (size=4096)",
        "color": "#ef4444",
        "default_on": True,
    },
    {
        "id": CLASS_P2P,
        "key": "p2p",
        "label": "P2P Cross-Stage (size=128)",
        "color": "#10b981",
        "default_on": True,
    },
    {
        "id": CLASS_BWD_ALLREDUCE,
        "key": "bwd_allreduce",
        "label": "BWD ALLREDUCE (size=6144)",
        "color": "#f59e0b",
        "default_on": True,
    },
    {
        "id": CLASS_BWD_ALLTOALL,
        "key": "bwd_alltoall",
        "label": "BWD ALLTOALL (size=8192)",
        "color": "#06b6d4",
        "default_on": True,
    },
    {
        "id": CLASS_OTHER,
        "key": "other",
        "label": "Other Sizes",
        "color": "#6b7280",
        "default_on": False,
    },
]


def classify_size(size: int) -> int:
    if size == 2048:
        return CLASS_FWD_ALLREDUCE
    if size == 4096:
        return CLASS_FWD_ALLTOALL
    if size == 128:
        return CLASS_P2P
    if size == 6144:
        return CLASS_BWD_ALLREDUCE
    if size == 8192:
        return CLASS_BWD_ALLTOALL
    return CLASS_OTHER


def parse_int_set(raw: str) -> set[int]:
    if not raw.strip():
        return set()
    values: set[int] = set()
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        values.add(int(token))
    return values


def parse_flows(
    log_path: Path,
    node_count: int,
    min_size: int,
    excluded_sizes: set[int],
) -> Tuple[List[FlowRecord], Dict[str, int]]:
    flows: List[FlowRecord] = []
    stats = {
        "matched_lines": 0,
        "kept_lines": 0,
        "skipped_by_node": 0,
        "skipped_by_size": 0,
    }
    with log_path.open("r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            match = LINE_RE.search(line)
            if not match:
                continue
            stats["matched_lines"] += 1
            time_ns, src, dst, tag, size = (int(v) for v in match.groups())
            if src >= node_count or dst >= node_count:
                stats["skipped_by_node"] += 1
                continue
            if size < min_size or size in excluded_sizes:
                stats["skipped_by_size"] += 1
                continue
            class_id = classify_size(size)
            flows.append((time_ns, src, dst, tag, size, class_id))
            stats["kept_lines"] += 1
    return flows, stats


def build_layout(node_count: int, row_size: int, pp_size: int) -> Dict[str, object]:
    rows = math.ceil(node_count / row_size)
    pp_per_row = row_size // pp_size

    x0 = 90
    y0 = 90
    x_step = 64
    y_step = 140
    radius = 12
    width = x0 * 2 + (row_size - 1) * x_step + 100
    height = y0 * 2 + (rows - 1) * y_step + 120

    nodes = []
    for node_id in range(node_count):
        row = node_id // row_size
        col = node_id % row_size
        x = x0 + col * x_step
        y = y0 + row * y_step
        nodes.append(
            {
                "id": node_id,
                "row": row,
                "col": col,
                "dp_group": row,
                "pp_group": row * pp_per_row + (col // pp_size),
                "x": x,
                "y": y,
            }
        )

    dp_rects = []
    for row in range(rows):
        y = y0 + row * y_step
        dp_rects.append(
            {
                "x": x0 - 32,
                "y": y - 36,
                "w": (row_size - 1) * x_step + 64,
                "h": 72,
                "label": f"DP{row}",
            }
        )

    pp_rects = []
    for row in range(rows):
        y = y0 + row * y_step
        for g in range(pp_per_row):
            x = x0 + (g * pp_size) * x_step
            pp_rects.append(
                {
                    "x": x - 22,
                    "y": y - 24,
                    "w": (pp_size - 1) * x_step + 44,
                    "h": 48,
                    "label": f"PP{row * pp_per_row + g}",
                }
            )

    return {
        "rows": rows,
        "pp_per_row": pp_per_row,
        "radius": radius,
        "width": width,
        "height": height,
        "nodes": nodes,
        "dp_rects": dp_rects,
        "pp_rects": pp_rects,
    }


def choose_default_window_ns(span_ns: int) -> int:
    if span_ns <= 0:
        return 1
    # Aim for around ~180 frames initially.
    raw = max(1, span_ns // 180)
    power = 10 ** int(math.floor(math.log10(raw)))
    for multiplier in (1, 2, 5, 10):
        candidate = multiplier * power
        if candidate >= raw:
            return candidate
    return raw


def build_report(
    flows: List[FlowRecord],
    node_count: int,
    row_size: int,
    pp_size: int,
    top_k: int,
    window_ns: int,
    step_ns: int,
) -> Dict[str, object]:
    flows.sort(key=lambda x: x[0])

    time_min = flows[0][0]
    time_max = flows[-1][0]
    span_ns = time_max - time_min

    total_bytes = 0
    size_hist: Counter[int] = Counter()
    tag_hist: Counter[int] = Counter()
    class_hist: Counter[int] = Counter()
    pair_set: set[Tuple[int, int]] = set()

    node_send_bytes = [0] * node_count
    node_recv_bytes = [0] * node_count
    node_send_count = [0] * node_count
    node_recv_count = [0] * node_count

    # compact flow format for browser: [rel_t_ns, src, dst, size, class_id]
    compact_flows: List[List[int]] = []
    for t, src, dst, tag, size, class_id in flows:
        rel_t = t - time_min
        compact_flows.append([rel_t, src, dst, size, class_id])

        total_bytes += size
        size_hist[size] += 1
        tag_hist[tag] += 1
        class_hist[class_id] += 1
        pair_set.add((src, dst))

        node_send_bytes[src] += size
        node_recv_bytes[dst] += size
        node_send_count[src] += 1
        node_recv_count[dst] += 1

    layout = build_layout(node_count=node_count, row_size=row_size, pp_size=pp_size)
    nodes = []
    for item in layout["nodes"]:
        node_id = item["id"]
        node = dict(item)
        node.update(
            {
                "global_send_bytes": node_send_bytes[node_id],
                "global_recv_bytes": node_recv_bytes[node_id],
                "global_send_count": node_send_count[node_id],
                "global_recv_count": node_recv_count[node_id],
            }
        )
        nodes.append(node)

    default_window_ns = window_ns if window_ns > 0 else choose_default_window_ns(span_ns)
    default_step_ns = step_ns if step_ns > 0 else default_window_ns
    default_top_k = (
        min(top_k, len(pair_set)) if top_k > 0 else min(120, max(1, len(pair_set)))
    )

    class_hist_list = []
    for meta in FLOW_CLASS_META:
        class_hist_list.append(
            {
                "id": meta["id"],
                "label": meta["label"],
                "count": class_hist.get(meta["id"], 0),
            }
        )

    return {
        "node_count": node_count,
        "row_size": row_size,
        "pp_size": pp_size,
        "dp_group_count": layout["rows"],
        "pp_group_count": layout["rows"] * layout["pp_per_row"],
        "default_top_k": default_top_k,
        "default_window_ns": default_window_ns,
        "default_step_ns": default_step_ns,
        "class_meta": FLOW_CLASS_META,
        "summary": {
            "flow_count": len(flows),
            "pair_count": len(pair_set),
            "total_bytes": total_bytes,
            "size_hist": size_hist.most_common(8),
            "top_tags": tag_hist.most_common(12),
            "class_hist": class_hist_list,
            "time_base_ns_str": str(time_min),
            "time_max_ns_str": str(time_max),
            "time_span_ns": span_ns,
        },
        "layout": {
            "rows": layout["rows"],
            "pp_per_row": layout["pp_per_row"],
            "radius": layout["radius"],
            "width": layout["width"],
            "height": layout["height"],
            "dp_rects": layout["dp_rects"],
            "pp_rects": layout["pp_rects"],
        },
        "nodes": nodes,
        "flows": compact_flows,
    }


def render_html(title: str, report: Dict[str, object], parse_stats: Dict[str, int]) -> str:
    payload = {"report": report, "parse_stats": parse_stats}
    data_json = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    safe_title = html.escape(title)
    template = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>__TITLE__</title>
  <style>
    :root {
      --bg: #f5f8ff;
      --panel: #ffffff;
      --ink: #17233d;
      --sub: #5e6d8b;
      --line: #d0dbef;
      --accent: #0b6bd6;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      color: var(--ink);
      background: radial-gradient(1200px 500px at 50% -120px, #dce9ff 0%, var(--bg) 56%);
    }
    .wrap {
      max-width: 1550px;
      margin: 24px auto;
      padding: 0 16px 28px;
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 12px;
      padding: 14px;
      margin-top: 14px;
      box-shadow: 0 8px 26px rgba(13, 29, 70, 0.06);
    }
    h1 {
      margin: 4px 0 8px;
      font-size: 23px;
      letter-spacing: 0.2px;
    }
    h2 {
      margin: 2px 0 10px;
      font-size: 16px;
    }
    .sub {
      color: var(--sub);
      margin: 0;
      font-size: 13px;
    }
    .kv {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
      gap: 8px;
      font-size: 13px;
      margin-top: 8px;
    }
    .kv > div {
      background: #f7faff;
      border: 1px solid #e1eaf8;
      border-radius: 8px;
      padding: 7px 8px;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .controls {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 10px 14px;
      margin-top: 6px;
      font-size: 13px;
    }
    .ctl {
      display: grid;
      grid-template-columns: 1fr;
      gap: 4px;
    }
    .ctl label { color: var(--sub); }
    .hstack {
      display: flex;
      gap: 8px;
      align-items: center;
      flex-wrap: wrap;
    }
    .hstack button {
      border: 1px solid #c6d3ec;
      background: #f8fbff;
      border-radius: 8px;
      padding: 6px 10px;
      cursor: pointer;
      color: #1c3d72;
      font-family: inherit;
    }
    .hstack button:hover { background: #eef4ff; }
    input[type="range"], select, input[type="number"] {
      width: 100%;
      accent-color: var(--accent);
      border: 1px solid #d4dff0;
      border-radius: 8px;
      padding: 5px 8px;
      font-family: inherit;
      font-size: 13px;
      background: #fff;
    }
    .mono {
      background: #f6f9ff;
      border: 1px solid #e1eaf8;
      border-radius: 6px;
      padding: 2px 6px;
    }
    .class-list {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(210px, 1fr));
      gap: 6px 10px;
    }
    .class-item {
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 4px 0;
      color: #33415f;
    }
    .swatch {
      width: 14px;
      height: 14px;
      border-radius: 3px;
      border: 1px solid #d1d9ea;
      flex: 0 0 auto;
    }
    svg {
      border: 1px solid #d5def0;
      border-radius: 8px;
      background: #fff;
      width: 100%;
      display: block;
    }
    canvas {
      border: 1px solid #d5def0;
      border-radius: 8px;
      background: #fff;
      width: 100%;
      display: block;
      height: 80px;
      cursor: pointer;
    }
    .hint {
      color: var(--sub);
      margin: 8px 0 0;
      font-size: 12px;
    }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>__TITLE__</h1>
    <p class="sub">Animation by time window; size-1023 control traffic is ignored by default.</p>

    <div class="panel">
      <h2>Summary</h2>
      <div id="summary-kv" class="kv"></div>
      <p class="hint" id="summary-extra"></p>
    </div>

    <div class="panel">
      <h2>Animation Controls</h2>
      <div class="controls">
        <div class="ctl">
          <label>Metric</label>
          <div class="mono">Flow Count</div>
        </div>
        <div class="ctl">
          <label for="window-ns">Window (ns)</label>
          <input id="window-ns" type="number" min="1" step="1" />
        </div>
        <div class="ctl">
          <label for="step-ns">Step (ns)</label>
          <input id="step-ns" type="number" min="1" step="1" />
        </div>
        <div class="ctl">
          <label for="topk">Top Links per Frame</label>
          <input id="topk" type="range" min="1" max="500" step="1" />
          <div class="hstack"><span id="topk-value" class="mono"></span></div>
        </div>
        <div class="ctl">
          <label for="fps">Playback FPS</label>
          <input id="fps" type="range" min="1" max="30" step="1" />
          <div class="hstack"><span id="fps-value" class="mono"></span></div>
        </div>
        <div class="ctl">
          <label for="frame">Frame</label>
          <input id="frame" type="range" min="0" max="0" step="1" />
          <div class="hstack"><span id="frame-value" class="mono"></span></div>
        </div>
      </div>
      <div class="hstack" style="margin-top:10px;">
        <button id="btn-play">Play</button>
        <button id="btn-prev">Prev</button>
        <button id="btn-next">Next</button>
        <button id="btn-reset">Reset</button>
        <button id="btn-rebuild">Apply Window/Step</button>
      </div>

      <div style="margin-top:10px;">
        <div class="hint" style="margin:0 0 4px;">Flow Class Filter</div>
        <div id="class-filters" class="class-list"></div>
      </div>

      <p class="hint" id="status"></p>
      <canvas id="timeline"></canvas>
      <p class="hint">Timeline shows per-frame intensity; click to jump to a frame.</p>
    </div>

    <div class="panel">
      <h2>Topology Animation</h2>
      <svg id="topology"></svg>
      <p class="hint">Node color = current-window load (selected classes only). Line color = dominant flow class on that edge in current window.</p>
    </div>
  </div>

  <script>
    const DATA = __DATA__;
    const REPORT = DATA.report;
    const PARSE = DATA.parse_stats;

    const MAX_SAFE = Number.MAX_SAFE_INTEGER;
    const CLASS_META = REPORT.class_meta;
    const CLASS_COUNT = CLASS_META.length;
    const CLASS_BY_ID = new Map(CLASS_META.map((x) => [x.id, x]));
    const CLASS_OTHER_ID = (() => {
      const found = CLASS_META.find((x) => x.key === "other");
      return found ? found.id : CLASS_META[CLASS_META.length - 1].id;
    })();

    const baseNsBig = BigInt(REPORT.summary.time_base_ns_str);
    // flow item: [rel_t_ns, src, dst, size, class_id]
    const flows = REPORT.flows;
    const times = flows.map((f) => f[0]);

    const state = {
      topK: REPORT.default_top_k,
      windowNs: REPORT.default_window_ns,
      stepNs: REPORT.default_step_ns,
      fps: 8,
      frameStarts: [0],
      frameIndex: 0,
      playing: false,
      timer: null,
      timelineMetricVals: [],
      enabledClasses: new Set(CLASS_META.filter((x) => x.default_on).map((x) => x.id))
    };

    function number(v) {
      return new Intl.NumberFormat().format(v);
    }

    function formatBigInt(v) {
      const s = v.toString();
      return s.replace(/\\B(?=(\\d{3})+(?!\\d))/g, ",");
    }

    function absNsFromRel(relNs) {
      if (!Number.isFinite(relNs) || relNs < 0 || relNs > MAX_SAFE) return "n/a";
      return formatBigInt(baseNsBig + BigInt(Math.trunc(relNs)));
    }

    function colorByValue(v, maxV) {
      if (maxV <= 0 || v <= 0) return "#f5f8ff";
      const t = Math.log10(v + 1) / Math.log10(maxV + 1);
      const hue = 215 - Math.round(208 * t);
      const light = 96 - Math.round(50 * t);
      return `hsl(${hue}, 84%, ${light}%)`;
    }

    function lowerBound(arr, target) {
      let l = 0;
      let r = arr.length;
      while (l < r) {
        const m = (l + r) >> 1;
        if (arr[m] < target) l = m + 1;
        else r = m;
      }
      return l;
    }

    function frameWindowStats(frameIndex) {
      const start = state.frameStarts[frameIndex];
      const end = start + state.windowNs;
      const i0 = lowerBound(times, start);
      const i1 = lowerBound(times, end);
      return { start, end, i0, i1 };
    }

    function metricValue(_bytes, count) {
      return count;
    }

    function classMetricForEdge(edge, classId) {
      return edge.byClassCount[classId];
    }

    function dominantClass(edge) {
      let bestClass = CLASS_OTHER_ID;
      let bestVal = -1;
      for (const classId of state.enabledClasses) {
        const v = classMetricForEdge(edge, classId);
        if (v > bestVal) {
          bestVal = v;
          bestClass = classId;
        }
      }
      return bestClass;
    }

    function activeClassLabelList() {
      const labels = [];
      for (const classId of state.enabledClasses) {
        const meta = CLASS_BY_ID.get(classId);
        if (meta) labels.push(meta.label);
      }
      return labels.join(", ");
    }

    function aggregateFrame(frameIndex) {
      const w = frameWindowStats(frameIndex);
      const pairMap = new Map();
      const nodeLoad = new Array(REPORT.node_count).fill(0);
      let windowCount = 0;

      for (let i = w.i0; i < w.i1; i += 1) {
        const f = flows[i];
        const src = f[1];
        const dst = f[2];
        const size = f[3];
        const classId = f[4];
        if (!state.enabledClasses.has(classId)) {
          continue;
        }

        const key = `${src},${dst}`;
        if (!pairMap.has(key)) {
          pairMap.set(key, {
            src,
            dst,
            bytes: 0,
            count: 0,
            byClassBytes: new Array(CLASS_COUNT).fill(0),
            byClassCount: new Array(CLASS_COUNT).fill(0),
          });
        }

        const edge = pairMap.get(key);
        edge.bytes += size;
        edge.count += 1;
        edge.byClassBytes[classId] += size;
        edge.byClassCount[classId] += 1;

        nodeLoad[src] += 1;
        nodeLoad[dst] += 1;
        windowCount += 1;
      }

      const edges = Array.from(pairMap.values());
      edges.sort((a, b) => {
        const ma = metricValue(a.bytes, a.count);
        const mb = metricValue(b.bytes, b.count);
        if (mb !== ma) return mb - ma;
        return b.bytes - a.bytes;
      });

      return {
        start: w.start,
        end: w.end,
        i0: w.i0,
        i1: w.i1,
        edges,
        nodeLoad,
        windowCount,
      };
    }

    function setSummary() {
      const s = REPORT.summary;
      const kv = document.getElementById("summary-kv");
      const items = [
        `Flows kept: ${number(s.flow_count)}`,
        `Unique src->dst pairs: ${number(s.pair_count)}`,
        `Metric mode: Flow Count`,
        `Node count: ${REPORT.node_count}`,
        `DP groups: ${REPORT.dp_group_count}`,
        `PP groups: ${REPORT.pp_group_count}`,
        `Matched lines: ${number(PARSE.matched_lines)}`,
        `Filtered lines: ${number(PARSE.skipped_by_node + PARSE.skipped_by_size)}`
      ];
      kv.innerHTML = items.map((x) => `<div>${x}</div>`).join("");

      const sizeHist = s.size_hist
        .slice(0, 8)
        .map(([sz, cnt]) => `${sz}:${number(cnt)}`)
        .join("  ");
      const classHist = s.class_hist
        .map((x) => `${x.label}:${number(x.count)}`)
        .join(" | ");
      document.getElementById("summary-extra").textContent =
        `time(ns): ${s.time_base_ns_str} -> ${s.time_max_ns_str} | span(ns): ${number(s.time_span_ns)} | size_hist: ${sizeHist} | class_hist: ${classHist}`;
    }

    function buildClassFilters() {
      const root = document.getElementById("class-filters");
      root.innerHTML = "";
      for (const meta of CLASS_META) {
        const item = document.createElement("label");
        item.className = "class-item";

        const cb = document.createElement("input");
        cb.type = "checkbox";
        cb.checked = state.enabledClasses.has(meta.id);
        cb.dataset.classId = String(meta.id);
        cb.addEventListener("change", () => {
          const classId = Number(cb.dataset.classId);
          if (cb.checked) {
            state.enabledClasses.add(classId);
          } else {
            state.enabledClasses.delete(classId);
          }
          if (state.enabledClasses.size === 0) {
            // Keep at least one class enabled to avoid empty UI state.
            state.enabledClasses.add(meta.id);
            cb.checked = true;
          }
          recomputeTimelineMetric();
          render();
        });

        const swatch = document.createElement("span");
        swatch.className = "swatch";
        swatch.style.background = meta.color;

        const text = document.createElement("span");
        text.textContent = meta.label;

        item.appendChild(cb);
        item.appendChild(swatch);
        item.appendChild(text);
        root.appendChild(item);
      }
    }

    function setControlDefaults() {
      const topk = document.getElementById("topk");
      const windowNs = document.getElementById("window-ns");
      const stepNs = document.getElementById("step-ns");
      const fps = document.getElementById("fps");

      topk.max = String(Math.max(1, REPORT.summary.pair_count));
      topk.value = String(Math.min(REPORT.default_top_k, REPORT.summary.pair_count || 1));
      windowNs.value = String(Math.max(1, REPORT.default_window_ns));
      stepNs.value = String(Math.max(1, REPORT.default_step_ns));
      fps.value = String(state.fps);

      document.getElementById("topk-value").textContent = topk.value;
      document.getElementById("fps-value").textContent = fps.value;
    }

    function stopPlayback() {
      if (state.timer !== null) {
        clearInterval(state.timer);
        state.timer = null;
      }
      state.playing = false;
      document.getElementById("btn-play").textContent = "Play";
    }

    function startPlayback() {
      stopPlayback();
      state.playing = true;
      document.getElementById("btn-play").textContent = "Pause";
      const tickMs = Math.max(10, Math.round(1000 / Math.max(1, state.fps)));
      state.timer = setInterval(() => {
        if (state.frameStarts.length <= 1) return;
        state.frameIndex = (state.frameIndex + 1) % state.frameStarts.length;
        render();
      }, tickMs);
    }

    function togglePlayback() {
      if (state.playing) stopPlayback();
      else startPlayback();
    }

    function rebuildFrames(preserveIndex = true) {
      const oldStart = state.frameStarts[state.frameIndex] || 0;
      const maxT = REPORT.summary.time_span_ns;
      const windowNs = Math.max(1, Number(document.getElementById("window-ns").value || 1));
      const stepNs = Math.max(1, Number(document.getElementById("step-ns").value || windowNs));

      state.windowNs = windowNs;
      state.stepNs = stepNs;

      const starts = [];
      for (let t = 0; t <= maxT; t += stepNs) {
        starts.push(t);
      }
      if (starts.length === 0) starts.push(0);
      if (starts[starts.length - 1] < maxT) starts.push(maxT);
      state.frameStarts = starts;

      if (preserveIndex) {
        let bestIdx = 0;
        let bestDist = Math.abs((starts[0] || 0) - oldStart);
        for (let i = 1; i < starts.length; i += 1) {
          const d = Math.abs(starts[i] - oldStart);
          if (d < bestDist) {
            bestDist = d;
            bestIdx = i;
          }
        }
        state.frameIndex = bestIdx;
      } else {
        state.frameIndex = 0;
      }

      const frameSlider = document.getElementById("frame");
      frameSlider.max = String(Math.max(0, starts.length - 1));
      frameSlider.value = String(state.frameIndex);

      recomputeTimelineMetric();
      render();
    }

    function recomputeTimelineMetric() {
      const vals = [];
      for (let i = 0; i < state.frameStarts.length; i += 1) {
        const w = frameWindowStats(i);
        let sum = 0;
        for (let j = w.i0; j < w.i1; j += 1) {
          const f = flows[j];
          if (!state.enabledClasses.has(f[4])) {
            continue;
          }
          sum += 1;
        }
        vals.push(sum);
      }
      state.timelineMetricVals = vals;
    }

    function drawTimeline() {
      const canvas = document.getElementById("timeline");
      const ctx = canvas.getContext("2d");
      const dpr = window.devicePixelRatio || 1;
      const cssW = Math.max(260, Math.floor(canvas.clientWidth));
      const cssH = Math.max(72, Math.floor(canvas.clientHeight));
      canvas.width = Math.floor(cssW * dpr);
      canvas.height = Math.floor(cssH * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

      ctx.clearRect(0, 0, cssW, cssH);
      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, cssW, cssH);

      const vals = state.timelineMetricVals;
      const n = vals.length;
      if (n <= 0) return;

      const pad = { left: 8, right: 8, top: 6, bottom: 12 };
      const plotW = cssW - pad.left - pad.right;
      const plotH = cssH - pad.top - pad.bottom;
      const maxV = Math.max(1, ...vals);

      for (let i = 0; i < n; i += 1) {
        const v = vals[i];
        const t = Math.log10(v + 1) / Math.log10(maxV + 1);
        const h = Math.max(1, Math.round(plotH * t));
        const x0 = pad.left + Math.floor((i / n) * plotW);
        const x1 = pad.left + Math.floor(((i + 1) / n) * plotW);
        const w = Math.max(1, x1 - x0);
        ctx.fillStyle = i === state.frameIndex ? "#df5a3f" : "#7e93bc";
        ctx.globalAlpha = i === state.frameIndex ? 0.92 : 0.55;
        ctx.fillRect(x0, pad.top + plotH - h, w, h);
      }
      ctx.globalAlpha = 1;
      ctx.strokeStyle = "#9fb0cf";
      ctx.strokeRect(pad.left, pad.top, plotW, plotH);
    }

    function drawTopology(frameData) {
      const svg = document.getElementById("topology");
      while (svg.firstChild) svg.removeChild(svg.firstChild);

      const ns = "http://www.w3.org/2000/svg";
      const layout = REPORT.layout;
      svg.setAttribute("viewBox", `0 0 ${layout.width} ${layout.height}`);
      svg.setAttribute("height", `${layout.height}`);

      for (let i = 0; i < layout.dp_rects.length; i += 1) {
        const r = layout.dp_rects[i];
        const rect = document.createElementNS(ns, "rect");
        rect.setAttribute("x", r.x);
        rect.setAttribute("y", r.y);
        rect.setAttribute("width", r.w);
        rect.setAttribute("height", r.h);
        rect.setAttribute("fill", i % 2 === 0 ? "#f7fbff" : "#f7fff8");
        rect.setAttribute("stroke", "#d8e4f7");
        rect.setAttribute("rx", "10");
        svg.appendChild(rect);

        const text = document.createElementNS(ns, "text");
        text.setAttribute("x", r.x + 8);
        text.setAttribute("y", r.y + 16);
        text.setAttribute("font-size", "12");
        text.setAttribute("fill", "#4e5f7f");
        text.textContent = r.label;
        svg.appendChild(text);
      }

      for (const r of layout.pp_rects) {
        const rect = document.createElementNS(ns, "rect");
        rect.setAttribute("x", r.x);
        rect.setAttribute("y", r.y);
        rect.setAttribute("width", r.w);
        rect.setAttribute("height", r.h);
        rect.setAttribute("fill", "none");
        rect.setAttribute("stroke", "#8ca3c8");
        rect.setAttribute("stroke-dasharray", "4 4");
        rect.setAttribute("rx", "8");
        svg.appendChild(rect);
      }

      const nodeMap = new Map(REPORT.nodes.map((n) => [n.id, n]));
      const edges = frameData.edges.slice(0, Math.min(state.topK, frameData.edges.length));
      const maxEdge = Math.max(
        1,
        ...edges.map((e) => metricValue(e.bytes, e.count))
      );

      for (const e of edges) {
        if (e.src === e.dst) continue;
        const srcNode = nodeMap.get(e.src);
        const dstNode = nodeMap.get(e.dst);
        if (!srcNode || !dstNode) continue;

        const edgeMetric = metricValue(e.bytes, e.count);
        const t = Math.log10(edgeMetric + 1) / Math.log10(maxEdge + 1);
        const classId = dominantClass(e);
        const classMeta = CLASS_BY_ID.get(classId) || CLASS_BY_ID.get(CLASS_OTHER_ID);
        const strokeColor = classMeta ? classMeta.color : "#6b7280";

        const line = document.createElementNS(ns, "line");
        line.setAttribute("x1", srcNode.x);
        line.setAttribute("y1", srcNode.y);
        line.setAttribute("x2", dstNode.x);
        line.setAttribute("y2", dstNode.y);
        line.setAttribute("stroke-width", (0.45 + 4.1 * t).toFixed(2));
        line.setAttribute("stroke", strokeColor);
        line.setAttribute("opacity", (0.10 + 0.72 * t).toFixed(2));

        const classParts = [];
        for (const meta of CLASS_META) {
          const byCount = e.byClassCount[meta.id];
          if (byCount <= 0) continue;
          classParts.push(`${meta.key}: ${number(byCount)}`);
        }

        const title = document.createElementNS(ns, "title");
        title.textContent =
          `src=${e.src} dst=${e.dst} count=${number(e.count)} dominant=${classMeta ? classMeta.key : "other"} | ` +
          classParts.join(" ; ");
        line.appendChild(title);
        svg.appendChild(line);
      }

      const maxLoad = Math.max(1, ...frameData.nodeLoad);
      for (const n of REPORT.nodes) {
        const load = frameData.nodeLoad[n.id] || 0;
        const circle = document.createElementNS(ns, "circle");
        circle.setAttribute("cx", n.x);
        circle.setAttribute("cy", n.y);
        circle.setAttribute("r", layout.radius);
        circle.setAttribute("fill", colorByValue(load, maxLoad));
        circle.setAttribute("stroke", "#17366d");
        circle.setAttribute("stroke-width", "1");

        const title = document.createElementNS(ns, "title");
        title.textContent =
          `node=${n.id} dp=${n.dp_group} pp=${n.pp_group} current=${number(load)} globalSendCount=${number(n.global_send_count)} globalRecvCount=${number(n.global_recv_count)}`;
        circle.appendChild(title);
        svg.appendChild(circle);

        const text = document.createElementNS(ns, "text");
        text.setAttribute("x", n.x);
        text.setAttribute("y", n.y + 4);
        text.setAttribute("font-size", "10");
        text.setAttribute("text-anchor", "middle");
        text.setAttribute("fill", "#1a2f55");
        text.textContent = String(n.id);
        svg.appendChild(text);
      }
    }

    function updateStatus(frameData) {
      const frameSlider = document.getElementById("frame");
      frameSlider.value = String(state.frameIndex);
      document.getElementById("frame-value").textContent =
        `${state.frameIndex + 1}/${state.frameStarts.length}`;

      const absStart = absNsFromRel(frameData.start);
      const absEnd = absNsFromRel(frameData.end);
      const metricVal = `${number(frameData.windowCount)} flows`;

      const status = [
        `Frame ${state.frameIndex + 1}/${state.frameStarts.length}`,
        `window_rel=[${number(frameData.start)}, ${number(frameData.end)}) ns`,
        `window_abs=[${absStart}, ${absEnd}) ns`,
        `metric_value=${metricVal}`,
        `classes=[${activeClassLabelList()}]`
      ].join(" | ");
      document.getElementById("status").textContent = status;
    }

    function render() {
      const frameData = aggregateFrame(state.frameIndex);
      drawTopology(frameData);
      drawTimeline();
      updateStatus(frameData);
    }

    function bindEvents() {
      document.getElementById("topk").addEventListener("input", (ev) => {
        state.topK = Math.max(1, Number(ev.target.value || 1));
        document.getElementById("topk-value").textContent = String(state.topK);
        render();
      });

      document.getElementById("fps").addEventListener("input", (ev) => {
        state.fps = Math.max(1, Number(ev.target.value || 1));
        document.getElementById("fps-value").textContent = String(state.fps);
        if (state.playing) startPlayback();
      });

      document.getElementById("frame").addEventListener("input", (ev) => {
        state.frameIndex = Math.min(
          state.frameStarts.length - 1,
          Math.max(0, Number(ev.target.value || 0))
        );
        render();
      });

      document.getElementById("btn-play").addEventListener("click", togglePlayback);
      document.getElementById("btn-prev").addEventListener("click", () => {
        state.frameIndex = Math.max(0, state.frameIndex - 1);
        render();
      });
      document.getElementById("btn-next").addEventListener("click", () => {
        state.frameIndex = Math.min(state.frameStarts.length - 1, state.frameIndex + 1);
        render();
      });
      document.getElementById("btn-reset").addEventListener("click", () => {
        stopPlayback();
        state.frameIndex = 0;
        render();
      });
      document.getElementById("btn-rebuild").addEventListener("click", () => {
        rebuildFrames(true);
      });

      window.addEventListener("resize", () => {
        drawTimeline();
      });

      const timeline = document.getElementById("timeline");
      timeline.addEventListener("click", (ev) => {
        const rect = timeline.getBoundingClientRect();
        const x = ev.clientX - rect.left;
        const t = rect.width <= 1 ? 0 : x / rect.width;
        const idx = Math.min(
          state.frameStarts.length - 1,
          Math.max(0, Math.round(t * (state.frameStarts.length - 1)))
        );
        state.frameIndex = idx;
        render();
      });
    }

    function bootstrap() {
      setSummary();
      buildClassFilters();
      setControlDefaults();
      state.topK = Number(document.getElementById("topk").value);
      state.fps = Number(document.getElementById("fps").value);
      bindEvents();
      rebuildFrames(false);
    }

    bootstrap();
  </script>
</body>
</html>
"""
    return template.replace("__TITLE__", safe_title).replace("__DATA__", data_json)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a layout-aware HTML animation for communication flows."
    )
    parser.add_argument(
        "--log",
        type=Path,
        default=Path("simulation.log"),
        help="Input log file (default: simulation.log)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("traffic_report.html"),
        help="Output HTML path",
    )
    parser.add_argument("--nodes", type=int, default=64, help="Node count")
    parser.add_argument(
        "--row-size",
        type=int,
        default=16,
        help="Nodes per row; each row is one DP group",
    )
    parser.add_argument(
        "--pp-size",
        type=int,
        default=4,
        help="Nodes per PP group inside one row",
    )
    parser.add_argument(
        "--top-k",
        type=int,
        default=120,
        help="Default top links shown per frame",
    )
    parser.add_argument(
        "--window-ns",
        type=int,
        default=0,
        help="Default time-window size in ns (0 means auto)",
    )
    parser.add_argument(
        "--step-ns",
        type=int,
        default=0,
        help="Default frame-step size in ns (0 means same as window)",
    )
    parser.add_argument(
        "--min-size",
        type=int,
        default=0,
        help="Skip flows with size < min-size",
    )
    parser.add_argument(
        "--exclude-sizes",
        type=str,
        default="1023",
        help="Comma-separated packet sizes to exclude; size 1023 is always excluded",
    )
    parser.add_argument(
        "--title",
        type=str,
        default="Communication Traffic Animation",
        help="Report title",
    )
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.nodes <= 0:
        raise ValueError("--nodes must be > 0")
    if args.row_size <= 0:
        raise ValueError("--row-size must be > 0")
    if args.pp_size <= 0:
        raise ValueError("--pp-size must be > 0")
    if args.row_size % args.pp_size != 0:
        raise ValueError("--row-size must be divisible by --pp-size")
    if args.nodes % args.row_size != 0:
        raise ValueError("--nodes must be divisible by --row-size")
    if args.top_k == 0:
        raise ValueError("--top-k cannot be 0")
    if args.window_ns < 0:
        raise ValueError("--window-ns must be >= 0")
    if args.step_ns < 0:
        raise ValueError("--step-ns must be >= 0")
    if args.window_ns > 0 and args.step_ns > 0 and args.step_ns > args.window_ns * 1000:
        # Soft guard against accidental huge step causing near-empty timeline.
        raise ValueError("--step-ns is too large relative to --window-ns")


def main() -> int:
    args = parse_args()
    validate_args(args)

    excluded_sizes = parse_int_set(args.exclude_sizes)
    excluded_sizes.add(1023)  # hard requirement from workflow request

    flows, parse_stats = parse_flows(
        log_path=args.log,
        node_count=args.nodes,
        min_size=args.min_size,
        excluded_sizes=excluded_sizes,
    )
    if not flows:
        print("No flow entries found after filtering; nothing to render.")
        return 1

    report = build_report(
        flows=flows,
        node_count=args.nodes,
        row_size=args.row_size,
        pp_size=args.pp_size,
        top_k=args.top_k,
        window_ns=args.window_ns,
        step_ns=args.step_ns,
    )
    html_doc = render_html(title=args.title, report=report, parse_stats=parse_stats)
    args.output.write_text(html_doc, encoding="utf-8")
    print(f"Wrote report: {args.output}")
    print("Tip: open the HTML in browser and click Play; class colors are filterable.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
