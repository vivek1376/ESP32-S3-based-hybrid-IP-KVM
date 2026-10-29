#!/usr/bin/env python3
"""
Flask backend for ESP32-S3 IP-KVM Radial Overlay Telemetry Visualization.

Incrementally reads fixed 64-byte binary records from overlay_metrics.bin,
remembering the last read byte offset and automatically detecting client
session restarts.
"""

from __future__ import annotations

import argparse
import os
import struct
import threading
from pathlib import Path
from typing import Any

from flask import Flask, jsonify, request, send_from_directory

MAGIC = b"KVMOVL01"
HEADER_STRUCT = struct.Struct("<8sHHI")       # 16 bytes: magic, version, record_size, session_id
RECORD_STRUCT = struct.Struct("<IfffBBhhH10f")  # 64 bytes per frame

DEFAULT_METRICS_PATH = Path(__file__).resolve().parent.parent / "overlay_metrics.bin"
MAX_INITIAL_RECORDS = 36000  # Cap initial catch-up read to ~10 minutes @ 60 FPS


class BinaryMetricsReader:
    """Thread-safe incremental binary reader remembering last read-upto offset."""

    def __init__(self, file_path: Path) -> None:
        self.file_path = file_path
        self._lock = threading.Lock()
        self.session_id: int | None = None
        self.last_offset: int = HEADER_STRUCT.size

    def poll(self, client_offset: int | None = None,
             client_session: int | None = None,
             init_request: bool = False) -> dict[str, Any]:
        with self._lock:
            if not self.file_path.exists():
                return {
                    "status": "waiting",
                    "file": str(self.file_path),
                    "reset": False,
                    "session_id": None,
                    "next_offset": HEADER_STRUCT.size,
                    "count": 0,
                    "data": {},
                }

            try:
                file_size = self.file_path.stat().st_size
                if file_size < HEADER_STRUCT.size:
                    return {
                        "status": "waiting",
                        "file": str(self.file_path),
                        "reset": False,
                        "session_id": None,
                        "next_offset": HEADER_STRUCT.size,
                        "count": 0,
                        "data": {},
                    }

                with self.file_path.open("rb") as fp:
                    hdr_bytes = fp.read(HEADER_STRUCT.size)
                    if len(hdr_bytes) < HEADER_STRUCT.size:
                        return {
                            "status": "waiting",
                            "file": str(self.file_path),
                            "reset": False,
                            "session_id": None,
                            "next_offset": HEADER_STRUCT.size,
                            "count": 0,
                            "data": {},
                        }

                    magic, version, rec_size, hdr_session = HEADER_STRUCT.unpack(hdr_bytes)
                    if magic != MAGIC or rec_size != RECORD_STRUCT.size:
                        return {
                            "status": "error",
                            "message": f"Invalid binary header in {self.file_path}",
                            "reset": False,
                            "count": 0,
                            "data": {},
                        }

                    # Determine starting offset & whether kvm_client restarted
                    reset = False
                    if self.session_id != hdr_session or file_size < self.last_offset:
                        self.session_id = hdr_session
                        self.last_offset = HEADER_STRUCT.size
                        reset = True

                    if init_request:
                        read_from = HEADER_STRUCT.size
                        reset = True
                    elif client_offset is not None and client_session is not None:
                        if client_session != hdr_session or file_size < client_offset:
                            read_from = HEADER_STRUCT.size
                            reset = True
                        else:
                            read_from = max(HEADER_STRUCT.size, client_offset)
                    else:
                        read_from = self.last_offset

                    avail_bytes = max(0, file_size - read_from)
                    total_avail_records = avail_bytes // rec_size

                    # On initial page load of a very large file, tail the latest window
                    if init_request and total_avail_records > MAX_INITIAL_RECORDS:
                        skip_records = total_avail_records - MAX_INITIAL_RECORDS
                        read_from += skip_records * rec_size
                        total_avail_records = MAX_INITIAL_RECORDS

                    bytes_to_read = total_avail_records * rec_size
                    new_offset = read_from + bytes_to_read
                    self.last_offset = new_offset

                    if bytes_to_read == 0:
                        return {
                            "status": "ok",
                            "file": str(self.file_path),
                            "version": version,
                            "reset": reset,
                            "session_id": hdr_session,
                            "next_offset": new_offset,
                            "file_size": file_size,
                            "count": 0,
                            "data": {},
                        }

                    fp.seek(read_from)
                    payload = fp.read(bytes_to_read)

            except OSError as exc:
                return {
                    "status": "error",
                    "message": str(exc),
                    "reset": False,
                    "count": 0,
                    "data": {},
                }

            # Unpack fixed 64-byte records into compact columnar arrays
            n_actual = len(payload) // RECORD_STRUCT.size
            frame_idx: list[int] = []
            t: list[float] = []
            raw_dt_ms: list[float] = []
            clamped_dt_ms: list[float] = []
            phase: list[int] = []
            flags: list[int] = []
            raw_x: list[int] = []
            raw_y: list[int] = []
            d: list[float] = []
            v_theta: list[float] = []
            v_r: list[float] = []
            omega_ptr: list[float] = []
            psi: list[float] = []
            ccw_drive: list[float] = []
            spring_open: list[float] = []
            u: list[float] = []
            omega_vis: list[float] = []
            r_curr: list[float] = []

            for rec in RECORD_STRUCT.iter_unpack(payload[: n_actual * RECORD_STRUCT.size]):
                frame_idx.append(rec[0])
                t.append(round(rec[1], 4))
                raw_dt_ms.append(round(rec[2], 3))
                clamped_dt_ms.append(round(rec[3], 3))
                phase.append(rec[4])
                flags.append(rec[5])
                raw_x.append(rec[6])
                raw_y.append(rec[7])
                # rec[8] is reserved uint16
                d.append(round(rec[9], 2))
                v_theta.append(round(rec[10], 2))
                v_r.append(round(rec[11], 2))
                omega_ptr.append(round(rec[12], 4))
                psi.append(round(rec[13], 4))
                ccw_drive.append(round(rec[14], 4))
                spring_open.append(round(rec[15], 4))
                u.append(round(rec[16], 4))
                omega_vis.append(round(rec[17], 4))
                r_curr.append(round(rec[18], 2))

            return {
                "status": "ok",
                "file": str(self.file_path),
                "version": version,
                "reset": reset,
                "session_id": hdr_session,
                "next_offset": read_from + n_actual * RECORD_STRUCT.size,
                "file_size": file_size,
                "count": n_actual,
                "data": {
                    "frame_idx": frame_idx,
                    "t": t,
                    "raw_dt_ms": raw_dt_ms,
                    "clamped_dt_ms": clamped_dt_ms,
                    "phase": phase,
                    "flags": flags,
                    "raw_x": raw_x,
                    "raw_y": raw_y,
                    "d": d,
                    "v_theta": v_theta,
                    "v_r": v_r,
                    "omega_ptr": omega_ptr,
                    "psi": psi,
                    "ccw_drive": ccw_drive,
                    "spring_open": spring_open,
                    "u": u,
                    "omega_vis": omega_vis,
                    "r_curr": r_curr,
                },
            }


