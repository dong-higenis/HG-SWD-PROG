#!/usr/bin/env python3
"""Bench tool for the H5 <-> ESP32S3_NET UART protocol (PROTOCOL.md v1).

Plays the H5 (master) role through a USB-UART adapter wired to the ESP
(adapter TX -> ESP GPIO48, adapter RX <- ESP GPIO45, GND).

  python net_link_test.py COM7                       # HELLO, STATUS, scan, HTTP if already online
  python net_link_test.py COM7 --ssid MyAP --pw secret
  python net_link_test.py COM7 --apn lte.sktelecom.com --no-scan
  python net_link_test.py --selftest                 # no hardware: CRC + framing checks

Requires pyserial for hardware runs (pip install pyserial).
"""

import argparse
import binascii
import random
import struct
import sys
import time
import zlib

SYNC1, SYNC2 = 0xA5, 0x5A
MAX_PAYLOAD = 1100
RESP_FLAG = 0x80
INTER_BYTE_TIMEOUT = 0.050

CMD_HELLO, CMD_STATUS, CMD_OFF = 0x01, 0x02, 0x03
CMD_WIFI_SCAN, CMD_WIFI_LIST, CMD_WIFI_JOIN = 0x10, 0x11, 0x12
CMD_LTE_JOIN = 0x20
CMD_HTTP_GET, CMD_HTTP_READ, CMD_HTTP_CLOSE = 0x30, 0x31, 0x32

ST_OK, ST_BUSY, ST_ERROR, ST_BAD_REQUEST, ST_NOT_CONNECTED, ST_HTTP, ST_TIMEOUT, ST_UNKNOWN_CMD = range(8)
STATUS_NAMES = ["OK", "BUSY", "ERROR", "BAD_REQUEST", "NOT_CONNECTED", "HTTP", "TIMEOUT", "UNKNOWN_CMD"]
MODE_NAMES = ["off", "wifi", "lte"]
STATE_NAMES = ["idle", "connecting", "connected", "failed"]

# H5-side response timeouts (PROTOCOL.md) and retry count.
TIMEOUTS = {CMD_HTTP_GET: 25.0, CMD_HTTP_READ: 8.0}
DEFAULT_TIMEOUT = 0.3
RETRIES = 3

DEFAULT_URL = "https://can-dubu.github.io/hg-swd-fw-hub/manifest.json"


# --------------------------------------------------------------------------- framing

