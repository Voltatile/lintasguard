"""LintasGuard dashboard (README Step 10): FastAPI + WebSocket, read-only view of the crossing.

    python host/dashboard/app.py --vm 192.168.56.102          then open http://localhost:8000

It connects to lg-bridge on the QNX target (NDJSON over TCP, port 5555) through relay.Relay and
pushes every line to the browsers over the WebSocket /ws. Browsers cannot send anything to the
crossing: whatever arrives on /ws is read and discarded. Prototype, simulation only.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import os
from pathlib import Path

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from relay import Relay, origin_allowed

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
STATIC = HERE / "static"

relay = Relay(os.environ.get("LG_VM", "192.168.56.102"), int(os.environ.get("LG_BRIDGE_PORT", "5555")))


@contextlib.asynccontextmanager
async def lifespan(_app: FastAPI):
    task = asyncio.create_task(relay.run())
    try:
        yield
    finally:
        relay.stop()
        with contextlib.suppress(asyncio.TimeoutError, asyncio.CancelledError):
            await asyncio.wait_for(task, timeout=3)


app = FastAPI(title="LintasGuard dashboard", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=STATIC), name="static")


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC / "index.html")


@app.get("/config.json")
async def config() -> JSONResponse:
    """Crossing geometry for the drawing, from config/crossing.json (metres)."""
    try:
        cfg = json.loads((REPO / "config" / "crossing.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        cfg = {"strike_in_m": 400, "island_m": 12, "sensor_zone_m": 2}
    return JSONResponse({k: cfg.get(k) for k in ("strike_in_m", "island_m", "sensor_zone_m")})


@app.get("/link")
async def link() -> JSONResponse:
    return JSONResponse(relay.link())


@app.websocket("/ws")
async def ws(socket: WebSocket) -> None:
    # Only the dashboard page may read the stream, not any other page open in the browser (review L4).
    if not origin_allowed(socket.headers.get("origin"), socket.headers.get("host")):
        await socket.close(code=1008)
        return
    await socket.accept()
    queue = relay.subscribe()

    async def discard_incoming() -> None:
        # Read-only dashboard: anything a browser sends is ignored, never forwarded to the bridge.
        while True:
            await socket.receive_text()

    reader = asyncio.create_task(discard_incoming())
    try:
        await socket.send_text(json.dumps(relay.hello()))
        while True:
            try:
                line = await asyncio.wait_for(queue.get(), timeout=1.0)
            except asyncio.TimeoutError:
                line = json.dumps(relay.link())  # a heartbeat with the link status
            await socket.send_text(line)
    except (WebSocketDisconnect, RuntimeError):
        pass
    finally:
        reader.cancel()
        relay.unsubscribe(queue)


def main() -> None:
    ap = argparse.ArgumentParser(description="LintasGuard dashboard")
    ap.add_argument("--vm", default=relay.host, help="IP address of the QNX VM running lg-bridge")
    ap.add_argument("--bridge-port", type=int, default=relay.port)
    ap.add_argument("--http", type=int, default=8000, help="local HTTP port")
    a = ap.parse_args()
    relay.host = a.vm
    relay.port = a.bridge_port
    uvicorn.run(app, host="127.0.0.1", port=a.http, log_level="warning")


if __name__ == "__main__":
    main()
