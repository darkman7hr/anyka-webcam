# Install guide

Everything below was run against a real AK3918E + GC1084 camera (the "Gerge"
SD-card hack) at `192.168.10.46`, and the numbers quoted are measured on that
board, not estimates. The commands are generic: set `CAM_HOST` to your camera's
address and they work.

## 0. What you are installing

Two independent things, either of which can be installed alone:

1. **camweb UI** — a replacement web interface (plain HTML/JS plus one `ash`
   CGI) giving PTZ control, 12 stored positions, patrol, drag-to-aim, the white
   LED, reboot, and a token instead of no authentication at all.
2. **On-demand live preview** — `go2rtc` running *on the camera*, started only
   while a browser is watching, serving the camera's own 640x480 substream as
   HLS/fMP4 on `:1984`. The viewer's browser decodes the H.264; the camera only
   muxes and serves. It shuts itself down 300 s after the last viewer.

Measured cost while previewing, on a 36 MB single-core board: `go2rtc` RSS
~9-12 MB, load average rises to ~10 and falls back to ~2.5 within a couple of
minutes after the relay exits, and all ~9 MB of memory comes straight back.
Idle — nobody watching — the camera runs exactly the processes it always ran.

```
browser :8080 page  --act=live_start-->  CGI  -->  run_live.sh (supervisor)
                                                              |
        vendor rtsp :554 /vs1  --ingest-->  go2rtc :1984 (HLS/fMP4, CORS)
                                                              |
        browser  <-- fetch + MediaSource ---  (decodes on the viewer)
```

## 1. What you need on your PC

* `python3` (standard library only — no pip installs) for every tool.
* `curl` for the HTTP checks.
* **Optional**, only for the browser end-to-end check: Google Chrome/Chromium
  (any recent build) and Node 18+. Nothing else needs it.
* The camera must already have the SD-card hack: a writable
  `/mnt/Factory/custom/`, telnet root shell on `:23`, FTP server on `:21`.
  See <https://github.com/openhwproject/anyka3918-gc1084-camera> for that part;
  it is not included here.

## 2. Unpack and point the tools at your camera

```sh
tar xzf anyka-cam-2026-11-17.tar.gz
cd anyka-cam
export CAM_HOST=192.168.10.46        # your camera's address; this is the default
```

`CAM_HOST` is read by every tool (`tools/cam.py`, `deploy.py`, `live.py`,
`relay.py`, and the `.mjs` browser checks). Nothing is installed on the camera
by unpacking.

The access token does not exist yet: the deploy step creates it on the camera
(`/mnt/camweb/token`) and caches it locally in `tools/.token`. Until then every
API call answers `unauthorized`, and the tools say so rather than failing
silently. Which port ours serves is remembered in `tools/.port`; with no such
file the tools probe `:8080` and `:8090` for their own page title, or you can
say it directly: `export CAM_HTTP_PORT=8080`.

The telnet/FTP password for the SD-card hack is the stock `root`/`anyka` pair,
hardcoded in `tools/cam.py` and `tools/deploy.py` — if yours differs, edit
those two lines.

## 3. Check access before installing anything

```sh
cd tools
./cam.py 'cat /proc/loadavg; head -3 /proc/meminfo; ls /mnt/Factory/custom'
```

Expected: three load numbers, `MemTotal: ~36540 kB` on this board, and a listing
that contains `web_interface`. If this fails, nothing else will work — check the
address, that the SD-card hack is in place, and that nothing else is holding the
camera's single telnet session (busybox `telnetd` is picky about that here: if a
previous session is wedged, reboot the camera with `curl http://$CAM_HOST:8080/`
reachable and try again).

## 4. Install the UI beside the stock one — this step is reversible by design

```sh
./deploy.py            # upload, create token + config, serve on :8090
./deploy.py --status
./deploy.py --verify
```

`--verify` runs endpoint checks including injection attempts and JSON
validity. At this point your camera serves **two** interfaces: the stock Factory
page on `:8080` and ours on `:8090`. Open `http://$CAM_HOST:8090/`, paste the
token the deploy printed, and try the PTZ pad. Nothing on the camera has been
changed except new files under `/mnt/camweb/`.

To take the stock port over:

```sh
./deploy.py --promote   # backs up first, then edits config.sh
./deploy.py --revert    # puts the stock Factory page back on :8080
```

