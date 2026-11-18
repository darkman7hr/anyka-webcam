#!/usr/bin/env python3
"""MJPEG/JPEG relay: turns the camera's RTSP into an <img>-able URL.

The camera cannot do this itself. Its 36 MB cannot hold ffmpeg (instant
`Killed`), and the only process that can serve JPEG on :3000 (libre_anyka_app)
dies shortly after "capture start" when the vendor rtsp binary has already been
running. So the stream is decoded here instead, and the web UI is pointed at
this relay's URL.

    ./relay.py                      # serve on 0.0.0.0:8124
    ./relay.py --port 8124 --src rtsp://192.168.10.46:554/vs1 --width 640

Endpoints
    /preview.jpg   newest single JPEG (what the UI's <img> polls)
    /mjpeg         multipart/x-mixed-replace stream
    /              one-line status
"""

import argparse
import shutil
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

lock = threading.Lock()
latest = {"jpg": b"", "t": 0, "frames": 0}
stop = threading.Event()


def frame_reader(src, width, fps):
    """Decode RTSP to a JPEG stream and keep only the newest frame.

    image2pipe emits JPEGs back to back; a frame boundary is the EOI marker, so
    the buffer is cut there rather than trusting any framing ffmpeg might add.
    """
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-rtsp_transport", "tcp", "-i", src,
        "-vf", "scale=%d:-2" % width, "-r", str(fps),
        "-f", "image2pipe", "-vcodec", "mjpeg", "-q:v", "6", "-an", "-",
    ]
    while not stop.is_set():
        print("start:", " ".join(cmd), flush=True)
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        buf = b""
        try:
            while not stop.is_set():
                chunk = p.stdout.read(65536)
                if not chunk:
                    break
                buf += chunk
                while True:
                    eoi = buf.find(b"\xff\xd9")
                    if eoi < 0:
                        break
                    frame = buf[:eoi + 2]
                    buf = buf[eoi + 2:]
                    if len(frame) > 500 and frame[:2] == b"\xff\xd8":
                        with lock:
                            latest["jpg"] = frame
                            latest["t"] = time.time()
                            latest["frames"] += 1
                if len(buf) > 4_000_000:      # runaway guard, never expected
                    buf = b""
        finally:
            p.terminate()
            err = p.stderr.read().decode("utf-8", "replace").strip()
            if err:
                print("ffmpeg:", err[:300], flush=True)
            time.sleep(2)                    # back off, then reconnect


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body, extra=None):
        if isinstance(body, str):        # status text; frames are already bytes
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-type", ctype)
        self.send_header("Content-length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/preview.jpg":
            with lock:
                body = latest["jpg"]
            if not body:
                return self._send(503, "text/plain", b"no frame yet")
            return self._send(200, "image/jpeg", body)
        if path == "/mjpeg":
            boundary = "relayframe"
            self.send_response(200)
            self.send_header("Content-type", "multipart/x-mixed-replace; boundary=%s" % boundary)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            last = 0
            try:
                while not stop.is_set():
                    with lock:
                        n, body = latest["frames"], latest["jpg"]
                    if n != last and body:
                        last = n
                        self.wfile.write(b"--%s\r\nContent-type: image/jpeg\r\nContent-length: %d\r\n\r\n" % (boundary.encode(), len(body)))
                        self.wfile.write(body)
                        self.wfile.write(b"\r\n")
                    time.sleep(0.2)
            except Exception:
                pass
            return
        with lock:
            n, age = latest["frames"], (time.time() - latest["t"]) if latest["t"] else -1
        return self._send(200, "text/plain", "frames=%d age=%.1fs\n" % (n, age))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="rtsp://192.168.10.46:554/vs1")
    ap.add_argument("--port", type=int, default=8124)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--fps", type=int, default=10)
    a = ap.parse_args()
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg is required on this machine")
    threading.Thread(target=frame_reader, args=(a.src, a.width, a.fps), daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), H)
    print("relay on http://0.0.0.0:%d/preview.jpg  (src %s)" % (a.port, a.src), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()
