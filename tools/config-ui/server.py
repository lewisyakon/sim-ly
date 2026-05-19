#!/usr/bin/env python3
"""Small local web UI for editing simconfig/config.json."""

from __future__ import annotations

import argparse
import json
import os
import shutil
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = ROOT / "simconfig" / "config.json"
INDEX_HTML = Path(__file__).resolve().with_name("index.html")


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        value = json.load(f)
    if not isinstance(value, dict):
        raise ValueError("config root must be a JSON object")
    return value


def validate_config(config: dict[str, Any]) -> list[str]:
    errors: list[str] = []

    topology = config.get("topology", {})
    if not isinstance(topology, dict):
        errors.append("topology 必须是对象")
    else:
        raw_model = topology.get("model", "fattree")
        if isinstance(raw_model, int):
            model = raw_model
            model_name = {
                0: "fattree",
                1: "clos",
                2: "fullmesh",
                3: "torus",
                4: "test",
                5: "hpn",
                6: "external",
            }.get(raw_model)
        else:
            model_name = str(raw_model).lower()
            model = {
                "fattree": 0,
                "clos": 1,
                "fullmesh": 2,
                "torus": 3,
                "test": 4,
                "hpn": 5,
                "external": 6,
                "file": 6,
            }.get(model_name)
        if model is None:
            errors.append("topology.model 不是已知拓扑")
        if model_name in {"fattree", "clos", "fullmesh", "torus", "test", "hpn"}:
            try:
                if int(topology.get("k", 0)) <= 0:
                    errors.append("topology.k 必须大于 0")
            except (TypeError, ValueError):
                errors.append("topology.k 必须是整数")
        if model_name in {"external", "file"} and not str(topology.get("file", "")).strip():
            errors.append("external 拓扑必须填写 topology.file")
        try:
            subnet_k = int(topology.get("subnet_k", 1))
            if subnet_k <= 0:
                errors.append("topology.subnet_k 必须大于 0")
        except (TypeError, ValueError):
            errors.append("topology.subnet_k 必须是整数")

    network = config.get("network", {})
    if not isinstance(network, dict):
        errors.append("network 必须是对象")
    else:
        try:
            ecmp = int(network.get("ecmpmodel", 1))
            if ecmp < 0 or ecmp > 4:
                errors.append("network.ecmpmodel 必须在 0..4 之间")
        except (TypeError, ValueError):
            errors.append("network.ecmpmodel 必须是整数")
        try:
            if int(network.get("mtu", 1)) <= 0:
                errors.append("network.mtu 必须大于 0")
        except (TypeError, ValueError):
            errors.append("network.mtu 必须是整数")

    pfc = config.get("pfc", {})
    if isinstance(pfc, dict):
        try:
            low = float(pfc.get("low_watermark", 0.2))
            high = float(pfc.get("high_watermark", 0.8))
            if not (0 <= low < high <= 1):
                errors.append("PFC 水位必须满足 0 <= low < high <= 1")
        except (TypeError, ValueError):
            errors.append("PFC 水位必须是数字")

    roce = config.get("roce", {})
    if isinstance(roce, dict):
        for key in ("sq_depth", "rq_depth", "transport_window_packets", "reorder_window_packets"):
            try:
                if int(roce.get(key, 1)) <= 0:
                    errors.append(f"roce.{key} 必须大于 0")
            except (TypeError, ValueError):
                errors.append(f"roce.{key} 必须是整数")
        try:
            if int(roce.get("max_retransmissions", 0)) < 0:
                errors.append("roce.max_retransmissions 不能小于 0")
        except (TypeError, ValueError):
            errors.append("roce.max_retransmissions 必须是整数")

    return errors


def write_json_atomic(path: Path, config: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        shutil.copy2(path, path.with_suffix(path.suffix + ".bak"))

    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump(config, f, ensure_ascii=False, indent=2)
        f.write("\n")
    os.replace(tmp, path)


class ConfigUiHandler(BaseHTTPRequestHandler):
    server: "ConfigUiServer"

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path in {"/", "/index.html"}:
            self.send_file(INDEX_HTML, "text/html; charset=utf-8")
            return
        if path == "/api/config":
            try:
                config = read_json(self.server.config_path)
                self.send_json(
                    {
                        "ok": True,
                        "config": config,
                        "config_path": str(self.server.config_path.relative_to(ROOT)),
                    }
                )
            except Exception as exc:  # pragma: no cover - surfaced to UI
                self.send_json({"ok": False, "errors": [str(exc)]}, status=500)
            return
        self.send_error(404)

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path != "/api/config":
            self.send_error(404)
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            payload = json.loads(body.decode("utf-8"))
            config = payload.get("config") if isinstance(payload, dict) else None
            if not isinstance(config, dict):
                self.send_json({"ok": False, "errors": ["payload.config 必须是对象"]}, status=400)
                return

            errors = validate_config(config)
            if errors:
                self.send_json({"ok": False, "errors": errors}, status=400)
                return

            write_json_atomic(self.server.config_path, config)
            self.send_json(
                {
                    "ok": True,
                    "message": f"已保存到 {self.server.config_path.relative_to(ROOT)}",
                    "backup": str(self.server.config_path.with_suffix(self.server.config_path.suffix + ".bak").relative_to(ROOT)),
                }
            )
        except Exception as exc:  # pragma: no cover - surfaced to UI
            self.send_json({"ok": False, "errors": [str(exc)]}, status=500)

    def send_file(self, path: Path, content_type: str) -> None:
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_json(self, value: dict[str, Any], status: int = 200) -> None:
        data = json.dumps(value, ensure_ascii=False, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt: str, *args: Any) -> None:
        print(f"[config-ui] {self.address_string()} - {fmt % args}")


class ConfigUiServer(ThreadingHTTPServer):
    def __init__(self, server_address: tuple[str, int], handler_class: type[BaseHTTPRequestHandler], config_path: Path):
        super().__init__(server_address, handler_class)
        self.config_path = config_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Local web UI for simconfig/config.json")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG), help="config JSON path")
    parser.add_argument("--host", default="127.0.0.1", help="bind host")
    parser.add_argument("--port", type=int, default=8765, help="bind port")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = ROOT / config_path
    config_path = config_path.resolve()

    server = ConfigUiServer((args.host, args.port), ConfigUiHandler, config_path)
    print(f"[config-ui] serving {config_path}")
    print(f"[config-ui] open http://{args.host}:{args.port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