def crc16_ccitt_false(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frame(cmd, seq, payload=b""):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    body = bytes([cmd, seq & 0xFF]) + struct.pack("<H", len(payload)) + bytes(payload)
    return bytes([SYNC1, SYNC2]) + body + struct.pack("<H", crc16_ccitt_false(body))


class FrameDecoder:
    """Byte-stream parser mirroring the firmware (link.cpp): sync, 50 ms inter-byte
    timeout, len <= 1100, CRC over cmd..payload; bad frames are dropped."""

    def __init__(self, inter_byte_timeout=INTER_BYTE_TIMEOUT):
        self.timeout = inter_byte_timeout
        self.crc_errors = self.timeouts = self.oversize = 0
        self._reset()
        self._last = None

    def _reset(self):
        self._state = "sync1"
        self._buf = bytearray()
        self._need = 0

    def feed(self, data, now=None):
        if now is None:
            now = time.monotonic()
        frames = []
        for b in data:
            if self._state != "sync1" and self._last is not None and now - self._last > self.timeout:
                self.timeouts += 1
                self._reset()
            self._last = now
            st = self._state
            if st == "sync1":
                if b == SYNC1:
                    self._state = "sync2"
            elif st == "sync2":
                if b == SYNC2:
                    self._state = "head"
                    self._buf = bytearray()
                elif b != SYNC1:
                    self._state = "sync1"
            elif st == "head":
                self._buf.append(b)
                if len(self._buf) == 4:
                    length = self._buf[2] | (self._buf[3] << 8)
                    if length > MAX_PAYLOAD:
                        self.oversize += 1
                        self._reset()
                    else:
                        self._need = length + 2
                        self._state = "rest"
            elif st == "rest":
                self._buf.append(b)
                self._need -= 1
                if self._need == 0:
                    body, crc = bytes(self._buf[:-2]), self._buf[-2] | (self._buf[-1] << 8)
                    self._reset()
                    if crc16_ccitt_false(body) != crc:
                        self.crc_errors += 1
                    else:
                        frames.append((body[0], body[1], body[4:]))
        return frames


# --------------------------------------------------------------------------- payloads

def parse_status(data):
    if len(data) < 7:
        raise ValueError("STATUS payload too short")
    mode, state, rssi = data[0], data[1], struct.unpack("b", data[2:3])[0]
    ip = ".".join(str(x) for x in data[3:7])
    detail = data[7:].decode("utf-8", "replace")
    return {"mode": mode, "state": state, "rssi": rssi, "ip": ip, "detail": detail}


def format_status(s):
    mode = MODE_NAMES[s["mode"]] if s["mode"] < len(MODE_NAMES) else s["mode"]
    state = STATE_NAMES[s["state"]] if s["state"] < len(STATE_NAMES) else s["state"]
    return "mode=%s state=%s rssi=%d ip=%s detail='%s'" % (mode, state, s["rssi"], s["ip"], s["detail"])


def parse_wifi_list(data):
    count, pos, aps = data[0], 1, []
    for _ in range(count):
        rssi = struct.unpack("b", data[pos:pos + 1])[0]
        auth, n = data[pos + 1], data[pos + 2]
        ssid = data[pos + 3:pos + 3 + n]
        if len(ssid) != n:
            raise ValueError("WIFI_LIST truncated")
        aps.append((rssi, auth, ssid.decode("utf-8", "replace")))
        pos += 3 + n
    if pos != len(data):
        raise ValueError("WIFI_LIST has %d trailing bytes" % (len(data) - pos))
    return aps


def wifi_join_payload(ssid, pw):
    s, p = ssid.encode("utf-8"), pw.encode("utf-8")
    return bytes([len(s)]) + s + bytes([len(p)]) + p


def lte_join_payload(apn):
    a = apn.encode("ascii")
    return bytes([len(a)]) + a


def http_get_payload(url):
    u = url.encode("ascii")
    return struct.pack("<H", len(u)) + u


# --------------------------------------------------------------------------- link

class LinkError(Exception):
    pass


class Link:
    def __init__(self, port, verbose=False):
        self.port = port
        self.verbose = verbose
        self.seq = random.randrange(256)
        self.decoder = FrameDecoder()
        self.retransmits = 0
        self.last_frame = None

    def _wait_response(self, cmd, seq, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            n = self.port.in_waiting
            data = self.port.read(n if n else 1)
            if not data:
                continue
            for rcmd, rseq, payload in self.decoder.feed(data):
                if rcmd == (cmd | RESP_FLAG) and rseq == seq:
                    return payload
                if self.verbose:
                    print("  (ignored frame cmd=%02X seq=%u)" % (rcmd, rseq))
        return None

    def send_raw(self, frame, cmd, seq, timeout):
        self.port.write(frame)
        return self._wait_response(cmd, seq, timeout)

    def request(self, cmd, payload=b"", timeout=None):
        """New seq per request; on timeout resend the same frame (same seq) up to RETRIES times."""
        if timeout is None:
            timeout = TIMEOUTS.get(cmd, DEFAULT_TIMEOUT)
        self.seq = (self.seq + 1) & 0xFF
        frame = encode_frame(cmd, self.seq, payload)
        self.last_frame = (frame, cmd, self.seq)
        for attempt in range(1 + RETRIES):
            if attempt:
                self.retransmits += 1
                if self.verbose:
                    print("  retransmit cmd=%02X seq=%u (%d)" % (cmd, self.seq, attempt))
            resp = self.send_raw(frame, cmd, self.seq, timeout)
            if resp is not None:
                if not resp:
                    raise LinkError("empty response payload")
                return resp[0], bytes(resp[1:])
        raise LinkError("no response to cmd 0x%02X after %d tries" % (cmd, 1 + RETRIES))

    def resend_last(self):
        frame, cmd, seq = self.last_frame
        resp = self.send_raw(frame, cmd, seq, TIMEOUTS.get(cmd, DEFAULT_TIMEOUT))
        if resp is None:
            raise LinkError("no response to duplicate")
        return resp[0], bytes(resp[1:])


def st_name(st):
    return STATUS_NAMES[st] if st < len(STATUS_NAMES) else "0x%02X" % st


def expect_ok(name, st, data):
    if st != ST_OK:
        reason = data.decode("ascii", "replace") if data else ""
        raise LinkError("%s -> %s %s" % (name, st_name(st), reason))


# --------------------------------------------------------------------------- bench steps

def step_hello(link):
    st, data = link.request(CMD_HELLO)
    expect_ok("HELLO", st, data)
    print("HELLO: proto=%u fw='%s'" % (data[0], data[1:].decode("ascii", "replace")))
    if data[0] != 1:
        raise LinkError("unexpected protocol version %u" % data[0])


def step_status(link, label="STATUS"):
    st, data = link.request(CMD_STATUS)
    expect_ok("STATUS", st, data)
    s = parse_status(data)
    print("%s: %s" % (label, format_status(s)))
    return s


def step_scan(link, timeout=20.0):
    st, data = link.request(CMD_WIFI_SCAN)
    print("WIFI_SCAN: %s" % st_name(st))
    if st not in (ST_OK,):
        return
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        st, data = link.request(CMD_WIFI_LIST)
        if st == ST_BUSY:
            time.sleep(0.5)
            continue
        expect_ok("WIFI_LIST", st, data)
        aps = parse_wifi_list(data)
        print("WIFI_LIST: %d networks" % len(aps))
        for rssi, auth, ssid in aps:
            print("  %4d dBm  %s  %s" % (rssi, "lock" if auth else "open", ssid))
        return
    raise LinkError("scan did not finish in %.0f s" % timeout)


def wait_connected(link, timeout, poll):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        st, data = link.request(CMD_STATUS)
        expect_ok("STATUS", st, data)
        s = parse_status(data)
        line = format_status(s)
        if line != last:
            print("  %s" % line)
            last = line
        if s["state"] == 2:
            return True
        if s["state"] == 3:
            return False
        time.sleep(poll)
    print("  gave up after %.0f s" % timeout)
    return False


def step_wifi_join(link, ssid, pw):
    st, data = link.request(CMD_WIFI_JOIN, wifi_join_payload(ssid, pw))
    expect_ok("WIFI_JOIN", st, data)
    print("WIFI_JOIN '%s': OK, waiting" % ssid)
    return wait_connected(link, 30.0, 0.5)


def step_lte_join(link, apn):
    st, data = link.request(CMD_LTE_JOIN, lte_join_payload(apn))
    expect_ok("LTE_JOIN", st, data)
    print("LTE_JOIN apn='%s': OK, waiting (up to 200 s)" % apn)
    return wait_connected(link, 200.0, 1.0)


def step_http(link, url, chunk):
    t0 = time.monotonic()
    st, data = link.request(CMD_HTTP_GET, http_get_payload(url))
    expect_ok("HTTP_GET", st, data)
    code, length = struct.unpack("<HI", data[:6])
    print("HTTP_GET %s -> code=%u length=%s (%.2f s)" % (url, code, "unknown" if length == 0xFFFFFFFF else length,
                                                       time.monotonic() - t0))
    body = bytearray()
    dup_checked = False
    while True:
        st, data = link.request(CMD_HTTP_READ, struct.pack("<H", chunk))
        expect_ok("HTTP_READ", st, data)
        if not dup_checked and data:
            # Same cmd+seq again must return the cached response, not the next bytes.
            st2, data2 = link.resend_last()
            if (st2, data2) != (st, data):
                raise LinkError("retransmit cache mismatch on HTTP_READ")
            print("  retransmit cache: duplicate HTTP_READ answered identically (%d bytes)" % len(data))
            dup_checked = True
        if not data:
            break
        body += data
    dt = time.monotonic() - t0
    st, data = link.request(CMD_HTTP_CLOSE)
    expect_ok("HTTP_CLOSE", st, data)
    print("BODY: %d bytes, crc32=%08X, %.2f s, %.1f KB/s" % (len(body), zlib.crc32(body) & 0xFFFFFFFF, dt,
                                                           len(body) / 1024.0 / dt if dt else 0))
    if length != 0xFFFFFFFF and length != len(body):
        raise LinkError("length mismatch: header %u, received %u" % (length, len(body)))
    if body[:1] in (b"{", b"["):
        print("  starts with: %s" % bytes(body[:80]).decode("utf-8", "replace").replace("\n", " "))
    return body


def run_bench(args):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is required: pip install pyserial")

    with serial.Serial(args.port, args.baud, timeout=0.01) as port:
        port.reset_input_buffer()
        link = Link(port, args.verbose)
        ok = True
        try:
            step_hello(link)
            status = step_status(link)
            if not args.no_scan:
                step_scan(link)
            joined = False
            if args.ssid is not None:
                joined = step_wifi_join(link, args.ssid, args.pw)
                ok &= joined
                if joined:
                    step_http(link, args.url, args.chunk)
            if args.apn is not None:
                joined = step_lte_join(link, args.apn)
                ok &= joined
                if joined:
                    step_http(link, args.url, args.chunk)
            if args.ssid is None and args.apn is None:
                if status["state"] == 2 and args.no_scan:
                    step_http(link, args.url, args.chunk)
                else:
                    print("HTTP test skipped (give --ssid/--apn, or --no-scan while already connected)")
            step_status(link, "STATUS (end)")
            if args.off:
                st, data = link.request(CMD_OFF)
                expect_ok("OFF", st, data)
                print("OFF: OK")
        except LinkError as e:
            print("FAIL: %s" % e)
            ok = False
        d = link.decoder
        print("link: retransmits=%d crc_errors=%d timeouts=%d oversize=%d" % (link.retransmits, d.crc_errors,
                                                                             d.timeouts, d.oversize))
        return 0 if ok else 1


# --------------------------------------------------------------------------- self test

class _FakeEsp:
    """Minimal ESP model for the self test: answers HELLO/HTTP_READ, keeps the
    retransmit cache like link.cpp, and can drop responses to force retries."""

    def __init__(self):
        self.decoder = FrameDecoder()
        self.out = bytearray()
        self.cache = None
        self.executed = 0
        self.drop_next = 0
        self.counter = 0

    def write(self, data):
        for cmd, seq, payload in self.decoder.feed(data):
            if self.cache and self.cache[0] == cmd and self.cache[1] == seq:
                frame = self.cache[2]
            else:
                self.executed += 1
                if cmd == CMD_HELLO:
                    resp = bytes([ST_OK, 1]) + b"FAKE 0"
                elif cmd == CMD_HTTP_READ:
                    self.counter += 1
                    resp = bytes([ST_OK]) + bytes([self.counter]) * 4
                else:
                    resp = bytes([ST_UNKNOWN_CMD])
                frame = encode_frame(cmd | RESP_FLAG, seq, resp)
                self.cache = (cmd, seq, frame)
            if self.drop_next:
                self.drop_next -= 1
            else:
                self.out += frame

    @property
    def in_waiting(self):
        return len(self.out)

    def read(self, n):
        data = bytes(self.out[:n])
        del self.out[:n]
        return data


def selftest():
    failures = []

    def check(name, cond):
        print("  [%s] %s" % ("ok" if cond else "FAIL", name))
        if not cond:
            failures.append(name)

    print("CRC-16/CCITT-FALSE")
    check('crc("123456789") == 0x29B1', crc16_ccitt_false(b"123456789") == 0x29B1)
    check('crc("") == 0xFFFF', crc16_ccitt_false(b"") == 0xFFFF)
    rnd = random.Random(1234)
    blobs = [bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 1200))) for _ in range(50)]
    check("matches binascii.crc_hqx(init=0xFFFF) on 50 random blobs",
          all(crc16_ccitt_false(b) == binascii.crc_hqx(b, 0xFFFF) for b in blobs))

    print("Frame encoder")
    hello = encode_frame(CMD_HELLO, 1)
    check("HELLO seq=1 == a5 5a 01 01 00 00 + crc(01 01 00 00) LE",
          hello == bytes([0xA5, 0x5A, 0x01, 0x01, 0x00, 0x00]) + struct.pack("<H", binascii.crc_hqx(b"\x01\x01\x00\x00", 0xFFFF)))
    check("HELLO seq=1 == a5 5a 01 01 00 00 44 c5 (fixed vector)", hello.hex() == "a55a0101000044c5")
    read = encode_frame(CMD_HTTP_READ, 0x07, struct.pack("<H", 1024))
    check("HTTP_READ seq=7 max=1024 == a5 5a 31 07 02 00 00 04 04 ae (fixed vector)",
          read.hex() == "a55a31070200000404ae")
    try:
        encode_frame(CMD_HTTP_GET, 0, bytes(MAX_PAYLOAD + 1))
        check("encoder rejects len > 1100", False)
    except ValueError:
        check("encoder rejects len > 1100", True)

    print("Frame decoder")
    frames = [(rnd.randrange(0x80), rnd.randrange(256), bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 1101))))
              for _ in range(30)]
    stream = b"".join(b"\x00\xa5\x11" + encode_frame(c, s, p) for c, s, p in frames)
    dec = FrameDecoder()
    got = []
    for i in range(0, len(stream), 7):  # small chunks, same timestamp
        got += dec.feed(stream[i:i + 7], now=0.0)
    check("30 random frames with garbage between, fed in 7-byte chunks", got == frames)

    dec = FrameDecoder()
    bad = bytearray(encode_frame(CMD_STATUS, 5, b"abc"))
    bad[-1] ^= 0x01
    check("bad CRC dropped", dec.feed(bytes(bad), now=0.0) == [] and dec.crc_errors == 1)

    dec = FrameDecoder()
    over = bytes([0xA5, 0x5A, 0x30, 0x01]) + struct.pack("<H", MAX_PAYLOAD + 1)
    good = encode_frame(CMD_HELLO, 9)
    check("len 1101 dropped, next frame still decodes",
          dec.feed(over + good, now=0.0) == [(CMD_HELLO, 9, b"")] and dec.oversize == 1)

    dec = FrameDecoder()
    f = encode_frame(CMD_STATUS, 3)
    r1 = dec.feed(f[:4], now=0.0)
    r2 = dec.feed(f[4:], now=0.060)  # > 50 ms gap mid-frame
    r3 = dec.feed(f, now=0.070)
    check("50 ms inter-byte timeout resets parser", r1 == [] and r2 == [] and r3 == [(CMD_STATUS, 3, b"")]
          and dec.timeouts == 1)

    dec = FrameDecoder()
    check("'A5 A5 5A' still syncs", dec.feed(b"\xa5" + encode_frame(CMD_OFF, 4), now=0.0) == [(CMD_OFF, 4, b"")])

    print("Payload helpers")
    s = parse_status(bytes([2, 2, 0xB5, 10, 1, 2, 3]) + b"SKT/lte.sktelecom.com")
    check("STATUS parse (rssi int8, ip a.b.c.d)", s == {"mode": 2, "state": 2, "rssi": -75, "ip": "10.1.2.3",
                                                       "detail": "SKT/lte.sktelecom.com"})
    lst = bytes([2, 0xC4, 1, 4]) + b"home" + bytes([0xA6, 0, 4]) + b"cafe"
    check("WIFI_LIST parse", parse_wifi_list(lst) == [(-60, 1, "home"), (-90, 0, "cafe")])
    check("WIFI_JOIN payload", wifi_join_payload("ab", "12345678") == b"\x02ab\x0812345678")
    check("HTTP_GET payload", http_get_payload("https://x/") == b"\x0a\x00https://x/")

    print("Retransmit (fake ESP)")
    esp = _FakeEsp()
    link = Link(esp)
    st, data = link.request(CMD_HELLO)
    check("HELLO through fake ESP", st == ST_OK and data[0] == 1)
    esp.drop_next = 2
    st, data = link.request(CMD_HTTP_READ, struct.pack("<H", 4), timeout=0.05)
    check("lost responses -> same seq resent, command executed once",
          st == ST_OK and data == b"\x01" * 4 and link.retransmits == 2 and esp.executed == 2)
    st2, data2 = link.resend_last()
    check("explicit duplicate answered from cache", (st2, data2) == (st, data) and esp.executed == 2)
    st, data = link.request(CMD_HTTP_READ, struct.pack("<H", 4))
    check("next seq executes again", data == b"\x02" * 4 and esp.executed == 3)

    print("selftest: %s" % ("PASS" if not failures else "FAIL (%d)" % len(failures)))
    return 0 if not failures else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port of the USB-UART adapter (e.g. COM7, /dev/ttyUSB0)")
    ap.add_argument("baud", nargs="?", type=int, default=921600)
    ap.add_argument("--selftest", action="store_true", help="run offline checks and exit")
    ap.add_argument("--ssid", help="join this Wi-Fi network")
    ap.add_argument("--pw", default="", help="Wi-Fi password")
    ap.add_argument("--apn", help="start LTE with this APN ('' = network default)")
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--chunk", type=int, default=1024, help="HTTP_READ max (1..1024)")
    ap.add_argument("--no-scan", action="store_true", help="skip WIFI_SCAN (it switches LTE off)")
    ap.add_argument("--off", action="store_true", help="send OFF at the end")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.port:
        ap.error("port is required (or use --selftest)")
    if not 1 <= args.chunk <= 1024:
        ap.error("--chunk must be 1..1024")
    return run_bench(args)


if __name__ == "__main__":
    sys.exit(main())
