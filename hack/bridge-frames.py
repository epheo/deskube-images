#!/usr/bin/env python3
"""Streams from the web bridge as its browser client does on the
WebSocket transport, and counts the video frames it is sent.

bridge-frames.py URL HOST_ID APP_ID ["Header: value"]
URL is the bridge's API, such as http://127.0.0.1:8080/api.
SIZE=1920x1080 FPS=60 BITRATE=20000 CODEC=h264|hevc|av1 SECS=20
MAX_KEY_FRAMES=3

Standard library only: a CI runner has no WebSocket module.
"""
import base64
import json
import os
import socket
import struct
import sys
import time
from urllib.parse import urlsplit

# The bridge's WebSocketChannel: the first byte of a binary message.
VIDEO, AUDIO = 1, 2
# moonlight-common's VideoFormats bits.
CODECS = {"h264": 0x0001, "hevc": 0x0100, "av1": 0x1000}


def send(sock, opcode, payload=b""):
    # A client masks what it sends, RFC 6455 5.3.
    mask = os.urandom(4)
    n = len(payload)
    head = bytes([0x80 | opcode])
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 1 << 16:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def messages(rd, sock):
    """Yields (opcode, payload) of whole messages and of pings, answered."""
    kind, parts = None, []
    while True:
        head = rd.read(2)
        if len(head) < 2:
            return
        fin, opcode, n = head[0] & 0x80, head[0] & 0x0F, head[1] & 0x7F
        if n == 126:
            n = struct.unpack(">H", rd.read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", rd.read(8))[0]
        payload = rd.read(n)
        if len(payload) < n or opcode == 0x8:
            return
        if opcode == 0x9:
            # The bridge reads the link's delay from these and stops
            # sending video to a client that leaves them unanswered.
            send(sock, 0xA, payload)
            yield opcode, payload
        elif opcode in (0x0, 0x1, 0x2):
            kind = opcode or kind
            parts.append(payload)
            if fin:
                yield kind, b"".join(parts)
                parts = []


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    url, host_id, app_id = urlsplit(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    width, height = (int(v) for v in os.environ.get("SIZE", "1920x1080").split("x"))
    fps = int(os.environ.get("FPS", "60"))
    secs = float(os.environ.get("SECS", "20"))
    codec = CODECS[os.environ.get("CODEC", "h264")]

    sock = socket.create_connection((url.hostname, url.port or 80), timeout=30)
    request = [
        f"GET {url.path}/host/stream/web_socket HTTP/1.1",
        f"Host: {url.netloc}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        "Sec-WebSocket-Version: 13",
        "Sec-WebSocket-Key: " + base64.b64encode(os.urandom(16)).decode(),
        *sys.argv[4:],
    ]
    sock.sendall(("\r\n".join(request) + "\r\n\r\n").encode())
    rd = sock.makefile("rb")
    status = rd.readline().decode().strip()
    if " 101 " not in status:
        sys.exit(f"no WebSocket from the bridge: {status}")
    while rd.readline() not in (b"\r\n", b""):
        pass

    send(sock, 0x1, json.dumps({"Request": {
        "host_id": host_id, "app_id": app_id,
        "width": width, "height": height, "fps": fps,
        "bitrate": int(os.environ.get("BITRATE", "20000")),
        "hdr": False, "local_audio_play_mode": False,
        "supported_codecs": codec, "preferred_codecs": codec,
    }}).encode())

    frames = idr = audio = size = unordered = 0
    first, asked, cut, stamp = None, time.monotonic(), False, None
    try:
        for kind, data in messages(rd, sock):
            # Pings come ten times a second, so a still picture ends on
            # time too, and so does a stream that never shows one.
            now = time.monotonic()
            if now - (first or asked) > (secs if first else 60):
                break
            if kind == 0x1:
                message = json.loads(data)
                if "Response" in message:
                    print("Stream started:", json.dumps(message["Response"]), flush=True)
            elif kind != 0x2 or not data:
                continue
            elif data[0] == VIDEO:
                first = first or now
                frames += 1
                idr += data[1]
                size += len(data)
                # Microseconds since the stream began, 32 bits that wrap.
                # A frame sent twice or out of order shows here, not in the count.
                last, stamp = stamp, int.from_bytes(data[2:6], "big")
                if last is not None and not 0 < (stamp - last) & 0xFFFFFFFF < 1 << 31:
                    unordered += 1
            elif data[0] == AUDIO:
                audio += 1
        else:
            cut = True
    except TimeoutError:
        cut = True
    if not first:
        sys.exit("Video frames received: 0")
    # The whole window counts, the part with no frame too: a stream that
    # stalls must not keep the rate of its good seconds.
    took = min(secs, time.monotonic() - first)
    # The bridge ends the Sunshine session when the client says goodbye.
    try:
        send(sock, 0x8, struct.pack(">H", 1000))
        sock.settimeout(5)
        while rd.read(65536):
            pass
    except OSError:
        pass

    print(f"Video frames received: {frames} in {took:.1f} s, {frames / max(took, 1):.1f} fps, "
          f"{size * 8 / max(took, 1) / 1e6:.1f} Mbit/s; key frames: {idr}; audio packets: {audio}")
    if cut:
        sys.exit(f"the bridge ended the stream before {secs:.0f} s")
    if unordered:
        sys.exit(f"{unordered} frames came with a timestamp that is not after the frame before")
    # One key frame opens a stream. A capture that restarts on every frame
    # sends nothing else, and the bridge asks for one each time it drops
    # video for a client it finds late: both keep the count of frames up.
    most = int(os.environ.get("MAX_KEY_FRAMES", "3"))
    if idr > most:
        sys.exit(f"{idr} key frames, more than {most}")


main()
