#!/usr/bin/env python3
"""
rtsp_probe.py - measure an RTSP stream without trusting any player's arithmetic.

Why this exists: ffmpeg's `-t SECS` on an RTSP input limits *output* duration, and
on a raw H.264 output that duration comes from the timestamps the server sends, so
a timestamp bug shows up as "385 frames in 4 seconds" rather than as an error.
This counts what actually arrives against the wall clock and reports the RTP
timestamps it saw, which is the number that has to be right for an NVR and for
WebRTC.

  ./tools/rtsp_probe.py rtsp://192.168.10.46:8554/vs1 -t 6
  ./tools/rtsp_probe.py rtsp://192.168.10.46:554/vs1 -t 6     # the vendor server

TCP interleaved only, which is what this network actually uses (measured: the NVR
holds two TCP connections and there is not one UDP socket on the box).
"""

import argparse
import socket
import struct
import sys
import time


def read_headers(sock):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("server closed during response headers")
        buf += chunk
        if len(buf) > 200000:
            raise RuntimeError("response too large")
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode(errors="replace").split("\r\n")
    hdrs = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            hdrs[k.strip().lower()] = v.strip()
    # a small SDP routinely arrives in its own segment, so "headers seen" is not
    # "response seen": keep reading until Content-Length is satisfied, otherwise
    # the body is mistaken for the next response
    cl = int(hdrs.get("content-length", 0) or 0)
    while len(rest) < cl:
        chunk = sock.recv(4096)
        if not chunk:
            break
        rest += chunk
    return lines[0], hdrs, rest


def send_req(sock, method, url, extra=""):
    """
    Deliberately minimal. The vendor server on this box resets the connection on a
    DESCRIBE that carries a User-Agent/Connection pair it does not expect, and
    ignores a DESCRIBE that follows an OPTIONS on the same connection; CSeq plus
    whatever the method needs is what it actually answers.
    """
    global CSEQ
    CSEQ += 1
    req = "%s %s RTSP/1.0\r\nCSeq: %d\r\n" % (method, url, CSEQ)
    if extra:
        if not extra.endswith("\r\n"):
            extra += "\r\n"
        req += extra
    req += "\r\n"		# the blank line that ends the request; without it the
				# vendor server resets the connection
    sock.sendall(req.encode())


CSEQ = 0


def split_url(url):
    rest = url.split("://", 1)[1]
    hostport, _, path = rest.partition("/")
    host, _, port = hostport.partition(":")
    return host, int(port or 554), "/" + path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("-t", "--seconds", type=float, default=5.0)
    ap.add_argument("--transport", default="RTP/AVP/TCP;unicast;interleaved=0-1")
    ap.add_argument("--options", action="store_true",
                    help="send OPTIONS first (the vendor server will then not answer DESCRIBE)")
    args = ap.parse_args()

    host, port, path = split_url(args.url)
    url = "rtsp://%s:%d%s" % (host, port, path)
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(10)

    # DESCRIBE first, and deliberately no OPTIONS: measured on this box, the
    # vendor server never answers a DESCRIBE that follows an OPTIONS on the same
    # connection (it answers either one alone). Use --options to ask separately.
    if args.options:
        send_req(sock, "OPTIONS", url)
        print("OPTIONS  ", read_headers(sock)[0])
    send_req(sock, "DESCRIBE", url, "Accept: application/sdp")
    st, hdrs, body = read_headers(sock)
    print("DESCRIBE ", st, "sdp bytes:", len(body))
    for ln in body.decode(errors="replace").split("\r\n"):
        if ln.startswith(("m=", "a=rtpmap", "a=fmtp", "a=control")):
            print("   ", ln[:160])

    # the media control attribute is the client's choice to get right: the vendor
    # serves video and audio, so pick the control that follows m=video rather than
    # assuming "trackID=0"
    setup_url = url
    lines = body.decode(errors="replace").split("\r\n")
    for i, ln in enumerate(lines):
        if ln.startswith("m=video"):
            for nx in lines[i + 1:i + 12]:
                if nx.startswith("a=control:"):
                    ctl = nx.split(":", 1)[1].strip()
                    setup_url = ctl if ctl.startswith("rtsp://") else url.rstrip("/") + "/" + ctl
                    break
            break
    send_req(sock, "SETUP", setup_url, "Transport: " + args.transport)
    st, hdrs, _ = read_headers(sock)
    print("SETUP    ", st, hdrs.get("session", ""))
    if "200" not in st:
        return 2
    sess = hdrs.get("session", "1").split(";")[0]

    send_req(sock, "PLAY", url, "Session: %s\r\nRange: npt=now-" % sess)
    st, _, _ = read_headers(sock)
    print("PLAY     ", st)
    if "200" not in st:
        return 2
    return measure(sock, url, sess, args.seconds)


