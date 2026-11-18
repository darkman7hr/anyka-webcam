#!/usr/bin/env python3
"""Run shell commands on the hacked Anyka AK3918 camera over telnet.

Usage:
    ./cam.py 'ls -l /tmp'
    ./cam.py --timeout 20 'cat /proc/meminfo'
    echo 'ls' | ./cam.py -            # read commands from stdin

The camera (192.168.10.46) is a busybox armv5tejl box running the "Gerge"
AK3918 SD-card hack. Telnet hands out a root shell; the password is not
enforced (any string is accepted).

It speaks a minimal telnet negotiation (refuse everything) and delimits each
command with a unique marker so output can be captured cleanly.
"""

import os
import random
import re
import socket
import sys
import time

HOST = os.environ.get("CAM_HOST", "192.168.10.46")
PORT = int(os.environ.get("CAM_PORT", "23"))

IAC, WILL, WONT, DO, DONT = 255, 251, 252, 253, 254


def _negotiate(data):
    """Reply to every telnet command with WONT/DONT: we want raw bytes."""
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i] == IAC and i + 2 < len(data):
            cmd, opt = data[i + 1], data[i + 2]
            if cmd == WILL:
                out += bytes([IAC, DONT, opt])
            elif cmd == DO:
                out += bytes([IAC, WONT, opt])
            i += 3
        else:
            i += 1
    return bytes(out)


class Cam:
    def __init__(self, host=HOST, port=PORT, user="root", password="anyka"):
        self.s = socket.create_connection((host, port), timeout=10)
        self.s.settimeout(2)
        self._drain(1.0)
        self.s.sendall((user + "\r\n").encode())
        self._drain(0.8)
        self.s.sendall((password + "\r\n").encode())
        out = self._drain(1.2)
        if b"#" not in out and b"$" not in out:
            raise RuntimeError("login failed: %r" % out[-200:])
        # Turn the tty echo off. Left on, every command is typed back into the
        # stream and no marker scheme can tell echo apart with real output.
        self.s.sendall(b"stty -echo 2>/dev/null; PS1='@> '; echo CAMREADY\n")
        for _ in range(10):
            if b"CAMREADY" in self._drain(0.6):
                break

    def _drain(self, wait=1.0):
        """Collect bytes for up to `wait` per quiet spell.

        Telnet negotiation is answered only for bytes we have not answered
        before; replaying the whole buffer would re-send stale IAC replies.
        """
        buf = b""
        handled = 0
        self.s.settimeout(wait)
        try:
            while True:
                b = self.s.recv(65536)
                if not b:
                    break
                buf += b
                n = _negotiate(buf[handled:])
                handled = len(buf)
                if n:
                    self.s.sendall(n)
        except socket.timeout:
            pass
        return buf

    def run(self, cmd, timeout=30):
        """Run one command and return its output.

        Echo is off (set in __init__), so a random literal marker is safe:
        nothing we type can appear in the stream on its own.
        """
        marker = "CAMEND%d" % random.randint(100000, 999999)
        self.s.sendall((cmd + "\n").encode())
        self.s.sendall(("echo %s\n" % marker).encode())
        deadline = time.time() + timeout
        buf = b""
        while time.time() < deadline:
            buf += self._drain(0.4)
            if marker.encode() in buf:
                break
        text = buf.decode("latin1", "replace")
        i = text.find(marker)
        if i >= 0:
            text = text[:i]
        text = re.sub(r"^@> ?\r?\n?", "", text, flags=re.M)
        return text.replace("\r\n", "\n")

    def readfile(self, path):
        """Return a file's contents exactly.

        Wrapping in sentinels matters: a file with no trailing newline runs
        straight into the shell prompt, so a plain `cat` and strip gets you
        the prompt glued onto the last line.
        """
        out = self.run("echo \"@@$(cat '%s' 2>/dev/null)@@\"" % path)
        m = re.search(r"@@(.*)@@", out, re.S)
        return m.group(1) if m else ""

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def main():
    args = sys.argv[1:]
    timeout = 30
    if args and args[0] == "--timeout":
        timeout = int(args[1])
        args = args[2:]
    cmd = " ".join(args) if args else sys.stdin.read()
    cam = Cam()
    try:
        for chunk in cmd.strip().split("\n\n"):
            if chunk.strip():
                sys.stdout.write(cam.run(chunk, timeout))
    finally:
        cam.close()


if __name__ == "__main__":
    main()
