"""Edge cases of host/dashboard/relay.py: event dedup (missing or wrong-typed seq/t, window eviction, no
broadcast of a duplicate), size limits, hello() isolation and several browsers (standard library only)."""

from __future__ import annotations

import asyncio
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "host" / "dashboard"))

from relay import HISTORY, MAX_LINE, QUEUE_MAX, Relay  # noqa: E402

WINDOW = 4 * HISTORY  # size of the dedup window in relay.py


def ev(kind: str = "level", seq: object = 1, t: object = 5.0, **extra: object) -> bytes:
    obj: dict[str, object] = {"kind": kind}
    if seq is not None:
        obj["seq"] = seq
    if t is not None:
        obj["t"] = t
    obj.update(extra)
    return json.dumps(obj).encode()


def drain(q: asyncio.Queue[str]) -> list[dict]:
    out = []
    while not q.empty():
        out.append(json.loads(q.get_nowait()))
    return out


class DedupTests(unittest.IsolatedAsyncioTestCase):
    def make(self) -> tuple[Relay, asyncio.Queue[str]]:
        relay = Relay("127.0.0.1", 9)
        return relay, relay.subscribe()

    async def test_duplicate_is_not_broadcast_and_returns_true(self) -> None:
        relay, q = self.make()
        self.assertTrue(relay.handle_line(ev()))
        self.assertTrue(relay.handle_line(ev()))  # duplicate: accepted (not a bad line) but swallowed
        self.assertEqual(len(relay.history), 1)
        self.assertEqual(len(drain(q)), 1)
        self.assertEqual(relay.bad_lines, 0)

    async def test_kind_is_part_of_the_key(self) -> None:
        relay, q = self.make()
        relay.handle_line(ev("level", 1, 5.0))
        relay.handle_line(ev("notify", 1, 5.0))
        self.assertEqual([h["kind"] for h in relay.history], ["level", "notify"])
        self.assertEqual(len(drain(q)), 2)

    async def test_different_seq_or_t_are_different_events(self) -> None:
        relay, _ = self.make()
        for seq, t in ((1, 5.0), (2, 5.0), (1, 6.0), (1, 5.001)):
            relay.handle_line(ev(seq=seq, t=t))
        self.assertEqual(len(relay.history), 4)

    async def test_int_and_float_t_are_the_same_time(self) -> None:
        relay, _ = self.make()
        relay.handle_line(b'{"kind":"level","seq":1,"t":5}')
        relay.handle_line(b'{"kind":"level","seq":1,"t":5.0}')
        self.assertEqual(len(relay.history), 1)

    async def test_missing_seq_or_t_is_never_deduplicated(self) -> None:
        relay, q = self.make()
        for _ in range(3):
            relay.handle_line(ev(seq=None))
            relay.handle_line(ev(t=None))
            relay.handle_line(ev(seq=None, t=None))
        self.assertEqual(len(relay.history), 9)
        self.assertEqual(len(drain(q)), 9)
        self.assertEqual(len(relay._seen), 0)

    async def test_wrong_types_are_never_deduplicated(self) -> None:
        relay, _ = self.make()
        lines = [
            ev(seq="1"),            # string seq
            ev(seq=1.0),            # float seq
            ev(seq=None, t="5.0"),  # string t
            ev(seq=[1]),            # list seq
            ev(seq=1, t=[5]),       # list t
            ev(seq=1, t={"a": 1}),  # object t
            ev(seq=1, t=None),      # null t is dropped by ev(), so build it by hand below
        ]
        for raw in lines:
            relay.handle_line(raw)
            relay.handle_line(raw)
        relay.handle_line(b'{"kind":"level","seq":null,"t":null}')
        relay.handle_line(b'{"kind":"level","seq":null,"t":null}')
        # None of these has an int seq plus a numeric t, so nothing was remembered or swallowed
        # (the sixth/seventh lines are the only ones with an int seq, and their t is not numeric).
        self.assertEqual(len(relay._seen), 0)
        self.assertEqual(len(relay.history), 2 * len(lines) + 2)

    async def test_huge_and_negative_numbers_are_ordinary_keys(self) -> None:
        relay, _ = self.make()
        for raw in (ev(seq=2**70, t=1e300), ev(seq=-1, t=-5.5), ev(seq=0, t=0)):
            relay.handle_line(raw)
            relay.handle_line(raw)
        self.assertEqual(len(relay.history), 3)

    async def test_nan_t_does_not_grow_the_window(self) -> None:
        relay, _ = self.make()
        for _ in range(3 * WINDOW):
            relay.handle_line(b'{"kind":"level","seq":1,"t":NaN}')
        self.assertLessEqual(len(relay._seen), WINDOW)
        self.assertLessEqual(len(relay._seen_order), WINDOW)
        self.assertEqual(len(relay._seen), len(relay._seen_order))

    async def test_window_holds_exactly_4_times_history(self) -> None:
        relay, _ = self.make()
        for i in range(WINDOW):
            relay.handle_line(ev(seq=i))
        self.assertEqual(len(relay._seen), WINDOW)
        # Every one of the last WINDOW events is still recognised as a replay ...
        before = len(relay.history)
        for i in range(WINDOW):
            relay.handle_line(ev(seq=i))
        self.assertEqual(len(relay.history), before)
        self.assertEqual(len(relay._seen), WINDOW)

    async def test_oldest_key_is_evicted_one_at_a_time(self) -> None:
        relay, q = self.make()
        for i in range(WINDOW + 1):  # seq 0 is pushed out by seq WINDOW
            relay.handle_line(ev(seq=i))
        self.assertEqual(len(relay._seen), WINDOW)
        self.assertNotIn(("level", 0, 5.0), relay._seen)
        self.assertIn(("level", 1, 5.0), relay._seen)
        drain(q)
        relay.handle_line(ev(seq=1))  # still known: swallowed
        self.assertEqual(drain(q), [])
        relay.handle_line(ev(seq=0))  # evicted: seen as new again
        self.assertEqual(len(drain(q)), 1)
        self.assertEqual(relay.history[-1]["seq"], 0)
        # seq 0 came back and took seq 1's place at the front of the window, so 1 is gone now.
        self.assertEqual(len(relay._seen), WINDOW)
        self.assertNotIn(("level", 1, 5.0), relay._seen)

    async def test_set_and_order_never_disagree(self) -> None:
        relay, _ = self.make()
        for i in range(5 * WINDOW):
            relay.handle_line(ev(seq=i % 313, t=float(i % 7)))
            self.assertEqual(set(relay._seen_order), relay._seen)
            self.assertLessEqual(len(relay._seen), WINDOW)

    async def test_replay_burst_after_reconnect_keeps_order_and_adds_nothing(self) -> None:
        relay, q = self.make()
        burst = [ev(seq=i, t=100.0 + i) for i in range(32)]  # what lg-bridge replays: up to 32 lines
        for raw in burst:
            relay.handle_line(raw)
        drain(q)
        first = [h["seq"] for h in relay.history]
        for raw in burst:
            relay.handle_line(raw)
        self.assertEqual([h["seq"] for h in relay.history], first)
        self.assertEqual(drain(q), [])

    async def test_history_is_trimmed_but_dedup_window_is_longer(self) -> None:
        relay, _ = self.make()
        for i in range(HISTORY + 20):
            relay.handle_line(ev(seq=i))
        self.assertEqual(len(relay.history), HISTORY)
        self.assertEqual(relay.history[0]["seq"], 20)
        # An event already trimmed out of the history is still not re-added by a replay.
        relay.handle_line(ev(seq=0))
        self.assertEqual(len(relay.history), HISTORY)
        self.assertEqual(relay.history[0]["seq"], 20)

    async def test_state_and_metric_lines_are_never_deduplicated(self) -> None:
        relay, q = self.make()
        for _ in range(3):
            relay.handle_line(b'{"kind":"state","seq":1,"t":5.0,"level":0}')
            relay.handle_line(b'{"kind":"metric","seq":1,"t":5.0,"name":"n","value":1}')
        self.assertEqual(len(drain(q)), 6)
        self.assertEqual(len(relay._seen), 0)
        self.assertEqual(len(relay.history), 0)


