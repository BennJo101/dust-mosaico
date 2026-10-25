"""The USB link to the bridge ESP32 (bridge/main/link_frame.h): COBS frames with a CRC-16."""
import logging
import struct
import threading
import time

LF_RX, LF_TX, LF_STATUS, LF_CHANNEL = 1, 2, 3, 4
BROADCAST = b"\xff" * 6
STATUS = struct.Struct("<BB6sIIII")   # lf_status_t

log = logging.getLogger("dust.link")


def crc16(data):
    """CRC-16/CCITT-FALSE."""
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def cobs_encode(raw):
    out, code_at, code = bytearray([0]), 0, 1
    for b in raw:
        if b:
            out.append(b)
            code += 1
        if not b or code == 0xFF:
            out[code_at] = code
            code_at, code = len(out), 1
            out.append(0)
    out[code_at] = code
    return bytes(out)


def cobs_decode(enc):
    out, i = bytearray(), 0
    while i < len(enc):
        code = enc[i]
        i += 1
        if not code or i + code - 1 > len(enc):
            return None
        out += enc[i:i + code - 1]
        i += code - 1
        if code != 0xFF and i < len(enc):
            out.append(0)
    return bytes(out)


def encode_frame(kind, mac, rssi, data):
    raw = bytes([kind]) + bytes(mac) + struct.pack("<b", rssi) + bytes(data)
    raw += struct.pack("<H", crc16(raw))
    return cobs_encode(raw) + b"\0"


def decode_frame(enc):
    """One frame's bytes (without the closing 0) → (kind, mac, rssi, data), or None if damaged."""
    raw = cobs_decode(enc)
    if raw is None or len(raw) < 10 or crc16(raw[:-2]) != struct.unpack_from("<H", raw, len(raw) - 2)[0]:
        return None
    return raw[0], raw[1:7], struct.unpack_from("<b", raw, 7)[0], raw[8:-2]


class Bridge:
    """Talks to the bridge over a serial port. on_packet(mac, rssi, data) runs on the reader thread."""

    def __init__(self, port, baud=921600, on_packet=None):
        import serial   # pyserial
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.on_packet = on_packet
        self.status = None          # last lf_status_t: version, channel, mac, heard, sent, failed, dropped
        self.status_at = 0.0
        self.bad_frames = 0
        self._wlock = threading.Lock()
        self._stop = False
        self._thread = threading.Thread(target=self._read, name="bridge-rx", daemon=True)
        self._thread.start()

    @property
    def mac(self):
        return self.status[2] if self.status else None

    @property
    def alive(self):
        return time.monotonic() - self.status_at < 3

    def send(self, data, mac=BROADCAST):
        frame = encode_frame(LF_TX, mac, 0, data)
        with self._wlock:
            self.ser.write(frame)

    def set_channel(self, ch):
        with self._wlock:
            self.ser.write(encode_frame(LF_CHANNEL, BROADCAST, 0, bytes([ch])))

    def close(self):
        self._stop = True
        self._thread.join(1)
        self.ser.close()

    def _read(self):
        buf = bytearray()
        while not self._stop:
            try:
                chunk = self.ser.read(4096)
            except Exception as e:   # unplugged: keep trying, the service reports it
                log.warning("bridge read failed: %s", e)
                time.sleep(1)
                continue
            for b in chunk:
                if b:
                    if len(buf) < 600:
                        buf.append(b)
                    continue
                if not buf:
                    continue
                f = decode_frame(bytes(buf))
                buf.clear()
                if f is None:
                    self.bad_frames += 1
                    continue
                kind, mac, rssi, data = f
                if kind == LF_RX and self.on_packet:
                    try:
                        self.on_packet(bytes(mac), rssi, data)
                    except Exception:
                        log.exception("handling a packet")
                elif kind == LF_STATUS and len(data) >= STATUS.size:
                    self.status = STATUS.unpack_from(data)
                    self.status_at = time.monotonic()
