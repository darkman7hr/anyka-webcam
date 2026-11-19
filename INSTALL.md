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

Sections 14 and 15 are two more independent pieces, also optional and also
removable on their own: **14** replaces the vendor's capture/encode/RTSP binary
with `akrtsp` (which is what makes the day/night control and the next item
possible), and **15** puts an NTP clock on the camera and burns the date into the
picture, so the NVR's recordings carry a time that means something.

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
relay at all (survives reboot); `--on` removes it. The page's **Relay** switch
writes and clears that same file through `act=live_off` / `act=live_on`, and both
of them commit the flag, read it back, print the reply, and only then run the
teardown in the background — on this box the web server serves one connection at a
time, so a CGI that spends three seconds tearing things down delays every other
request the page has outstanding, and that is how the switch broke in the first
place.

## 7. Prove it works

    ./live.py --test         # 21 checks, and it leaves the camera idle afterwards
    ./live.py --idle-test    # the idle reaper stops the relay by itself
    ./live.py --race-test    # 4 concurrent live_start -> exactly one supervisor, page still answers
    ./live.py --toggle-test  # the page's Relay switch, both ways, at the API level
    ./live.py --logs         # /tmp/live.log off the camera

`--test` is the one that matters: it starts the relay, pulls real segments off
`:1984`, checks there is genuine H.264 in them, and then asserts the camera is
clean again — no go2rtc process, ports 1984/8554 closed, `overcommit_memory`
back to its previous value, vendor `oom_score_adj` back to 0. On this board the
sequence ran 21/21 after a fresh boot.

`--toggle-test` checks the page's **Relay** switch at the API level: it blocks and
unblocks repeatedly and asserts the contract the switch rests on — the flag appears
and clears every time in both directions, `live_off` answers *while the relay is
still running* and the backgrounded teardown stops it a moment later, a stop does
not kill a relay started after it, and the new supervisor still owns its lock and
pidfiles. 18/18, and it leaves the camera as it found it. The browser half is
`./switch-check.mjs` (needs the optional Chrome/Node from section 1): it clicks the
real button and only passes when the button and the camera's own card agree in both
directions, including a status poll fired deliberately during a click and a double
click that must change the state exactly once. 12/12.

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
| `:554` | the stream server: `akrtsp` (section 14) or, after `run_takeover.sh --restore`, the vendor binary. Both serve `/vs0` 1280x720 and `/vs1` 640x480 |

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
| panel says "blocked on this camera" | `/mnt/camweb/live/disable` exists — press the page's **Relay: blocked** switch (or `./live.py --on`) |
| Relay switch flips and never comes back | the CGI answers before the teardown now, so this should not happen; `./live.py --toggle-test` and `./switch-check.mjs` pin it down, and `./live.py --logs` shows the teardown |
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
| `/mnt/camrtsp/akrtsp`, `run_akrtsp.sh`, `run_takeover.sh` | section 14 (stream server) | yes: uncomment the vendor line in `config.sh` |
| `/mnt/camweb/daynight.conf` | the UI (`act=nightmode`) and you | it is just the wanted mode; delete it and `--force-night` decides |
| `/mnt/camweb/akrtsp.args` | `osd.py` (the option line the supervisor starts `akrtsp` with) | it is just options; `--osd-time` in it turns the overlay on |
| `/mnt/camweb/time/` (`run_time.sh`, `time.conf`) | `time.py --install` | `time.py --restore` |
| `/mnt/camrtsp/lib/libmpi_osd.so` | `osd.py --install` (the vendor OSD library from the rootfs dump, see `SDK_LIB`) | `osd.py --off`, then it is unused |
| `/tmp/camweb_time.status`, `/tmp/camweb_time_last`, `/tmp/camtime.log` | the clock supervisor | it cleans these up itself |
| a clock line in `/mnt/Factory/config.sh` | `time.py --install` (backed up and syntax-checked; it stops parsing if broken) | `time.py --restore` |
| a second line in `/mnt/Factory/config.sh` | section 14, by hand | one-character uncomment of `# /mnt/Factory/custom/rtsp &` |