class LineHandlingTests(unittest.IsolatedAsyncioTestCase):
    async def test_line_size_limit_is_exact(self) -> None:
        relay = Relay("127.0.0.1", 9)
        head = b'{"kind":"state","x":"'
        tail = b'"}'
        ok = head + b"a" * (MAX_LINE - len(head) - len(tail)) + tail
        self.assertEqual(len(ok), MAX_LINE)
        self.assertTrue(relay.handle_line(ok))
        self.assertFalse(relay.handle_line(ok + b" "))
        self.assertEqual((relay.lines_in, relay.bad_lines), (1, 1))

    async def test_invalid_utf8_empty_and_odd_json_are_bad_lines(self) -> None:
        relay = Relay("127.0.0.1", 9)
        bad = [
            b"",
            b" ",
            b"{",
            b'{"kind":"state"',
            b'{"kind":"state"} x',
            b'{"kind":"st\xffate"}',
            b'{"kind":1}',
            b'{"kind":null}',
            b'{"kind":["state"]}',
            b"[]",
            b"null",
            b"42",
            b'"state"',
            b"\xef\xbb\xbf{}",  # BOM
        ]
        for raw in bad:
            self.assertFalse(relay.handle_line(raw), raw)
        self.assertEqual(relay.bad_lines, len(bad))
        self.assertEqual(relay.lines_in, 0)
        self.assertIsNone(relay.state)
        self.assertEqual(relay.link()["age_s"], None)  # a bad line is not a sign of life

    async def test_unknown_kind_is_forwarded_but_changes_nothing(self) -> None:
        relay = Relay("127.0.0.1", 9)
        q = relay.subscribe()
        self.assertTrue(relay.handle_line(b'{"kind":"ack","what":"subscribe"}'))
        self.assertEqual([m["kind"] for m in drain(q)], ["ack"])
        self.assertIsNone(relay.state)
        self.assertEqual(len(relay.history), 0)
        self.assertEqual(relay.metrics, {})

    async def test_metric_without_string_name_is_ignored(self) -> None:
        relay = Relay("127.0.0.1", 9)
        relay.handle_line(b'{"kind":"metric","name":5,"value":1}')
        relay.handle_line(b'{"kind":"metric","value":1}')
        relay.handle_line(b'{"kind":"metric","name":"a","value":1}')
        relay.handle_line(b'{"kind":"metric","name":"a"}')
        self.assertEqual(relay.metrics, {"a": None})

    async def test_state_is_replaced_not_merged(self) -> None:
        relay = Relay("127.0.0.1", 9)
        relay.handle_line(b'{"kind":"state","level":2,"gate":"DOWN"}')
        relay.handle_line(b'{"kind":"state","level":0}')
        self.assertEqual(relay.state, {"kind": "state", "level": 0})

    async def test_nan_and_infinity_are_not_relayed_to_browsers(self) -> None:
        # json.loads accepts NaN / Infinity, but a browser's JSON.parse does not: one such line would make the
        # dashboard script throw. The relay should treat it as a bad line (lg-bridge never writes it).
        relay = Relay("127.0.0.1", 9)
        q = relay.subscribe()
        relay.handle_line(b'{"kind":"state","level":NaN}')
        relay.handle_line(b'{"kind":"metric","name":"n","value":Infinity}')
        for text in (q.get_nowait() for _ in range(q.qsize())):
            self.assertNotIn("NaN", text)
            self.assertNotIn("Infinity", text)