`--promote` is deliberately ordered: it copies `config.sh` to
`/mnt/camweb/backup/config.sh.orig` and the whole stock docroot to
`/mnt/camweb/backup/stock_www`, **refuses to edit `config.sh` if those backups
are not there**, replaces the one stock `httpd -p 8080` line with
`/bin/sh /mnt/camweb/run_web.sh &`, syntax-checks the edited file with `sh -n`
and restores the original immediately if it does not parse. `run_web.sh` is a
restart loop, so the page comes back by itself after a crash and after boot.
Everything stock is preserved, never deleted.


## 5. The go2rtc binary — read this before downloading anything

This board is **armv5tejl, soft-float**. The prebuilt binary shipped in this
package is the one that has been running on the camera for hours at a time:

    live/go2rtc-armv5    20 316 320 bytes    md5 aed80b8384e4fc3d9496864a125c8e8f
    $ /mnt/camweb/live/go2rtc -version
    go2rtc version 1.9.15 (7c05066) linux/arm

**The official upstream asset does not work here — tested, not assumed.** The
upstream release asset `go2rtc_linux_arm` for the same v1.9.15 (5 092 480 bytes,
sha256 `cbffe407…6028`) was uploaded to this camera, made executable and run:

    Illegal instruction

Upstream's `linux/arm` build is not ARMv5-safe. What does work is a build pinned
to that same release and cross-compiled for ARMv5 soft-float:

    git clone --depth 1 --branch v1.9.15 https://github.com/AlexxIT/go2rtc
    cd go2rtc          # tag v1.9.15 == commit 7c05066c90ab6ad1fccb506d5f0408aa31ebe60f
    CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=5 \
      go build -trimpath -ldflags "-s -w" -o go2rtc-armv5 .

Put the result at `live/go2rtc-armv5`, or point at it with
`export GO2RTC_BIN=/path/to/go2rtc`. `live.py --install` looks for `$GO2RTC_BIN`
first, then `live/go2rtc-armv5`, and afterwards reports the ELF header and the
md5 of whatever ended up on the card, so a half-uploaded binary is caught.

One more trap: a Go binary of this size maps far more *address space* than this
kernel promises by default, so it can be refused at `execve` before executing a
single instruction — which looks like an out-of-memory kill and is not one. The
supervisor handles it: `vm.overcommit_memory=1` for as long as the relay runs
(the previous value is saved and restored), `GOMEMLIMIT=20MiB GOGC=100` for the
Go heap, `oom_score_adj=1000` on go2rtc so the kernel kills it first, and
`oom_score_adj=-500` on the vendor daemons so it never takes the camera down.
Starting go2rtc by hand without those is not a supported path; use
`./live.py --start`.

## 6. Install the relay

    cd tools
    ./live.py --install      # live.conf, run_live.sh, stop_live.sh, go2rtc -> /mnt/camweb/live
    ./live.py --ui           # index.html, app.js, cgi-bin/api -> /mnt/camweb/www
    ./live.py --status

Nothing starts at boot. The CGI starts the supervisor when the page asks, the
supervisor starts go2rtc, and the whole thing tears itself down `LIVE_IDLE_S`
(default 300 s) after the last viewer heartbeat — verified: a viewer left
standing, then closed, produced `supervisor exit: idle 302s` followed by
`go2rtc stopped`, ~9 MB of RAM back and both relay ports closed.

Knobs, in `live/live.conf`:

| key | default | what it does |
|---|---|---|
| `LIVE_SRC` | `rtsp://127.0.0.1:554/vs1` | source stream — `/vs1` is 640x480; `/vs0` (1280x720) overloads this board |
| `LIVE_API_PORT` | 1984 | go2rtc's own UI/REST/HLS, the port the browser reads |
| `LIVE_RTSP_PORT` | 8554 | RTSP output for other clients (VLC, Home Assistant) |
| `LIVE_IDLE_S` | 300 | seconds after the last heartbeat before everything stops |
| `LIVE_GOMEMLIMIT` / `LIVE_GOGC` | 20MiB / 100 | Go heap budget; lower GCs harder, higher risks the vendor stack |
| `LIVE_RESTART_MAX` | 3 | restart attempts before the supervisor gives up |

`./live.py --off` writes `/mnt/camweb/live/disable` so the page cannot start the
relay at all (survives reboot); `--on` removes it.

## 7. Prove it works

    ./live.py --test         # 21 checks, and it leaves the camera idle afterwards
    ./live.py --idle-test    # the idle reaper stops the relay by itself
    ./live.py --race-test    # 4 concurrent live_start -> exactly one supervisor, page still answers
    ./live.py --logs         # /tmp/live.log off the camera