def measure(sock, url, sess, seconds):
    buf = b""
    t0 = time.monotonic()
    t_end = t0 + seconds
    pkts = frames = keyframes = nbytes = 0
    ts_first = ts_last = seq_prev = None
    seq_gaps = 0
    deltas = []
    sock.settimeout(5)
    try:
        while time.monotonic() < t_end:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                print("recv timed out - server sent nothing")
                break
            if not chunk:
                print("connection closed by server")
                break
            buf += chunk
            while len(buf) >= 4 and buf[0] == 0x24:	# '$' interleaved framing
                clen = struct.unpack(">H", buf[2:4])[0]
                if len(buf) < 4 + clen:
                    break
                pkt, buf = buf[4:4 + clen], buf[4 + clen:]
                if len(pkt) < 12 or pkt[0] >> 6 != 2:
                    continue
                seq = struct.unpack(">H", pkt[2:4])[0]
                ts = struct.unpack(">I", pkt[4:8])[0]
                marker = bool(pkt[1] & 0x80)
                pkts += 1
                nbytes += clen
                if seq_prev is not None and seq != (seq_prev + 1) & 0xFFFF:
                    seq_gaps += 1
                seq_prev = seq
                if ts_first is None:
                    ts_first = ts
                if ts_last is not None:
                    deltas.append((ts - ts_last) & 0xFFFFFFFF)
                ts_last = ts
                if marker:
                    frames += 1
                    payload = pkt[12:]
                    if payload:
                        nt = payload[0] & 0x1F
                        if nt == 28 and len(payload) > 1:	# FU-A: real type is in the next byte
                            nt = payload[1] & 0x1F
                        if nt == 5:
                            keyframes += 1
    finally:
        el = time.monotonic() - t0
        send_req(sock, "TEARDOWN", url, "Session: %s" % sess)
        try:
            read_headers(sock)
        except Exception:
            pass
        sock.close()

    print()
    print("measured over %0.2f s of wall clock:" % el)
    print("  rtp packets   %d (%d bytes, %d kbps)" % (pkts, nbytes, nbytes * 8 / el / 1000))
    print("  access units  %d -> %0.2f fps (marker bit)" % (frames, frames / el))
    print("  key frames    %d -> one every %0.2f s" % (keyframes, el / max(keyframes, 1)))
    print("  seq gaps      %d" % seq_gaps)
    if ts_first is not None and ts_last is not None and frames > 1:
        span = (ts_last - ts_first) & 0xFFFFFFFF
        print("  rtp ts        %u -> %u = %d ticks = %0.2f media s over %0.2f s wall (%0.2f media s per wall s)"
              % (ts_first, ts_last, span, span / 90000.0, el, span / 90000.0 / el))
        neg = sum(1 for d in deltas if d > 0x7FFFFFFF)
        print("  backwards ts  %d%s" % (neg, "  <-- every player will object to this" if neg else ""))
        if deltas:
            uniq = sorted(set(d for d in deltas if d <= 0x7FFFFFFF))[:6]
            print("  ts deltas     %s%s" % (uniq, " ..." if len(set(deltas)) > 6 else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
