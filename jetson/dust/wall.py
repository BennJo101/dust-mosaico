"""The wall as the Jetson sees it: every tile's health (from their hellos), commands that are
repeated until the leader acknowledges them, and the beacon that tells the tiles the Jetson is
there and what it can do."""
import logging
import math
import random
import threading
import time
from dataclasses import dataclass

from . import proto
from .link import BROADCAST

log = logging.getLogger("dust.wall")

ALIVE_S = 0.6          # same as CONS_ALIVE_MS on the tiles
RESEND_S = 0.08        # same as REL_RESEND_MS
TRIES = 10
BEACON_S = 0.5


@dataclass
class Tile:
    hello: proto.Hello
    mac: bytes
    rssi: int
    seen: float


def uid_from_mac(mac):
    return (mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5]


class Wall:
    def __init__(self, bridge, caps=lambda: 0, busy=lambda: 0):
        self.bridge = bridge
        self.caps, self.busy = caps, busy
        self.tiles = {}                      # uid → Tile
        self.on_voice_packet = None          # (mac, rssi, data) for claims and audio
        self._lock = threading.Lock()
        self._seq = random.randrange(1, 65535)
        self._pending = {}                   # seq → [packet, tries, next send time]
        self.lost = 0
        self.other_firmware = {}             # mac → proto version heard
        self._stop = False
        threading.Thread(target=self._loop, name="wall", daemon=True).start()

    # ---------- what the wall looks like ----------
    @property
    def uid(self):
        mac = self.bridge.mac
        return uid_from_mac(mac) if mac else 0x0B21D6E5

    def alive(self):
        now = time.monotonic()
        with self._lock:
            return {u: t for u, t in self.tiles.items() if now - t.seen < ALIVE_S}

    def leader(self):
        """The slot that leads, by the tiles' own rule (lowest docked slot, skipping hot or flat ones)."""
        docked = sorted((t.hello.slot, t.hello.err) for t in self.alive().values() if t.hello.docked)
        for slot, err in docked:
            if not err & ((1 << 7) | (1 << 8)):
                return slot
        return docked[0][0] if docked else None

    def mac_of(self, uid):
        with self._lock:
            t = self.tiles.get(uid)
            return t.mac if t else None

    def slot_of(self, uid):
        with self._lock:
            t = self.tiles.get(uid)
            return t.hello.slot if t and t.hello.docked else None

    def health(self):
        """A short description of every tile, for logs and for Dust to talk about."""
        tiles = self.alive()
        if not tiles:
            return "No tiles can be heard."
        lines = []
        for uid, t in sorted(tiles.items(), key=lambda kv: (kv[1].hello.slot, kv[0])):
            h = t.hello
            where = f"screen {h.slot + 1}" if h.slot < 9 else "an unplaced tile"
            state = "lifted off the wall" if h.lifted else "on the wall" if h.docked else "in setup"
            parts = [f"{where} ({uid:08x}) is {state}, drawing {h.fps:.0f} fps"]
            if h.battery <= 100:
                parts.append(f"battery {h.battery}%")
            if h.temp_c != -128:
                parts.append(f"{h.temp_c} C")
            errs = h.errors()
            if errs:
                parts.append("problems: " + ", ".join(errs))
            lines.append(", ".join(parts))
        if self.other_firmware:
            lines.append(f"{len(self.other_firmware)} tile(s) run different firmware and can't join")
        return "\n".join(lines)

    # ---------- packets from the bridge ----------
    def on_packet(self, mac, rssi, data):
        h = proto.head(data)
        if not h:
            return
        version, typ, slot, flags = h
        if version != proto.PROTO:
            self.other_firmware[mac] = version
            return
        if typ == proto.PKT_HELLO:
            hello = proto.parse_hello(data)
            if hello:
                with self._lock:
                    old = self.tiles.get(hello.uid)
                    if old is None or old.hello.err != hello.err:
                        self._report(hello, old)
                    self.tiles[hello.uid] = Tile(hello, mac, rssi, time.monotonic())
        elif typ == proto.PKT_ACK:
            ack = proto.parse_ack(data)
            if ack and ack[0] == self.uid:
                with self._lock:
                    self._pending.pop(ack[1], None)
        if typ in (proto.PKT_CLAIM, proto.PKT_AUDIO, proto.PKT_HELLO) and self.on_voice_packet:
            self.on_voice_packet(mac, rssi, data)

    def _report(self, hello, old):
        name = f"screen {hello.slot + 1}" if hello.slot < 9 else f"tile {hello.uid:08x}"
        was = set(old.hello.errors()) if old else set()
        now = set(hello.errors())
        if old is None:
            log.info("%s joined%s", name, (" with problems: " + ", ".join(sorted(now))) if now else "")
        for e in sorted(now - was):
            if old is not None:
                log.warning("%s: %s", name, e)
        for e in sorted(was - now):
            log.info("%s: %s cleared", name, e)

    # ---------- commands ----------
    def _send_cmd(self, op, numbered=True, **kw):
        has_wall = self.leader() is not None   # (takes the lock itself)
        with self._lock:
            seq = self._seq
            self._seq = self._seq % 65535 + 1
            pkt = proto.make_cmd(self.uid, seq, op, numbered=numbered, **kw)
            # repeat until acknowledged, if there's a wall to acknowledge it
            if numbered and has_wall:
                self._pending[seq] = [pkt, 1, time.monotonic() + RESEND_S]
        self.bridge.send(pkt)

    def state(self, name):
        self._send_cmd(proto.CMD_STATE, a=proto.STATES[name])

    def form(self, shape, text=""):
        t = text.upper().encode("ascii", "ignore")[:proto.TEXT_MAX]
        self._send_cmd(proto.CMD_FORM, a=proto.FORMS[shape], text=t)

    def fx(self, kind, x=0.5, y=0.5, dx=0.0, dy=0.0, strength=0.8, radius=0.3, seconds=4.0):
        n = math.hypot(dx, dy)
        if n > 1e-5:
            dx, dy = dx / n, dy / n
        self._send_cmd(proto.CMD_FX, a=proto.FX[kind], x=x, y=y, dx=dx, dy=dy, strength=strength, radius=radius, seconds=seconds)

    def scatter(self):
        self._send_cmd(proto.CMD_SCATTER)

    def calm(self):
        self._send_cmd(proto.CMD_CALM)

    def send_to(self, uid, data):
        """Unicast to one tile (its replies), falling back to broadcast if its address isn't known."""
        self.bridge.send(data, self.mac_of(uid) or BROADCAST)

    # ---------- beacon and resends ----------
    def _loop(self):
        next_beacon = 0.0
        while not self._stop:
            now = time.monotonic()
            if now >= next_beacon:
                next_beacon = now + BEACON_S
                try:
                    self.bridge.send(proto.make_beacon(self.uid, self.caps(), min(255, self.busy())))
                except Exception as e:
                    log.warning("beacon failed: %s", e)
            with self._lock:
                due = [(s, p) for s, p in self._pending.items() if now >= p[2]]
                for s, p in due:
                    if p[1] >= TRIES:
                        del self._pending[s]
                        self.lost += 1
                        log.warning("a command was never acknowledged (is a leader on the wall?)")
                    else:
                        p[1] += 1
                        p[2] = now + RESEND_S
            for s, p in due:
                if s in self._pending:
                    self.bridge.send(p[0])
            time.sleep(0.02)

    def close(self):
        self._stop = True