`--test` is the one that matters: it starts the relay, pulls real segments off
`:1984`, checks there is genuine H.264 in them, and then asserts the camera is
clean again — no go2rtc process, ports 1984/8554 closed, `overcommit_memory`
back to its previous value, vendor `oom_score_adj` back to 0. On this board the
sequence ran 21/21 after a fresh boot.

Then in a browser: open the page, press **Start live preview**. The relay is up
in a few seconds and video appears. If it starts and then goes black, see
section 9 — that is almost always the HLS redirect, not a codec problem.

## 8. Using it day to day

| port | what answers there |
|---|---|
| `:8080` | the camweb UI (after `--promote`; the stock page after `--revert`) |
| `:8090` | the camweb UI if you never promoted it |
| `:1984` | go2rtc's own UI/REST — only while a preview is running |
| `:8554` | RTSP output, e.g. `rtsp://$CAM_HOST:8554/cam` for VLC or Home Assistant |
| `:554` | the vendor's own RTSP: `/vs0` 1280x720, `/vs1` 640x480 |

Useful one-liners:

```sh
./live.py --status          # running?, supervisor pid, relay pid, RSS, free memory
./live.py --stop            # free the RAM for everyone at once
./live.py --off / --on      # persistent block/unblock of the relay on the camera
./live.py --reboot          # reboot, wait for the UI, then run --boot-check
./deploy.py --status        # listeners + what the API reports
./diag.sh headers           # what the CGI returns, as the browser sees it
```

`--boot-check` is the one that answers "did any of this survive a reboot": no
go2rtc process, no relay ports, the UI up, `overcommit_memory` back at its stock
value. The relay is only ever started by a viewer.

## 9. The browser end-to-end check (optional, needs Chrome)

`tools/live.py` proves the camera side. It cannot prove the part only a browser
can fail: CORS between `:8080` and `:1984`, MediaSource accepting the codec, and
whether a decoded frame actually lands on screen. For that:

```sh
google-chrome-stable --headless=new --remote-debugging-port=9333 \
  --user-data-dir=/tmp/camweb-chrome about:blank &
cd tools && ./browser-check.mjs
```

It presses the page's own **Start live preview** button, waits for a real frame
size on the `<video>` element, requires `currentTime` to keep advancing, saves a
screenshot to `logs/live-preview.png`, presses stop and checks the relay port
closed. Result on this camera: `640x480`, `currentTime 2.5 -> 11.1 s`, no page
errors. 7 of 8 assertions pass; the one that fails is the panel's status text
lagging because a backgrounded headless tab has its timers throttled — the port
closure is verified in the same run.