def create_app(metrics_path: Path) -> Flask:
    static_dir = Path(__file__).resolve().parent / "static"
    app = Flask(__name__, static_folder=str(static_dir), static_url_path="/static")
    reader = BinaryMetricsReader(metrics_path)

    @app.route("/")
    def index() -> Any:
        return send_from_directory(app.static_folder, "index.html")

    @app.route("/api/metrics")
    def api_metrics() -> Any:
        init_req = request.args.get("init", "0") == "1"
        offset_arg = request.args.get("offset", type=int)
        session_arg = request.args.get("session_id", type=int)
        result = reader.poll(
            client_offset=offset_arg,
            client_session=session_arg,
            init_request=init_req,
        )
        return jsonify(result)

    return app


def main() -> None:
    parser = argparse.ArgumentParser(description="KVM Radial Overlay Telemetry Webapp")
    parser.add_argument(
        "-m", "--metrics",
        type=Path,
        default=Path(os.environ.get("METRICS_FILE", str(DEFAULT_METRICS_PATH))),
        help=f"Path to binary metrics file (default: {DEFAULT_METRICS_PATH})",
    )
    parser.add_argument(
        "-p", "--port",
        type=int,
        default=5000,
        help="HTTP server port (default: 5000)",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="HTTP bind address (default: 127.0.0.1)",
    )
    args = parser.parse_args()

    metrics_path = args.metrics.resolve()
    print(f"[INFO] Reading binary telemetry from: {metrics_path}")
    print(f"[INFO] Open http://{args.host}:{args.port} in your browser")

    app = create_app(metrics_path)
    app.run(host=args.host, port=args.port, debug=False, threaded=True)


if __name__ == "__main__":
    main()

