"""Unit tests of host/dashboard/relay.py with a fake lg-bridge (standard library only)."""

from __future__ import annotations

import asyncio
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host" / "dashboard"))

from relay import HISTORY, QUEUE_MAX, Relay  # noqa: E402

STATE = b'{"t":1.000,"kind":"state","level":0,"gate":"UP","trains":[]}\n'


class FakeBridge:
    """Accepts connections, records what clients send, sends the given lines, then closes."""

    def __init__(self, lines: list[bytes], close_after: bool = True) -> None:
        self.lines = lines
        self.close_after = close_after
        self.received: list[bytes] = []
        self.connections = 0
        self.server: asyncio.base_events.Server | None = None
        self.port = 0

    async def start(self) -> None:
        self.server = await asyncio.start_server(self._client, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.connections += 1
        try:
            self.received.append(await asyncio.wait_for(reader.readline(), timeout=2))
        except asyncio.TimeoutError:
            pass
        for line in self.lines:
            writer.write(line)
        await writer.drain()
        if self.close_after:
            await asyncio.sleep(0.05)
            writer.close()
        else:
            await asyncio.sleep(10)

    async def stop(self) -> None:
        if self.server is not None:
            self.server.close()
            await self.server.wait_closed()


async def run_for(relay: Relay, seconds: float) -> None:
    task = asyncio.create_task(relay.run())
    await asyncio.sleep(seconds)
    relay.stop()
    await asyncio.wait_for(task, timeout=3)


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def test_lines_state_history_metrics(self) -> None:
        lines = [
            STATE,
            b'{"t":2.0,"kind":"level","from":0,"to":2,"reason":"SENSOR_FAILED"}\n',
            b'{"t":2.0,"kind":"notify","to":"maintenance","severity":"urgent","sensor":"west/a"}\n',
            b'{"t":3.0,"kind":"metric","name":"closure_ms_per_hour","value":123}\n',
            b"not json\n",
            b'["array"]\n',
            b'{"no_kind":1}\n',
        ]
        bridge = FakeBridge(lines)
        await bridge.start()
        relay = Relay("127.0.0.1", bridge.port, reconnect_s=5)
        q = relay.subscribe()
        await run_for(relay, 0.5)
        await bridge.stop()
        self.assertEqual(bridge.received[0], b'{"kind":"subscribe"}\n')  # the only line ever sent
        self.assertEqual(relay.state["level"], 0)
        self.assertEqual([h["kind"] for h in relay.history], ["level", "notify"])
        self.assertEqual(relay.metrics["closure_ms_per_hour"], 123)
        self.assertEqual(relay.bad_lines, 3)
        self.assertEqual(relay.lines_in, 4)
        got = []
        while not q.empty():
            got.append(json.loads(q.get_nowait()))
        kinds = [g["kind"] for g in got]
        self.assertIn("state", kinds)
        self.assertIn("notify", kinds)
        self.assertIn("link", kinds)  # connected / disconnected
        hello = relay.hello()
        self.assertEqual(hello["kind"], "hello")
        self.assertEqual(hello["state"]["gate"], "UP")
        self.assertEqual(len(hello["history"]), 2)

    async def test_reconnects_after_the_bridge_closes(self) -> None:
        bridge = FakeBridge([STATE])
        await bridge.start()
        relay = Relay("127.0.0.1", bridge.port, reconnect_s=0.1)
        await run_for(relay, 0.8)
        await bridge.stop()
        self.assertGreaterEqual(bridge.connections, 2)
        self.assertGreaterEqual(relay.connects, 2)
        self.assertFalse(relay.connected)

    async def test_no_bridge_is_reported_not_raised(self) -> None:
        relay = Relay("127.0.0.1", 9, reconnect_s=0.05)  # nothing listens on port 9
        await run_for(relay, 0.3)
        self.assertFalse(relay.connected)
        self.assertEqual(relay.lines_in, 0)
        self.assertFalse(relay.link()["connected"])

    async def test_slow_browser_loses_old_lines_only(self) -> None:
        relay = Relay("127.0.0.1", 9)
        q = relay.subscribe()
        for i in range(QUEUE_MAX + 50):
            relay.handle_line(json.dumps({"kind": "metric", "name": "n", "value": i}).encode())
        self.assertEqual(q.qsize(), QUEUE_MAX)
        self.assertEqual(json.loads(q.get_nowait())["value"], 50)  # the 50 oldest were dropped
        relay.unsubscribe(q)
        relay.handle_line(STATE.strip())
        self.assertEqual(q.qsize(), QUEUE_MAX - 1)  # an unsubscribed queue gets nothing

    async def test_history_is_bounded_and_long_lines_dropped(self) -> None:
        relay = Relay("127.0.0.1", 9)
        for i in range(HISTORY + 10):
            relay.handle_line(json.dumps({"kind": "level", "from": 0, "to": 2, "n": i}).encode())
        self.assertEqual(len(relay.history), HISTORY)
        self.assertEqual(relay.history[0]["n"], 10)
        self.assertFalse(relay.handle_line(b'{"kind":"state","x":"' + b"a" * 70000 + b'"}'))


if __name__ == "__main__":
    unittest.main()