Never touched: `/mnt/Factory/custom/web_interface/www` (stock docroot, left in
place but no longer served once promoted), the vendor binaries themselves
(`/mnt/Factory/custom/rtsp` is still there and still runnable), the vendor
daemons, the kernel modules. What **is** touched on purpose, once section 14 is
followed: the ISP day/night mode (`ak_vi_switch_mode`), the IR illuminator and
IR-cut gpio, and one line of `config.sh` — all of them described in README.md and
all of them reversible.

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
* **The camera has no rtc and no battery.** After boot it sits at the epoch plus
  uptime, and the vendor daemon that used to learn the time from its cloud bind is
  not running here, so log timestamps are relative, not wall-clock — unless section
  15 is followed. Until that service says `synced`, `akrtsp --osd-time` prints
  `NO TIME` into the picture rather than a date it cannot vouch for.
* **The burned-in date is stamped twice, 16 px apart.** Not a bug in this code:
  `libmpi_osd` on this generation stamps the string once per 16-row block of its
  rect and refuses any rect shorter than twice the font size. Section 15.3.

## 14. The stream server (`akrtsp`) and day/night

Sections 1-13 work without this: the camera streams on whatever owns `:554`.
Install this part if you want the replacement server (measured better than the
vendor binary — README, "Milestone 2") or the day/night control that goes with it.

### 14.1 Build

Needs the pinned armv5 toolchain and the SDK dump (see README "Cross-compiling";
`ANYKA_ROOT` and `TOOLCHAIN` default to the paths in `src/Makefile`).

```sh
make -C src akrtsp          # compiled, not stripped
make -C src check           # strips it: src/akrtsp.stripped  <- this is the file to ship
```

`make akrtsp` alone does **not** refresh `akrtsp.stripped`, and that stripped file
is what the upload sends — build and check in one step, and `md5sum` both ends of
every upload. The Makefile links against the SDK libraries from `rootfs_v1basic`
because those are md5-identical to the camera's own `/mnt/Factory/custom/lib`;
linking a neighbouring SDK build is how an ABI difference becomes a board you
cannot debug.

### 14.2 Install (a running binary cannot be overwritten in place)

```sh
python3 - <<'EOF'
import ftplib
f = ftplib.FTP('192.168.10.46', timeout=20)
f.login('root', 'anyka')
f.storbinary('STOR /tmp/akrtsp.new', open('src/akrtsp.stripped', 'rb'))
f.quit()
EOF
./tools/cam.py 'md5sum /tmp/akrtsp.new'          # must equal the local md5
./tools/cam.py 'cp /tmp/akrtsp.new /mnt/camrtsp/akrtsp.new && chmod +x /mnt/camrtsp/akrtsp.new && mv -f /mnt/camrtsp/akrtsp.new /mnt/camrtsp/akrtsp && md5sum /mnt/camrtsp/akrtsp'
./tools/cam.py 'for p in $(ls /proc | grep -E "^[0-9]+$"); do read -r c < /proc/$p/comm 2>/dev/null; [ "$c" = akrtsp ] && kill $p; done'
```

`ftpd` answers `553 Error` if you try to write over a running executable — that is
ETXTBSY wearing a different hat. Upload as `.new`, `mv -f` it into place (a rename
needs no write permission on the running inode), and let the supervisor restart it;
the stream is down for the ~10 s that takes, and the NVR reconnects by itself.

### 14.3 Boot line — this is where the arguments really come from