class BrowserTests(unittest.IsolatedAsyncioTestCase):
    async def test_hello_is_a_snapshot_not_a_live_view(self) -> None:
        relay = Relay("127.0.0.1", 9)
        relay.handle_line(ev())
        relay.handle_line(b'{"kind":"metric","name":"a","value":1}')
        hello = relay.hello()
        hello["history"].clear()
        hello["metrics"]["a"] = 99
        hello["link"]["connected"] = True
        self.assertEqual(len(relay.history), 1)
        self.assertEqual(relay.metrics["a"], 1)
        self.assertFalse(relay.connected)

    async def test_hello_before_anything_arrived(self) -> None:
        hello = Relay("127.0.0.1", 9).hello()
        self.assertIsNone(hello["state"])
        self.assertEqual(hello["history"], [])
        self.assertEqual(hello["metrics"], {})
        self.assertFalse(hello["link"]["connected"])
        self.assertEqual(hello["link"]["target"], "127.0.0.1:9")

    async def test_a_full_queue_does_not_slow_the_others(self) -> None:
        relay = Relay("127.0.0.1", 9)
        slow, fast = relay.subscribe(), relay.subscribe()
        for i in range(QUEUE_MAX + 10):
            relay.handle_line(json.dumps({"kind": "metric", "name": "n", "value": i}).encode())
            if i % 2:
                fast.get_nowait()  # the fast browser keeps up
        self.assertEqual(slow.qsize(), QUEUE_MAX)
        self.assertLess(fast.qsize(), QUEUE_MAX)
        self.assertEqual(json.loads(slow.get_nowait())["value"], 10)

    async def test_unsubscribe_twice_and_unknown_queue_do_not_raise(self) -> None:
        relay = Relay("127.0.0.1", 9)
        q = relay.subscribe()
        relay.unsubscribe(q)
        relay.unsubscribe(q)
        relay.unsubscribe(asyncio.Queue())
        self.assertTrue(relay.handle_line(ev()))

    async def test_each_browser_gets_its_own_copy_of_every_line(self) -> None:
        relay = Relay("127.0.0.1", 9)
        a, b = relay.subscribe(), relay.subscribe()
        relay.handle_line(ev())
        self.assertEqual(len(drain(a)), 1)
        self.assertEqual(len(drain(b)), 1)

    async def test_lines_are_compact_single_line_json(self) -> None:
        relay = Relay("127.0.0.1", 9)
        q = relay.subscribe()
        relay.handle_line(b'{ "kind" : "state" ,\t"text" : "a b" }')
        text = q.get_nowait()
        self.assertEqual(text, '{"kind":"state","text":"a b"}')
        self.assertNotIn("\n", text)

    async def test_the_browser_side_never_reaches_the_bridge(self) -> None:
        # Relay has no method that writes to the bridge besides the fixed subscribe line.
        import relay as relay_mod

        self.assertEqual(relay_mod.SUBSCRIBE, b'{"kind":"subscribe"}\n')
        names = [n for n in dir(Relay) if not n.startswith("_")]
        self.assertEqual(sorted(names), ["handle_line", "hello", "link", "run", "stop", "subscribe", "unsubscribe"])


if __name__ == "__main__":
    unittest.main()