**The preview runs over WebRTC.** The page posts a recvonly offer to
`http://$CAM_HOST:1984/api/webrtc?src=cam` (go2rtc's own API - this build has no
`/api/whep`, and it answers **201**, not 200) and renders what arrives. Measured
on this camera: offer 0.14 s, answer 4.0 s (go2rtc waits for the encoder's next
key frame, and this encoder's GOP is about 8 s), first frame **8 s after the
button**, jitter buffer 235 ms, 0 packets lost, and `currentTime` advancing 6.00 s
per 6 s of wall clock - so it cannot drift. Probe it with `./webrtc-probe.mjs`.

Video only: the camera's audio is G.711 a-law (PCMA) and Chrome's WebRTC stack
cannot decode it. The HLS segment path stays in the page as the fallback for a
browser or relay build without WebRTC, and it is the one that can lag - 0.5 s
segments fetched one at a time, each stall adding delay nothing closes again.

**Why the HLS fallback reads a playlist instead of an MP4:** go2rtc 301-redirects
WebKit-family user-agents away from `/api/stream.mp4`. `fetch()` follows that
silently, so a browser that "asks for MP4" is quietly handed an
`#EXTM3U` playlist, and MediaSource rejects it. Reproduce it with
`tools/fetch-probe.mjs`, which runs `fetch()` inside the page with a different
user-agent per attempt. The page therefore reads the playlist, appends
`init.mp4`, then the `segment.m4s` pieces, and follows the live edge.

**Why there is no JPEG snapshot:** this unit has nothing listening on the stock
`:3000` JPEG endpoints the vendor page references, and go2rtc's snapshot feature
needs `ffmpeg`, which is not on the camera. H.264 through the relay, decoded on
the viewer, is the only working preview on this board.

## 10. Troubleshooting

| symptom | cause / fix |
|---|---|
| `Illegal instruction` running go2rtc | not an ARMv5 build — build with `GOARM=5 CGO_ENABLED=0`, see section 5 |
| go2rtc dies instantly, no log line | address space refused at `execve`; start it through the supervisor (`--start`), which sets `overcommit_memory=1` |
| relay starts, panel says live, no picture | usually joining a long-running session at a segment the decoder cannot start from; the page re-joins at the head of the window then falls back to the live edge — reload the page |
| panel says "blocked on this camera" | `/mnt/camweb/live/disable` exists — `./live.py --on` |
| `live_start` returns but nothing happens | `./live.py --logs`; also check free memory — the supervisor refuses when available memory is below `LIVE_MIN_AVAIL_KB` |
| picture lags further and further behind | you are on the HLS fallback - the page says "webrtc unavailable (...) - using HLS segments"; check UDP reachability to :8554 and that the deployed page is current |
| UI slow or timeouts | one core; while a preview is running the page polls less often, and CGIs queue. Close other viewers |
| telnet login fails / hangs | busybox `telnetd` here wedges if a previous session is half-open — reboot the camera |
| FTP upload fails midway | the card is doing video at the same time; retry, or copy the binary on-card (`live.py --install` reuses a copy already on the card if the md5 matches) |
| after reboot the page is gone | `./deploy.py --status`; if `config.sh` was reset by a firmware update, `--promote` again — the backups are still in `/mnt/camweb/backup` |


## 11. Undo everything

```sh
cd tools
./live.py --stop                 # stop the relay now
./deploy.py --revert             # stock Factory page back on :8080
```

Then, on the camera (telnet), remove what this install added:

```sh
rm -rf /mnt/camweb/live          # relay: config, supervisor, go2rtc
rm -f  /mnt/camweb/run_web.sh    # the httpd restart loop
cat /mnt/camweb/backup/config.sh.orig > /mnt/Factory/config.sh   # exact original boot script
rm -rf /mnt/camweb/www /mnt/camweb/token /mnt/camweb/webui.conf /mnt/camweb/presets
reboot
```

`/mnt/camweb/backup/` holds `config.sh.orig` and `stock_www/`; nothing else in
`/mnt/Factory` was ever modified, and the relay never wrote to it at all. To
remove only the relay and keep the new UI: `./live.py --stop` and
`rm -rf /mnt/camweb/live` is enough — the page then reports the relay as missing
and everything else keeps working.

## 12. What this writes on the camera

| path | written by | removable |
|---|---|---|
| `/mnt/camweb/www/` | `deploy.py` (the UI) | yes |
| `/mnt/camweb/cgi-bin/api` | `deploy.py` | yes |
| `/mnt/camweb/token`, `webui.conf`, `presets/` | `deploy.py` | yes |
| `/mnt/camweb/run_web.sh` | `deploy.py --promote` | yes |
| `/mnt/camweb/backup/` | `deploy.py --promote` (stock copies) | only after reverting |
| `/mnt/Factory/config.sh` | `deploy.py --promote` — one line replaced, original kept in `backup/config.sh.orig` | restore the original |
| `/mnt/camweb/live/` | `live.py --install` | yes |
| `/tmp/live.log`, `/tmp/live.json`, `/tmp/camweb_live.lock` | the running supervisor | it cleans these up itself |

Never touched: `/mnt/Factory/custom/web_interface/www` (stock docroot, left in
place but no longer served once promoted), the vendor daemons, the RTSP server,
the ISP/sensor configuration, the kernel modules.

## 13. Limits worth knowing before you deploy this

* **One viewer.** The relay is sized for a single browser session on a 36 MB
  board; a second viewer roughly doubles CPU and is where the box starts to lose
  frames.
* **640x480 only, deliberately.** `/vs0` at 1280x720 was tried and measurably
  overloaded the camera while relaying.
* **Cold start is seconds, not milliseconds**, and slower when the camera is
  already busy: page assets, the CGI and the relay all compete for one core.
* **A forgotten browser tab keeps the relay running.** The 300 s idle timer only
  starts once the viewer stops beating; `--stop` is the manual escape hatch.
* **No JPEG snapshot** on this unit (section 9).
* **The camera's clock is wrong after boot** (1970 until the vendor daemon
  syncs), so log timestamps are relative, not wall-clock.