In `/mnt/Factory/config.sh`, at the point where the vendor server used to start
(after `sleep 30`, so the ordering against `ptz_daemon` matches the vendor's):

```sh
BIN=/mnt/camrtsp/akrtsp AK_ARGS="--mode-file /mnt/camweb/daynight.conf --force-night night --ir-follow" \
  /bin/sh /mnt/camrtsp/run_akrtsp.sh 10 3 >/tmp/akrtsp_boot.log 2>&1 &
# /mnt/Factory/custom/rtsp &
```

Check it with `sh -n /mnt/Factory/config.sh` **on the camera** before trusting it,
and keep the vendor line as a comment rather than deleting it: that comment is the
rollback. Two traps that cost time here: `config.sh` sets `AK_ARGS`, so an option
added only to `run_akrtsp.sh`'s default never reaches the camera; and `BIN` must
point where the binary actually is, or the supervisor does the sensible thing —
starts the vendor server and exits — quietly.

### 14.4 Day/night mode

```sh
./tools/cam.py 'printf "night\n" > /mnt/camweb/daynight.conf'     # or day, or auto
./tools/cam.py 'grep -E "^(mode|want|auto|auto_reason|auto_gain|auto_lamp|ircut)=" /tmp/akrtsp.status'
```

`auto` reads the ISP's auto-exposure gain, not the light sensor on this board
(which reads DAY with the room light on and off) — the rule, the measured numbers
and the probe behaviour are in README, "Auto day/night that works". The knobs are
server options, set them in the `config.sh` line above:

| option | default | meaning |
|---|---|---|
| `--auto-dark-gain` / `--auto-light-gain` | 9000 / 6500 | hysteresis thresholds on the AE analogue gain |
| `--auto-dark-secs` / `--auto-lit-secs` | 15 / 30 | how long a reading must hold before it counts |
| `--auto-probe-secs` / `--auto-settle-secs` | 16 / 10 | night -> day is decided with the camera's own IR lamp off, and the settling window is discarded |
| `--auto-dwell-secs` / `--auto-rearm-secs` | 45 / 300 | minimum time in a state; wait after a probe said "still dark" |
| `--no-auto-lamp` / `--no-auto-filter` | off | let auto drive only the ISP mode |
| `--ir-led-node` / `--ircut-node` / `--ptz-fifo` | `/sys/user-gpio/IR_LED`, `/sys/user-gpio/gpio-ircut_a`, `/tmp/ptz.daemon` | the three surfaces auto moves |

This install runs `auto`. To pin it to night instead, one command:
`./tools/cam.py 'echo night > /mnt/camweb/daynight.conf'`.

### 14.5 Prove that part works

```sh
sh tools/auto_check.sh                       # ~6 min, room light required: dark -> night,
                                             # light on -> probe -> day, off -> night
node tools/night-check.mjs                   # the panel in a real browser (start Chrome
                                             # with --remote-debugging-port=9333 first)
python3 tools/rtsp_probe.py -t 6 rtsp://$CAM_HOST:554/vs0
python3 tools/rgbshot.py
./tools/cam.py 'cat /proc/loadavg; grep -m1 ^pid= /tmp/akrtsp.status'
```

Expected on this board: 20 fps, one key frame per second, 0 sequence gaps, the
night frame R=G=B across the full 0..255 range (a purple frame means the ISP is in
day mode under the illuminator; a black one means the filter is in while the lamp
is on), and a load average of 1.6-2.9 with the stream and the page open.

## 15. The clock, and the date burned into the picture

The camera has no rtc, so nothing about the date works until section 15.1 is
done. The overlay is optional; the clock is what makes it worth having, and
`akrtsp --osd-time` deliberately draws `NO TIME` until that clock says `synced`.

### 15.1 Install the clock

```sh
python3 tools/time.py --install
python3 tools/time.py --status
python3 tools/time.py --test
python3 tools/time.py --sync                                  # ask for a sync now
python3 tools/time.py --tz 'CET-1CEST,M3.5.0,M10.5.0/3'
python3 tools/time.py --logs
```

`--install` puts `run_time.sh` and `time.conf` in `/mnt/camweb/time/`, adds one
boot line to `/mnt/Factory/config.sh` (backed up and syntax-checked, and put back
if it stops parsing) and starts it. Edit `time.conf` for your own NTP servers and
leave `min_valid` alone: it is what stops one bogus reply from setting the camera
to 2009. The zone has to be a POSIX rule, not a name — uClibc on this board has no
zoneinfo database, so a named zone silently means UTC.

`--test` is deliberately not "the clock reads a plausible number", because 1970
plus uptime is plausible on a box with no rtc. It compares the camera's epoch with
this workstation's, checks that the zone the overlay prints in is the one in
`time.conf`, and checks that the boot line is still in `config.sh` — which is the
only reason any of it survives a reboot. `--restore` takes the boot line out and
stops the service.

### 15.2 Turn the overlay on

```sh
python3 tools/osd.py --install       # akrtsp + run_akrtsp.sh + libmpi_osd.so
python3 tools/osd.py --on --pos br   # tl | tr | bl | br
python3 tools/osd.py --status
python3 tools/osd.py --shot          # measure the text in a real frame
python3 tools/osd.py --off
```

`--install` reads the vendor OSD library from `$SDK_LIB` on your PC (default
`/home/darkman/opt/anyka_v380ipcam_experiments/rootfs_v1basic/rootfs/usr/lib`;
set it if your rootfs dump lives elsewhere) and puts it at
`/mnt/camrtsp/lib/libmpi_osd.so`. It also refuses to install unless
`/usr/local/ak_font_16.bin` is readable on the camera, because this build of the
library has no builtin font — the header's `ak_osd_set_font_size` is not in it.

The options end up in `/mnt/camweb/akrtsp.args`, and the overlay is set up when
the server starts: there is no runtime switch, so every change here restarts
`akrtsp` and the supervisor brings it back in roughly 8 s. `osd.py` rewrites only
the flags it owns and carries the day/night ones through untouched, because two
tools rewriting one argument line is how a deployment loses its night mode
overnight.

| `osd.py` | `akrtsp` | default | meaning |
|---|---|---|---|
| `--pos` | `--osd-pos` | `br` | corner the rect is anchored to |
| `--size` | `--osd-size` | 16 | passed to `ak_osd_set_font_file`. On this board it only sets the minimum rect height (twice its value); it does **not** scale the glyphs |
| `--rect-h` | `--osd-rect-h` | 2×size | rect height: shorter than 2×size the library refuses it, shorter than 32 the display path truncates it |
| `--fmt` | `--osd-fmt` | `%Y-%m-%d %H:%M:%S` | the line, as strftime |
| `--xoff` / `--yoff` | `--osd-xoff` / `--osd-yoff` | 0 | offset inside the rect, clamped per channel |
| `--osd-sub 0` | `--osd-sub` | 1 | burn the main channel only |

### 15.3 What it produces, the flaw included

`osd.py --shot` on the build described here: main rect `400x32 at 872,680`, sub
rect `400x32 at 232,440`, 18–20 of 32 rows carrying stroke edges inside the rect
(13 on the sub stream) and **0** in a same-sized control rect that has no text in
it. Put the control at the top of the frame when measuring the sub stream: its
mirror position overlaps the text band, and then it agrees with the rect perfectly
while proving nothing.

**The date is stamped twice, 16 px apart.** That is the library, not this code:
`libmpi_osd` on this camera generation stamps the string once per 16-row block of
its rect, refuses a rect shorter than `2 × --osd-size`, and truncates anything
below two blocks with its own message (`Small buffer to disp osd, only disp 4
characters of total 19`). Measured and ruled out: `--size 8 --rect-h 16` (four
characters), `--size 32 --rect-h 64` (four copies), `--osd-sub 0` (second band
unchanged), clearing the canvas below the first line after drawing (no effect).
The library's real signatures, and why the canvas has to be cleared by stepping
across the rect, are in README, "The date in the picture". One clean line is not
available through this board's OSD; if the recording needs it, the text has to be
added after the encoder, on the NVR side.

### 15.4 When it is wrong

| symptom | first check |
|---|---|
| `osd_text=NO TIME` in `/tmp/akrtsp.status` | `python3 tools/time.py --status`; then `--sync` and `--logs` |
| time in the picture is hours off | `TZ_STR` in `/mnt/camweb/time/time.conf` — `run_takeover.sh` and `run_akrtsp.sh` export it, and the vendor environment sets `TZ=GMT-08:00`, which is exactly what broke once |
| date right, text nowhere in the picture | `grep 'osd:' /tmp/akrtsp.log` for the rects actually used, then `osd.py --shot`; confirm `/usr/local/ak_font_16.bin` is readable |
| line clipped to `2026-11-19 1` | the rect is narrower than the string's real advance, which is the font file's 16 px per character — shorten `--fmt` |
| handover never takes `:554` after adding the overlay | `LD_LIBRARY_PATH` in `run_takeover.sh` has to include `/mnt/camrtsp/lib`; without it `akrtsp` dies on a missing `libmpi_osd.so` and the loop only ever reports "nothing on :554" |
| overlay settings gone after a reboot | the boot line and `/mnt/camweb/akrtsp.args` are on the card, not in `/tmp`: check both, then `python3 tools/time.py --test` |


