#!/usr/bin/env python3
"""Minimal ESP32-DIV *companion* simulator speaking divlink over TCP.

Pairs with the Tab5 host firmware running under esp-emu with
`--uart1-tcp 127.0.0.1:PORT`. Answers HELLO with CAPS+ACK and ACKs commands,
so the host's `link.linked()` goes true and menu selections are acknowledged.

Usage: companion_sim.py [PORT=5560]
"""
import socket, struct, sys, time

SOF_DELIM = 0x00
PROTO_VER = 1
T_HELLO, T_CAPS, T_HB = 0x01, 0x02, 0x03
T_CMD, T_ACK, T_NAK = 0x10, 0x11, 0x12
T_EVT, T_STREAM, T_BLOB, T_CANVAS, T_INPUT, T_LOG = 0x20,0x21,0x30,0x40,0x41,0x50
CMD_HELLO, CMD_FEATURE_START = 0x01, 0x02

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

def cobs_encode(data: bytes) -> bytes:
    out = bytearray(); idx = 0
    while True:
        z = data.find(0, idx)
        block = data[idx:] if z < 0 else data[idx:z]
        while len(block) >= 254:
            out.append(0xFF); out += block[:254]; block = block[254:]
        out.append(len(block) + 1); out += block
        if z < 0: break
        idx = z + 1
        if idx == len(data):        # trailing zero -> empty final block
            out.append(1); break
    return bytes(out)

def cobs_decode(data: bytes) -> bytes:
    out = bytearray(); i = 0
    while i < len(data):
        code = data[i]; i += 1
        if code == 0: return b''
        out += data[i:i+code-1]; i += code - 1
        if code < 0xFF and i < len(data): out.append(0)
    return bytes(out)

def encode_frame(ftype, chan, seq, payload=b'') -> bytes:
    hdr = struct.pack('<BBBHH', PROTO_VER, ftype, chan, seq, len(payload))
    body = hdr + payload
    body += struct.pack('<H', crc16(body))
    return cobs_encode(body) + b'\x00'

def parse_stream(buf: bytearray):
    """Yield (ftype, chan, seq, payload); mutate buf to drop consumed frames."""
    while True:
        z = buf.find(0)
        if z < 0: return
        block = bytes(buf[:z]); del buf[:z+1]
        if not block: continue
        dec = cobs_decode(block)
        if len(dec) < 9: continue
        body, crc = dec[:-2], struct.unpack('<H', dec[-2:])[0]
        if crc16(body) != crc: continue
        ver, ftype, chan, seq, ln = struct.unpack('<BBBHH', body[:7])
        payload = body[7:7+ln]
        if len(payload) != ln: continue
        yield ftype, chan, seq, payload

def caps_payload():
    # Caps: proto_ver, fw_major_minor, has_psram, bitmap0, bitmap1
    return struct.pack('<BHBII', PROTO_VER, 0x0107, 1, 0xFFFFFFFF, 0x0)

def connect_client(host, port, timeout_s=15):
    """esp-emu --uart1-tcp opens a TCP *server*; we connect to it (with retries)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            c = socket.create_connection((host, port), timeout=2)
            print(f"[companion_sim] connected to emu UART1 at {host}:{port}", flush=True)
            return c
        except OSError:
            time.sleep(0.3)
    raise SystemExit(f"[companion_sim] could not connect to {host}:{port}")

def main():
    # Modes:  companion_sim.py [PORT]                 -> listen (server)
    #         companion_sim.py connect HOST PORT      -> connect (esp-emu UART1)
    if len(sys.argv) >= 2 and sys.argv[1] == 'connect':
        host = sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1'
        port = int(sys.argv[3]) if len(sys.argv) > 3 else 5560
        conn = connect_client(host, port)
        s = None
    else:
        port = int(sys.argv[1]) if len(sys.argv) > 1 else 5560
        s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(('127.0.0.1', port)); s.listen(1)
        print(f"[companion_sim] listening on 127.0.0.1:{port}", flush=True)
        conn, _ = s.accept(); print("[companion_sim] host connected", flush=True)
    buf = bytearray()
    conn.settimeout(20)
    try:
        while True:
            data = conn.recv(4096)
            if not data: break
            buf += data
            for ftype, chan, seq, payload in parse_stream(buf):
                if ftype == T_CMD and payload[:1] == bytes([CMD_HELLO]):
                    conn.sendall(encode_frame(T_CAPS, chan, seq, caps_payload()))
                    conn.sendall(encode_frame(T_ACK, chan, seq))
                    print("[companion_sim] HELLO -> CAPS+ACK", flush=True)
                elif ftype == T_CMD:
                    conn.sendall(encode_frame(T_ACK, chan, seq))
                    op = payload[0] if payload else -1
                    print(f"[companion_sim] CMD op=0x{op:02X} chan=0x{chan:02X} -> ACK", flush=True)
                    # push a demo telemetry EVT back
                    conn.sendall(encode_frame(T_LOG, 0, 0, b"companion: feature running"))
                elif ftype == T_INPUT:
                    print("[companion_sim] INPUT event", flush=True)
    except socket.timeout:
        print("[companion_sim] idle timeout, exiting", flush=True)
    finally:
        conn.close()
        if s is not None: s.close()

if __name__ == '__main__':
    main()
