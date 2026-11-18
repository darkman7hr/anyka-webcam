# Anyka AK3918 camera — research, replacement web UI, and what to do next

Camera: `192.168.10.46` · Anyka AK3918 · GC1084 sensor · Linux 3.4.35 armv5tejl ·
36 MB RAM (~12 MB free) · 14.6 GB microSD at `/mnt` · Wi-Fi 2.4 GHz. The network
name (SSID) and the MAC of this particular camera are deliberately **not** recorded
in this file — they identify your home, not the board. Read them on the camera when
you need them: `ifconfig wlan0`, `cat /etc/jffs2/wpa_supplicant.conf`. The same
rule applies to any password: nothing here stores one except the vendor's stock
factory pair, which is in `INSTALL.md` because it is what the SD-card hack needs.

**Want to install this on your own camera? Start at `INSTALL.md`** — it is the
complete path from an untouched SD-card hack to a working preview, including which
go2rtc build this board actually accepts. `NOTICE.md` covers third-party parts.
This README is the findings log: what was measured, what broke, what is known.

## 1. How these cameras are hacked

There is no exploitable network service needed to root the stock firmware: the
hack is **a boot hook on the SD card**. The vendor firmware mounts the card at
`/mnt` at start and, if `/mnt/Factory/config.sh` exists, executes it as root.
Drop that file on the card and you get arbitrary code at boot with no soldering
and no serial cable. Everything else — telnet, RTSP, PTZ, the web UI — is just
whatever that script chooses to start.

Two independent lineages implement this, and **your card carries both**:

| | Vendor "Factory/custom" build | Gerge `anyka_hack` |
|---|---|---|
| Entry | `/mnt/Factory/config.sh` | same hook → copies `gergehack.sh` to flash, execs it |
| Lives in | `/mnt/Factory/custom/` | `/mnt/anyka_hack/` + `/etc/jffs2/` |
| Web UI | `busybox httpd -p 8080` | `busybox httpd -p 80` |
| Video | own `rtsp` binary, `:554` | `libre_anyka_app`: RTSP `:554` **and** JPEG `:3000` |
| Extras | `onvif` binary (present, **never started**), `vpn.sh` | settings/events pages, motion recording, dropbear SSH |
| **Status here** | **running** | **installed but never launched** |

`/mnt/Factory/config.sh` on this camera:

```sh
#!/bin/sh
export LD_LIBRARY_PATH=/mnt/Factory/custom/lib
/usr/sbin/net_manage.sh &
telnetd &
ak_adec_demo 16000 2 aac /usr/share/audio_file/common/didi.aac
/mnt/Factory/custom/ptz_daemon &
/mnt/Factory/custom/busybox httpd -p 8080 -h /mnt/Factory/custom/web_interface/www &
sleep 30
/mnt/Factory/custom/rtsp &
/mnt/Factory/custom/vpn/vpn.sh &
```

Because *this* `config.sh` is the vendor's and not the Gerge one,
`/etc/jffs2/gergehack.sh` never runs. That explains the odd state of the box:
the Gerge tree is on the card, its CGI mirror even sits in `/etc/jffs2/www`,
and the settings file says `run_libre_anyka=1` — yet nothing starts it. Which is
why **port 3000 is dead**: the JPEG/preview service belongs to the stack that
never launched. RTSP on 554 is fine, served by `/mnt/Factory/custom/rtsp`
(`rtsp://192.168.10.46:554/vs0`, H264 + G.711 a-law audio, verified by DESCRIBE).

## 2. Verified PTZ contract

The daemon (`/mnt/Factory/custom/ptz_daemon`, kuhnchris
`IOT-ANYKA-PTZdaemon` lineage) reads newline commands from the FIFO
`/tmp/ptz.daemon` and **keeps the authoritative position in
`/etc/jffs2/ptz.ini`** as `"<pan> <tilt>"`. Run against the live camera, each
step confirmed by reading the position back:

| Action | Result |
|---|---|
| `cat /etc/jffs2/ptz.ini` | `260 120` — current position, readable |
| `t2p 200 100` | `ptz.ini` → `200 100`. Absolute positioning works |
| `left` | `200→190`, exactly 10° — relative steps are fixed size |
| `init` | position untouched; safe to send |
| `t2p 260 120` | returned home |

Command set from the binary: `init` `setar` `setdeg` `t2p` `up` `down` `left`
`right` `irinit` `irsetircut` `irsetcheckmode` `irsettres` `irgettres`
`irgetinputlevel`. It requires the stock `cmd_serverd` on `127.0.0.1:8782`
(running). Note the homing spelling differs by build — the Gerge docs insist on
`init_ptz` for *their* daemon, while this one's usage string says `init`.

Two consequences shaped the UI:

* **Recorded positions are easy and reliable.** Save `pan tilt`, write `t2p` to
  recall. That is the whole feature and it works.
* **Relative moves are always 10°,** so the UI computes its own steps from the
  reported position (`current + step → t2p`). That enables 2°/5°/30° steps and
  drag-to-aim, and means the gimbal cannot drift out of sync the way repeated
  fixed nudges do.

The daemon's self-calibration reports ~369° pan and ~189° tilt of travel.
Defaults clamp narrower (pan 0–360, tilt 10–170) in `/mnt/camweb/webui.conf`;
tighten those if you find the ends of travel.

## 3. Bugs in the running 8080 interface

1. **The arrow pad is wired wrong.** Top-left renders ↗ but sends
   `right`+`down`; middle-left (←) sends `ptzr`, middle-right (→) sends `ptzl`.
   Pan is inverted and the diagonals are mirrored.
2. **No authentication of any kind.** `GET /cgi-bin/webui?command=ledon` writes
   `/sys/user-gpio/WHITE_LED` unauthenticated — same file that drives motors.
3. **The page reloads entirely on every button press** (`window.location.href`),
   so PTZ feels sluggish and there is zero position feedback.
4. No absolute positioning and no presets; `t2p` was previously telnet-only.


## 4. What is deployed now

The redesigned UI now **owns port 8080** — the port this camera's interface has
always used. The stock page was backed up first, on the card and in `pull/`, and
nothing was deleted:

* **new UI → <http://192.168.10.46:8080/>** (docroot `/mnt/camweb/www`, kept
  alive by `/mnt/camweb/run_web.sh`, which `config.sh` starts at boot)
* stock Factory page → **disabled**, preserved in `/mnt/camweb/backup/stock_www`
  and `pull/mnt__Factory__custom__web_interface__www__*`; `./deploy.py --revert`
  puts it back on 8080 in one command
* `:8090` is retired — the second server was stopped when ours took 8080 over
* token: generated per install, in `/mnt/camweb/token` and cached in
  `tools/.token` (printed by `./deploy.py`). The browser asks once and remembers it.

The old page turned out to be a factory test screen: a redirect to a 2.9 KB
shell CGI with LED on/off, eight PTZ arrows and an audio-clip player. Everything
it could do, the new UI does too (the white LED is on it), plus presets, auth
and live position — only the "play an .aac through the speaker" test button is
missing, and `--revert` brings that back if it is ever wanted.

Features: 8-way pad with press-and-hold repeat · step sizes 2/5/10/30° ·
**12 named recorded positions** (Go / Save / Clear, stored on the camera so they
survive reboot and are shared between browsers) · patrol mode that cycles the
saved positions · **drag anywhere on the image to aim** · absolute pan/tilt
entry · live position in the header · white LED · **image mode day / night / auto
and the IR illuminator and IR-cut filter as separate switches, each showing what
the camera reports rather than what was clicked** (see "Day/night from the
browser") · reboot · RTSP URLs click to copy.

Naming a position opens a text field **inside its own card**, delete asks twice
(the `×` turns into "Sure?"), and the token is typed into a panel rather than a
browser dialog: nothing in the page calls `prompt()` or `confirm()` any more,
and a failed request now prints a message instead of doing nothing at all.
Verified round trip: `goto 240/115` → save slot 4 → `goto 200/100` → `preset_goto
slot 4` → camera reports `240/115`.

The camera held **Front Door = 260/120** in slot 1 as an undo reference; it now
holds your own positions, so if you want that original back, move to 260/120 and
store it in a free slot before overwriting anything.
## Live preview relay (on-demand go2rtc on the card)

The page now carries real video. It is not a JPEG relay and it is not off-box:
the camera runs `go2rtc` **on the card, only while somebody is watching**, and
the viewer's browser does the H.264 decoding.

```
browser :8080 page  --act=live_start-->  CGI  -->  run_live.sh (supervisor)
                                                    |
              vendor rtsp :554 /vs1  --ingest-->  go2rtc :1984 (HLS/fMP4, CORS)
                                                    |
              browser  <-- fetch + MediaSource -----  (decodes on the viewer)
```

Nothing was written to `/mnt/Factory` and the vendor boot script is untouched:
**the relay never starts at boot.** The CGI starts it when the page asks, and it
exits by itself `LIVE_IDLE_S` (300 s) after the last heartbeat, so with nobody
watching the camera runs exactly the processes it always ran.

```sh
cd tools
./live.py --install      # live.conf + run_live.sh + stop_live.sh + go2rtc -> /mnt/camweb/live
./live.py --ui           # upload index.html / app.js / cgi-bin/api
./live.py --start        # the same path the page takes (act=live_start)
./live.py --status       # pid, RSS, available memory, state
./live.py --stop         # free the RAM for everyone at once
./live.py --off / --on   # persistent block/unblock on the card (/mnt/camweb/live/disable)
./live.py --test         # 21 checks: media really flows, and the camera is back clean
./live.py --idle-test    # prove the idle reaper shuts it down alone
./live.py --reboot       # reboot, wait for the UI, then --boot-check
./live.py --logs         # supervisor log (/tmp/live.log)
```

### Preview transport: WebRTC first, HLS as the fallback

The preview used to be HLS segments only, and it drifted: users reported the
picture being a minute or two behind reality. The cause is structural, not a
bug in one place:

* go2rtc's HLS window is per-session (`MEDIA-SEQUENCE: 0`, fresh `id=` per
  playlist request), so it is not a long retained window - fetching it is fast:
  **20 segments (10 s of video) in 3.6 s** measured from a quiet PC.
* but each session only carries 0.5 s chunks and this encoder's **GOP is ~8 s**
  (visible as the size spikes: segments 0, 1 and 16 were 24 kB, 17 kB and 25 kB,
  the rest 0.6-1.3 kB). A viewer can only start at a keyframe, and every stall
  in the chain - CGI poll, playlist, init segment, one segment at a time,
  MediaSource `updateend` - adds another 0.5 s that **nothing closes again**. On
  a box that is encoding, serving the page and answering CGIs on one core, that
  gap grows for as long as you watch.

WebRTC removes the queue. Measured against this camera with
`tools/webrtc-probe.mjs` (go2rtc 1.9.15, its native API - this build has no
`/api/whep`, it answers 201 not 200):

```
offer gathered 0.14 s  ->  answer 4.0 s (go2rtc waits for the next key frame,
                           which is the 8 s GOP)  ->  first frame 8.0 s
640x480, jitter buffer 235 ms, 103 frames received / 97 decoded / 0 dropped,
0 packets lost, 0 NACKs, and currentTime advanced 6.00 s in 6 s of wall clock.
```

In the real page (`browser-check.mjs`, v22): chip reads
**"live (WebRTC) - picture in 7.0 s"**, 8/8 checks pass, and go2rtc's RSS while
viewing was **7.1 MB** against ~12 MB for the HLS session. Video only: the
camera's audio is G.711 a-law and Chrome's WebRTC stack cannot decode PCMA.
HLS stays as the fallback for a browser or relay build without WebRTC.

### Which go2rtc this board actually accepts

The binary used here is `live/go2rtc-armv5`: go2rtc **v1.9.15** (commit 7c05066),
20 316 320 bytes, md5 `aed80b8384e4fc3d9496864a125c8e8f`, built with
`CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=5`. That is not a preference — the
**official upstream `go2rtc_linux_arm` asset for the same tag was uploaded to this
camera and answered `Illegal instruction`**. This board is armv5tejl soft-float,
and upstream's `linux/arm` build is not ARMv5-safe. `--install` now looks at
`$GO2RTC_BIN`, then `live/go2rtc-armv5`, then the SDK paths, and reports the ELF
header and md5 of whatever ended up on the card.

Measured on this camera, `./live.py --test` after a fresh boot (21/21 passed):

| | before | relaying | after stop |
|---|---|---|---|
| `go2rtc` processes | 0 | 1 (RSS 7.2 MB) | 0 |
| listening ports | 554 8080 21 23 8782 | + **1984 8554** | 554 8080 21 23 8782 |
| `overcommit_memory` | 0 | **1** | 0 |
| vendor `rtsp` `oom_score_adj` | 0 | **-500** | 0 |
| `go2rtc` `oom_score_adj` | – | **1000** | – |
| MemFree / available | 2.9 / 22.6 MB | 3.2 / 15.8 MB | 8.4 / 22.3 MB |

Media proof from the same run: 606 KB of fragmented MP4 in 12 s (~400 kbit/s),
boxes `ftyp` + `moov` + 49 × (`moof`,`mdat`), and the relay's own HLS playlist
advertises `CODECS="avc1.4DE028"` — the same H.264 Main/L4.0 the vendor server
negotiates, repackaged rather than transcoded.

Why the guards are there (all of them were measured, see
`../anyka-sdk/docs/go2rtc.md`):

* **`overcommit_memory=1` for the lifetime of the relay.** go2rtc maps ~53 MB of
  address space at exec; with the vendor default the kernel refuses the exec
  before a single instruction runs, even though it only ever resident-maps
  7 MB. This is the one reason go2rtc "does not work" on this box, and it is an
  address-space policy, not a memory shortage. The saved value is restored on
  every exit path, including SIGTERM and the crash path.
* **`GOMEMLIMIT=12MiB GOGC=60`** keeps the Go heap inside a budget the vendor
  stack can live beside.
* **`oom_score_adj`**: go2rtc 1000 (first choice if memory ever does run out),
  vendor `rtsp` -500 while the relay is up. The camera must keep streaming even
  if the preview dies.
* **Available-memory guard (6 MB) and start margin (12 MB)**, using
  MemAvailable-or-proxy, never raw `MemFree`: this board keeps ~19 MB of its
  36 MB as reclaimable page cache and `MemFree` of 2-3 MB is *normal*, so a
  MemFree guard would have the relay kill itself on a healthy camera.
* **Restart cap (5 per session).** go2rtc's MP4 consumer panics reproducibly
  when a consumer disconnects (`pkg/mp4/consumer.go:82` →
  `pkg/core/writebuffer.go:37`, nil `http.ResponseWriter`). The supervisor
  restarts it and the page's reconnect loop re-attaches, so a viewer sees a
  hitch; the cap stops a crash loop from outlasting the camera.

To take it back off: `./live.py --off` blocks it at once; `rm -rf
/mnt/camweb/live` removes it permanently. Nothing else has to be undone, because
nothing was added to the boot path.



Payload is 36 KB total (12.4 KB HTML + 16.1 KB JS + 8.6 KB CGI), no framework,
no CDN, no external requests — deliberate on a box with ~10 MB free RAM that
idles near load 2.

```sh
cd tools
./deploy.py            # upload + make sure ours is running, idempotent
./deploy.py --verify   # 13 checks: endpoints, injection, and JSON parsing
./deploy.py --status   # listener table
./deploy.py --promote  # back up the stock page, take over :8080, install runner
./deploy.py --revert   # restore config.sh, stock page back on :8080, ours to :8090
./deploy.py --preview=http://<pc-ip>:8124/preview.jpg
./deploy.py --stop     # stops ONLY ours (and the runner that would restart it)
./live.py --install    # on-demand preview relay: go2rtc + supervisor onto the card
./live.py --ui         # upload the UI (index.html, app.js, cgi-bin/api)
./live.py --start/--stop/--status   # drive the relay through the CGI
./live.py --test       # 21 checks: media flows, then the camera is back clean
./live.py --idle-test  # prove the relay shuts itself down when nobody watches
./live.py --reboot     # reboot, wait for the page, then check the boot state
./relay.py             # optional: RTSP -> JPEG on this PC, port 8124
./cam.py 'cat /proc/loadavg'

```

`tools/.port` records which port ours is on, so every command knows where to
look after a promote or a revert. `CAM_HTTP_PORT=…` overrides it.

### Promotion to 8080, and why a reboot no longer matters

The camera rebooted mid-session (uptime went to ~19 min) and `:8090` simply
vanished — nothing of ours started at boot, which is the failure mode that made
the page unreachable. Promotion fixes that permanently:

```sh
/mnt/Factory/config.sh          # stock httpd line replaced, original in
                                # /mnt/camweb/backup/config.sh.orig
/bin/sh /mnt/camweb/run_web.sh &
```

`run_web.sh` runs `httpd -f` in the foreground inside a `while :` loop, so if the
server ever dies the page comes back by itself. `config.sh` was edited over FTP
byte-for-byte, then syntax-checked with `sh -n` on the box; if it had failed to
parse it would have been restored immediately. `telnetd &` sits on line 6,
*before* the httpd line, so this change can never cost access to the shell.

Verified, not assumed:

* killed the httpd PID by hand → runner respawned it 3 s later (`5219` → `6020`)
* rebooted the camera → the UI was serving on 8080 by itself, log line
  `8s starting httpd on 8080`, with no `deploy.py` run at all
* after that reboot: 554 RTSP up (the vendor's own `config.sh` starts it after
  `sleep 30`), presets intact, position `250 110` restored, load back to 2.7,
  `--verify` 13/13

### Starting the relay: non-blocking, and exactly one of each

`live_start` answers immediately (`ok:1, started:1`) and does **not** wait for the
relay to bind. On this box that mattered: the first version polled for the port
inside the CGI, held the request for 15–30 s, and four concurrent starts (page +
second tab + retry timers) made the whole UI unresponsive even though nothing on
the camera had failed. Callers poll `live_status` instead — the page's `lvWaitUp()`
asks every 1.5 s, `tools/live.py` every 3 s.

Single instance is enforced in both directions, and both halves were needed:

* `run_live.sh` takes `/tmp/camweb_live.lock` with `mkdir` (atomic) and writes its
  PID into `lock/pid`. A newcomer refuses if that PID is alive.
* every supervisor re-reads `lock/pid` each beat and, if it no longer owns it,
  logs `superseded by pid N` and exits **without touching go2rtc** — because the
  new owner is responsible for the relay now.

The second half is what was missing. Refusing to start only protects the moment
of starting; a log check found two supervisors alive at once (`restarts=0` and
`restarts=2`, each with its own go2rtc), because the first had been reclaimed
while wedged in a wait loop. Reclaiming on a *stale heartbeat* alone turned out to
be wrong on a loaded box, so only a dead or zombie holder loses the lock, and the
ownership check makes the loser leave. Verified on the device: overwrite
`lock/pid` with a foreign PID and the running supervisor logs
`28973s superseded by pid 99999; leaving without touching the new relay` and is
gone within one 5 s beat, while the relay keeps serving.

Two PID-related lies this code now handles:

* a zombie keeps its `/proc/<pid>` directory, so "the directory exists" is not
  proof of life — check state `Z`;
* `busybox ash` leaves `/proc/<pid>/cmdline` **empty**, so "is this our script?"
  via cmdline answers *no* for a perfectly healthy supervisor. Identity is
  proven by owning the lock, not by the command line.

`/proc` walks are expensive here (one fork per process, ~0.5 s each under load),
so the orphan-go2rtc reaper only runs when a relay port is actually occupied; run
unconditionally it stalled the supervisor for close to a minute during a start
race. `tools/live.py` counts supervisors through the pidfile + heartbeat for the
same reason.

Memory: `LIVE_GOMEMLIMIT` is now **20MiB** (was 12MiB) with `GOGC=100`. A Go soft
limit below the real working set does not stop the process, it turns the collector
into a CPU hog: measured RSS with one viewer pulling 720p was ~13 MB, so 12MiB
sat exactly on the working set, the GC ran constantly on the single core, and the
relay kept dropping the browser's stream. The preview source is also now
**`/vs1` (640x480)** rather than `/vs0` (1280x720) — same reason, measured.

### The browser preview: what works, and what go2rtc does to Chrome

Proven at the byte level: `GET /api/stream.mp4?src=cam` from the camera's own
relay returns a valid **fragmented MP4** — `ftyp, moov, moof, mdat, moof, mdat…`
with `Content-Type: video/mp4; codecs="avc1.4DE028"`, 175–245 kB per 6 s.

The panel does **not** use that URL, and `tools/fetch-probe.mjs` is what found
out why: it runs `fetch()` inside the page instead of trusting curl, and the page
never receives MP4 at all. go2rtc redirects WebKit-family user-agents to HLS:

| request `User-Agent` | `/api/stream.mp4?src=cam` |
|---|---|
| (none, curl) | `200 video/mp4` |
| `Firefox/130.0` | `200 video/mp4` |
| `Chrome/154… Safari/537.36` | `301 → /api/stream.m3u8?src=cam&mp4` |
| `HeadlessChrome/154… Safari/537.36` | `301 → same` |
| `Version/17.0 Safari/605.1.15` | `301 → same` |

`fetch()` follows that redirect silently, so the page was feeding a **93-byte
`#EXTM3U` playlist** to a MediaSource SourceBuffer: `buffered.length` stayed 0 and
the pipe reported `relay closed the stream`. `User-Agent` cannot be overridden
from a page, so the page now speaks the HLS that go2rtc insists on handing to
browsers — which turns out to be the better format anyway:

```text
/api/stream.m3u8?src=cam&mp4   ->  hls/playlist.m3u8?id=XXXX
hls/playlist.m3u8?id=XXXX      ->  #EXT-X-MAP:URI="init.mp4?id=XXXX"
                                   segment.m4s?id=XXXX&n=0,1,2...  (0.5 s each)
```

Measured: `init.mp4` is 674 B (`ftyp`+`moov`), each `segment.m4s` is 10–70 kB of
`moof`+`mdat` — exactly what a SourceBuffer takes. The index is in the URL
(`&n=N`), so `lvPull()` appends the init segment once and then pulls segments by
index, re-reading the playlist only when its index drifts (`miss > 8`). It joins
at the **newest** published segment rather than the start of the relay's window,
and `lvFollowLive()` pulls the play head back when it falls more than 2.5 s
behind — a preview that quietly lags 20 s reads as a freeze.

Two behaviours found while getting there, both fixed in the page:

* **the stop button and the page fought each other.** "Stop relay now" freed the
  camera, but the page still had `lv.want` set, so its 8 s beat saw `running=0`
  and started the relay straight back up — a status check a minute later reported
  `running=1, state=stopped`. The button now leaves the preview first.
* **joining an HLS session at the newest segment can play nothing at all.**
  Segments start wherever the encoder pleases, so the newest one often begins
  mid-GOP and the decoder has nothing to start from: the element sat at 0:00 with
  readyState 1 and a fraction of a second buffered. The page now joins at the head
  of the relay's window, and falls back to the live edge on the second attempt
  (on a long-running relay the head has usually been dropped already, which shows
  up as 20-40 s of nothing before the first frame).

Known rough edges, honestly stated: the page's own status text can lag 30 s or
more after "Stop relay now" — in headless Chrome the tab is backgrounded and its
`setInterval` polling is throttled, so `tools/browser-check.mjs` still reports one
failing assertion ("page says the relay is idle") even though the relay port is
verified closed in the same run. Cold starts under load are slow: page assets can
take 60 s to arrive while the relay and a decoding browser are competing for the
one core, which is why the check waits for `app.js` before pressing anything.

Verified end to end in headless Chrome by `tools/browser-check.mjs`, which presses
the page's own button rather than calling an internal function:

```text
  page script loaded before pressing         PASS  ready
  video decoded in the page                  PASS  640x480
  playback advancing (live, not stalled)     PASS  5.89 -> 8.51 s
  UI says the relay is running               PASS  running (pid 22547, 10.6 MB) - running
  no page errors                             PASS
```

The cheaper JPEG option was looked for and does not exist on this unit, so H.264
through the relay is the only real preview:

* the vendor firmware does serve JPEG — `/etc/jffs2/www/cgi-bin/webui` embeds
  `http://$ipadd:3000/preview.jpeg` and `/snapshot.jpeg`, refreshed every 300 ms —
  but **nothing listens on 3000 here**: `preview.jpeg` gets connection refused,
  and a port sweep of the box shows only 21, 23, 554, 1984, 8080 and 8554;
* go2rtc's own snapshot endpoint answers
  `exec: "ffmpeg": executable file not found in $PATH`.

So the decode happens in the browser and the camera only muxes and serves bytes.
One caution this cost real time: while a browser was decoding and four CGIs were
running, the camera stopped answering on `:8080` for about a minute (load average
reached 16–25 on one ARM9 core). The page now slows its own status poll from 3 s
to 8 s while a live view is open, and the relay is expected to serve **one**
viewer.

### Security posture of the new code



The Gerge `header` CGI runs `eval $i` over every token in `QUERY_STRING` —
unauthenticated remote root — and those CGI files are sitting in
`/etc/jffs2/www` on this card right now. So the new API never evaluates input:
parameters match a fixed whitelist, numbers are type-checked, names are
percent-decoded then stripped to `[A-Za-z0-9 ._:-]`, and anything reaching the
PTZ FIFO must match a `case` pattern. Verified live:
`?act=goto&pan=1;reboot` → `bad args`; `?act=raw&cmd=q` (kill the daemon) →
`command not allowed`; bad token on `act=reboot` → `unauthorized`.

Preset files are `"<pan> <tilt> <name>"` parsed by field splitting, never
sourced — an early version sourced them, and `NAME=Door Front` would have
executed `Front`.

`--stop` kills by matching process name **and** port under `/proc`, never
`killall httpd`: the first draft used `killall` and would have taken the stock
8080 interface down with it.

### Video: why the preview cannot come from the camera

> **Corrected later — and the correction is the interesting part.** Everything
> below is still true about *re-encoding* on this box: nothing here can decode
> H.264 and give you a JPEG, and the two attempts documented below really do
> fail. What was wrong is the conclusion. Re-encoding was never the only way to
> get a picture into a page: **repackaging** the vendor's H.264 into fragmented
> MP4 needs no codec, no free core and about 7 MB, and the camera can be that
> process. The page now shows live video from the camera itself — see "Live
> preview relay" above. What is described below remains true and useful for the
> JPEG *snapshot* button, which genuinely does need a transcoder.

`/mnt/Factory/custom/rtsp` (started by `config.sh`) owns the video input and the

hardware encoder. Anything that wants to hand out a JPEG has to reach that same
encoder, and there are only two ways to try — both fail on this box:

* **Gerge's `libre_anyka_app`** is the process that would give `:554` **and**
  `:3000/preview.jpeg`. Tested live: with
  `LD_LIBRARY_PATH=/mnt/anyka_hack/libre_anyka_app/lib` it matched
  `/etc/jffs2/isp_gc1084.conf`, opened main 1280×720 / sub 640×360, reached
  `capture start`, served a valid SDP, and bound **both 554 and 3000** — then
  disappeared within about half a minute with nothing in its own log. It is
  36 MB too big to coexist with what is already resident.
* **`/mnt/anyka_hack/ffmpeg/ffmpeg`** (38 MB, it is on the card) is `Killed`
  instantly — the OOM killer. There is no way to pull a frame from the local
  RTSP and re-encode it on the box.

So preview is **relayed off the camera** instead: `tools/relay.py` decodes
`rtsp://192.168.10.46:554/vs1` on a PC and serves
`http://<pc>:8124/preview.jpg` (plus `/mjpeg`). Point the UI at it:

```sh
cd tools
./relay.py                      # ffmpeg on the PC, serves :8124
./deploy.py --preview=http://192.168.10.70:8124/preview.jpg
```

Measured cost on the camera of one relay client (640 px sub stream):
load `2.80 → 3.30`, `mem_free` unchanged at 10 MB. Real but modest; the sub
stream is what keeps it cheap. Home Assistant or go2rtc doing the same thing is
equally fine — anything that turns that RTSP into a JPEG URL.

Two traps, both hit on the way here:

* The vendor binaries **need `LD_LIBRARY_PATH`**. Started bare, `rtsp` dies with
  `can't load library 'libplat_vi.so'`. Restore the vendor stream with:
  `cd /mnt/Factory/custom && LD_LIBRARY_PATH=/mnt/Factory/custom/lib setsid ./rtsp &`
* Started without `setsid`, anything launched from a telnet shell dies of SIGHUP
  the moment the shell exits. That looked like "the process crashes on its own"
  for a while.

### Why no player can be embedded, VLC included

Asked whether the VLC player could be embedded in the page. Two independent
reasons it cannot, and the second one rules out every alternative too:

1. **The VLC browser plugin is gone.** `npvlc`/`axvlc` was NPAPI: disabled in
   Chrome 45 (2015), removed in Firefox 52 (2017) and Safari 12. There is no
   current browser in which `<object classid=…vlc…>` does anything.
2. **A page may not open a raw TCP socket.** RTSP *is* a raw TCP conversation on
   port 554 (plus RTP, often UDP). Browsers only allow HTTP(S), WebSocket,
   WebRTC and media-element fetches. So even VLC recompiled to WebAssembly could
   not reach `rtsp://192.168.10.46:554/vs0` — nor can ffmpeg.wasm, nor any
   "just embed a player" idea. This is why every in-page camera view in the world
   sits behind a converter: HLS, fMP4/MSE or WebRTC, all of which need a process
   on the network to repackage the H.264.

The claim here that "the camera cannot be that process (no free core, no free
RAM)" was **wrong**, and it is worth being precise about why, because it is the
kind of wrong that stops projects. It cannot be a *transcoder* — see the section
above. It can be a *repackager*: go2rtc ingests `rtsp://127.0.0.1:554/vs1` and
hands out the same H.264 NAL units as fragmented MP4 on `:1984`, and the viewer's
browser decodes. Measured cost of one viewer: 7-12 MB RSS, ~400 kbit/s out, and
the vendor server and encoder untouched. It runs on the card, on demand, with the
guards described in "Live preview relay" above.

The UI therefore does both things now: a real `<video>` in the page fed by the
camera's own relay, **and** `rtsp://…:554/vs0` / `/vs1` as `rtsp://` links for a
player app on the viewing device (VLC, mpv, MX Player), which still costs the
camera nothing at all.

The two traps that made this hard to see, both hit on the way here:

* The **`origin: "*"`** in go2rtc's api config is not cosmetic. The page is on
  `:8080` and the relay is on `:1984`; without
  `Access-Control-Allow-Origin` the browser refuses the fetch no matter that both
  ports are on the same box.
* **A live fMP4 stream needs MediaSource**, not `<video src=…>`: there is no
  duration to seek in and no end to the response, so the byte stream has to be
  appended to a `SourceBuffer` chunk by chunk. The codec string comes from the
  relay's own `stream.m3u8` (`avc1.4DE028`) rather than being hardcoded, because
  `addSourceBuffer` throws on anything the browser cannot decode.


### The preset bug, for the record

`saving positions does not work` was never a PTZ problem and never the browser's
fault. In the CGI that lists presets:

```sh
first=1
for f in $PRESETS/*.conf; do
  [ $first ] || printf ','      # <-- never prints a comma
  first=0
```

`[ 0 ]` is **true** in POSIX sh — a one-argument test asks whether the string is
non-empty, not whether it is non-zero. So the separator was never emitted:

```
{"presets":[{...,"name":"hhh"}{...,"name":"KREVET"}]}
```

Valid JSON with one preset. Invalid with two or more. `JSON.parse` threw, the
preset panel died silently, and every save still wrote its file on the card —
so the writes were there (`hhh`, `KREVET`) while the screen never changed. One
preset: fine. Two: broken. That is the whole bug, and it shipped in my first
version.

Three fixes came out of it:

* count instead of a boolean flag (`[ "$n" -gt 0 ] && printf ','`);
* `--verify` now `json.loads()` every endpoint instead of substring-matching,
  and does it with three presets present, because a substring test happily
  accepted `...}{...` for weeks;
* the CGI sends `Cache-Control: no-store` and the UI appends `?_=<ms>` to every
  call, so a browser can never answer `act=presets` from its own cache either.

The lesson generalises: **the endpoints said `ok` the entire time.** Anything
that reports success while the client silently chokes is missing one layer of
checking — here it was a parser in the test suite.

### CPU reality: this box has no idle core

Measured with `tools/loadtest.py`, which compares `/proc/stat` jiffies over a
quiet window and a window of UI requests (HZ=100, so 100 jiffies/s = one core):

```
control, no requests:      100.3 jiffies/s      load 4.31 -> 4.28
20 status requests:         95.7 jiffies/s      load 4.28 -> 7.22
```

The camera is already using a full core doing nothing you asked for — the vendor
`rtsp` alone sits at ~30% CPU with no clients attached, encoding regardless, and
a single `act=status` request took **4.3 s** to serve because it was queueing
behind that. It is why the relay pushed load to 5.5, and why an eager poller is
not free: busybox `httpd` serves one CGI at a time.

So the UI now polls status every 3 s instead of 1.5 s, pauses completely when
the tab is in the background (`visibilitychange`), and no longer polls presets on
a timer at all. Do not "improve" those numbers back down without re-running
`loadtest.py`.

### Replacing the vendor capture/encode path: milestone 1 measured (`src/akstream.c`)

`/mnt/Factory/custom/rtsp` is both the capture/encoder process and the RTSP
server, and the serving half is what costs the CPU (~33% of the single core with
one NVR attached, of which only ~2.3 points are `venc_encode`, plus ~9 points of
audio nobody can decode). Milestone 1 asks the narrow question: **can our own C
program drive the vendor SDK end to end — capture, 20 fps, valid H.264 — and at
what cost?** It can. Built by `src/Makefile` against the toolchain at
`~/opt/arm-anykav200-crosstool`, run with `src/run_once.sh` / `src/run_profiles.sh`
(they stop the vendor server for the test and always put it back — `trap ... EXIT`
— because this box has exactly one capture pipeline).

**The published VI struct does not match this library, and the error message
points at the wrong field.** `ak_vi_set_channel_attr` failed with
`sub channel argument error, w: 0, h: 0` while nothing in our sub channel was
zero. objdump of that function in the camera's own `libplat_vi.so` V2.3.01
(md5 `f924ed82...`) shows it reading main w/h at offsets 16/20 and sub w/h at
24/28 — two ints per channel, 32 bytes total — while `ak_vi.h` declares four
ints per channel (48 bytes) with `max_width`/`max_height`. Filling the published
struct therefore writes main's max pair onto the sub channel, which is exactly
what the error quoted: `w: 1280, h: 720` was *our main channel's max*, not the
sub size. `src/vi_abi.h` carries the measured layout with a size check, and
`src/akvi_probe.c` exists because `ak_vi_get_channel_attr()` — the obvious way
to read the layout back — **segfaults** in this build, so the layout had to be
probed. Probing also bounded the legal sizes: main 640..1280 x 480..960, sub at
most 640x480 (9 of 12 candidates accepted, the three illegal ones rejected).

**The encoder library requires `goplen >= fps`, and crashes when it is not.**
Every run with a GOP shorter than one second died at the *second keyframe*, after
encoding perfectly up to frame `goplen - 1`. The stack scan built into akstream
(named symbols still exist in `libakstreamenc.so`, md5-identical to the SDK copy)
gave the chain `_AKVD_Enc_Start_One_Frame → H264EncStrmEncodeStart →
H264BeforePicRc → PicQuant → VBR_ratecontrol → __aeabi_ldiv0`, and the
disassembly at `VBR_ratecontrol+0x11c..0x188` is a division by a quotient that is
itself an integer division — zero once the GOP is shorter than a second. ARMv5
has no hardware divide, so the soft helper calls `raise(SIGFPE)`, which is why
the first report showed a pc inside `raise()` and looked like a pthread bug.

| fps | gop | result |
|---|---|---|
| 20 | 5 | dies after 4 frames |
| 20 | 10 | dies after 9 frames |
| 20 | 15 | dies after 14 frames (VBR and CBR alike) |
| 20 | 20 | fine — 19.86 fps over 8 s |
| 10 | 10 | fine — 9.66 fps over 6 s |
| 15 | 30 | fine — 14.91 fps over 12 s |

That kills the profile this plan originally proposed (`640x480@20, GOP 15`) and
explains the stock binary's `gop 50` at 25 fps. `akstream` now clamps gop up to
fps instead of crashing, and says so.

**Frame rate is ours to set.** The vendor binary never calls `ak_vi_set_fps` or
`ak_vi_set_switch_fps_enable` at all (its import list is `match_sensor, open,
get_handle, get_sensor_resolution, set_channel_attr, switch_mode, capture_on,
close`), which is how it ends up near 10 fps behind an SDP that advertises 25.
Asking for 20 gives 20 — measured over 10 s, one stream at a time:

| profile | frames | key frames | rate | bitrate | our process CPU |
|---|---|---|---|---|---|
| sub 640x480 @20 gop20 512k | 200 | 10 | 19.99 fps | 232 kbps | **6.5% of one core** |
| main 1280x720 @20 gop20 1500k | 199 | 10 | 19.89 fps | 561 kbps | **7.2% of one core** |

`ffprobe -count_frames` agrees (200/199 frames, H.264 Constrained Baseline,
640x480 / 1280x720), `ffmpeg -f null -` decodes both with no errors, and the
elementary stream is Annex-B with NAL order SPS, PPS, IDR, slice —
`profile-level-id=42e028`, so the RTSP SDP can carry
`sprop-parameter-sets` straight from the first frames and the packetiser needs
FU-A splitting.

Two API traps worth knowing before writing the server: `ak_vi_set_fps()` returns
0 for 15/20/25 but `ak_vi_get_fps()` reports 30 before *and* after, so it
confirms nothing — only counting delivered frames does; and the ISP re-writes the
sensor rate on its own (`set sensor fps: 15` from `isp_gc1084.conf`, later
`set fps:10`), so the encoder's rate has to be watched, not assumed.

### Milestone 2: our own RTSP server on that pipeline (`src/akrtsp.c`), measured

`src/akrtsp.c` is the second half of the plan: one process, one VI device, two
encode channels (`/vs0` main 1280x720, `/vs1` sub 640x480 — the names go2rtc's
`LIVE_SRC` and the stock firmware already use), RTP over TCP interleaved, and
nothing else. No audio: the camera's only audio is PCMA/G.711, Chrome's WebRTC
will not take it, and the vendor's `ai_capture` plus the kernel `aec_task` cost
about nine points of core for something nobody hears. Built by `make akrtsp`,
22 KB stripped.

Every claim below comes from `tools/rtsp_probe.py`, which exists because
`ffmpeg -t SECS` **lies about a live stream**: it limits output duration derived
from the timestamps the *server* sends, so a timestamp bug shows up as "385
frames in 4 seconds" instead of as an error. The probe counts packets, access
units (marker bit), key frames and sequence gaps against the wall clock and
prints the RTP timestamps it saw.

| stream | frames/s delivered | key frame every | seq gaps | media time vs wall time |
|---|---|---|---|---|
| ours `/vs1` 640x480 | 20.24–20.29 | 1.00 s | 0 | 1.00 |
| ours `/vs0` 1280x720 | 20.22–20.24 | 1.00 s | 0 | 1.01 |
| vendor `/vs1` (production) | 12.76–13.57 | 2.51–2.68 s | 1 | **526** |

`ffmpeg -rtsp_transport tcp -c copy` on our `/vs0` produced 503 frames,
`avg_frame_rate=20/1`, and **no warnings at all**; the same command against the
vendor server is where the timestamp problem below first surfaced. So this is
not just "cheaper": the stock server's timing is wrong on three counts — it
delivers ~13 fps behind an SDP that advertises 25, its GOP is 2.5 s, and its
RTP timestamps advance 0/8100/9000 ticks per frame with 47-million-tick jumps,
which is how "2638 media seconds in 5 wall seconds" ends up in a recording.

**The SDK's `vs.ts` is milliseconds, but it is not a usable media clock.**
One channel: `144433750 → 144433800 → 144433850` (50 ms steps at 20 fps) —
`ak_global.h` is right about the unit. Two channels: it came back **0**, which
muxed as a constant timestamp. So the RTP clock is ours: count frames at
`90000/fps` and creep the base **forward only** when the encoder falls behind
nominal. A timestamp that goes backwards is a hard fault in every client
(ffmpeg: `Non-monotonic DTS`, then it muxes nonsense); one that runs slightly
ahead costs nothing here, because there is no audio track to keep in step with
and no RTCP sender report to contradict it. An earlier version corrected in both
directions and produced backwards steps on the main channel inside a minute —
that asymmetry is what the forward-only rule protects.

**A swapped RTP header is invisible until you measure it.** I wrote the SSRC at
bytes 4–7 and the timestamp at 8–11. Video still played, the probe still counted
20.25 fps, and the only symptom was `rtp ts 1234324106 -> 1234324106`: the
client had been reading the constant SSRC as the timestamp. Nothing anywhere in
the pipeline objected.

**Two races on the one capture pipeline, and one of them can leave the camera
with no stream server at all.** `ak_vi_match_sensor` fails while the vendor
binary holds the device; worse, the vendor binary exits the *other* way round
with `vi init fail` if it starts before our process has released the device, and
`ak_vi_set_channel_attr` answers `VIDIOC_S_FMT, Device or resource busy` if it
starts before the vendor's threads have let go of the ISP. So
`src/run_server_test.sh` and `src/run_idle_test.sh` kill, **poll `ps` until the
process is really gone**, start, and then verify by port (`:554`) instead of
trusting the launch, with one retry.

**Match the vendor server by command word, not by path.** The scripts' greps used
`grep 'custom/rtsp'`, which matches `/mnt/Factory/custom/rtsp` but **not**
`./rtsp` — and a vendor server restarted by hand runs as `./rtsp`. The script
then reported `vendor rtsp pid: none`, left that process holding the ISP, and
every later test failed with "Device or resource busy" for a reason that looked
like a camera fault. The scripts now use `ps | grep -w rtsp | grep -v akrtsp`.

**What the stock server teaches about being a client here** (all of it is encoded
in `tools/rtsp_probe.py`): it never answers a `DESCRIBE` that follows an `OPTIONS`
on the same connection; it resets the connection if a request lacks the
terminating blank line or carries headers it does not expect; and its media
controls are `camera` and `micphone`, not `trackID=0`. Our server therefore
answers `DESCRIBE` on its own terms, keeps requests minimal, and sends the SDP in
**one** write — two writes leave the SDP in its own TCP segment, which is legal
and made one thin client read the SDP as if it were the next response.

Cost, measured with `top` inside the test scripts while two client sessions came
and went: **4.5–17.8% of the core**, against ~33% for the vendor binary with the
NVR attached, and consistent with the 6.5–7.2% a single-channel `akstream` run
measured with `clock()`. `top` on this box is coarse, so read that as "well under
half the vendor's cost", not as a precise figure. With no client at all the
server held 20.22–20.26 fps for 65 s and free memory did not move (`free`:
~2.7 MB — the same number that bounds `MAX_SESS 4` × 256 KB per-session rings).

Load-bearing design points, not style: a slow client is GOP-dropped and
resynchronises on the next keyframe rather than being allowed to block the
encoder (on a single core with a loaded WiFi link a stalled NVR would otherwise
stall capture for everyone else); every session has its own writer thread, so the
producer never blocks on a socket; and GOP is clamped up to fps on the way in,
because of the `VBR_ratecontrol` division from milestone 1.

Still open before this replaces the stock server:

* **UDP transport is not implemented** — a UDP `SETUP` gets `461`. Deliberate for
  now: measured on this network the NVR holds two TCP connections and there is
  not one UDP socket on the box, and go2rtc pulls RTSP over TCP as well.
* **No RTCP sender reports and no session timeout reaping**; sessions die with
  their socket. Enough for the two known consumers, not for arbitrary clients.
* ~~**It has never served the NVR.**~~ **Done** — it owns `:554` now and the NVR
  pulled both channels from it; see "Deployed" below. What remains is persistence
  across a reboot, and supervision (neither our server nor the vendor's is
  restarted by anything).
* One early run exited ~10 s after the last client hung up (the log's `stopping`
  means a signal was caught). The handler now prints which signal; the
  instrumented run then survived two connect/disconnect cycles and 75 s of
  uptime, so the exit is not reproduced — the logging stays in until it is.

### Deployed: `akrtsp` on :554, measured against the real NVR

`sh /tmp/run_takeover.sh` was run. It rehearses on `:8554` first, then hands
`:554` to `akrtsp`, and its `EXIT` trap (armed before anything is stopped, still
armed afterwards) restores the vendor binary if anything leaves port 554 dead.
This is the first time our server has faced the vendor NVR, so everything below
is that comparison, with the **same script and the same measurement method on
both sides** (`src/run_watch.sh`: `/proc/<pid>/stat` utime+stime over a measured
interval, not `top`'s swinging %CPU column).

| | vendor `rtsp` (before) | `akrtsp` (now) |
|---|---|---|
| CPU with the NVR attached | **29.3–33.9 % of the core** | **16–20 %, steady 18 %** |
| frames delivered | 12.76–13.57 fps | 20.20–20.23 fps |
| key frame interval | 2.51–2.68 s | 1.00 s |
| RTP timestamps | 526× wall time | 1.00× wall time |
| kernel `[aec_task]` | busy (112 min accumulated) | **0.0 % — idle** |
| free memory while running | ~3.3 MB | **~4.5–4.9 MB** |

The NVR reconnected **by itself**, and completed a full handshake against our
server — `OPTIONS`, `DESCRIBE`, `SETUP`, `PLAY` on `/vs0` and then `/vs1`, two
TCP connections, exactly the shape it used with its own binary. It sends no
`GET_PARAMETER` keepalives at all; it just holds the connection, which is why
session lifetime here is simply socket lifetime.

Health of that link, checked rather than assumed: **Send-Q 0** on both NVR
connections (we drain as fast as the encoder produces, so the per-session rings
never back up into the encoder), and `wlan0` tx counters grew **673 KB in 11 s
(490 kbps)** — matching the encoder's own 331 kbps main + 135 kbps sub = 466 kbps.
The NVR's first connection connected and closed with 0 packets (a probe), the
next two are the live ones.

The preview relay was then re-tested against our server with the browser path's
own suite (`./tools/live.py --test`), and it passes end to end: go2rtc pulls
`rtsp://127.0.0.1:554/vs1` from us, advertises `avc1.42E028` over HLS, streams
fragmented MP4 to a viewer (221 KB in 12 s, 253 fragments ≈ 21 fps out), keeps
21 MB available while it runs, and after `live_stop` the camera is back exactly
where it was — one relay process gone, ports 1984/8554 closed, overcommit back
to 0, our server's `oom_score_adj` back to 0, 25.2 MB available. **22/22 checks.**

Two questions that were open when this was first measured have since been closed,
each in its own section below: nothing supervised either server, and a reboot put
the stock binary back. Both are dealt with now — see "Supervision" and
"Persistent: the card, not `/tmp`". The rollback is still one command while the box
is up (`run_takeover.sh --restore`), and still a one-line comment in `config.sh` if
the vendor binary is wanted back for good.

**A grep that matched its own script.** The helper greps in these scripts went
through two wrong versions of the same idea. `grep 'custom/rtsp'` misses a
vendor server restarted by hand as `./rtsp` (so the script thinks nothing is
running and the real one keeps holding the ISP). `grep -w rtsp` then *matched
the watch script's own command line* — `sh /tmp/run_watch.sh 8 rtsp` contains
the word `rtsp` — so one run measured its own grep pipeline and reported
`cpu=0`. All of them now match `/proc/<pid>/comm` exactly. The lesson generalises
to any "find the process" helper on a busybox box: match the executable, never
the command line.

### The purple image: night mode, and why nothing on this board can decide it

The replacement server ran for two days before the picture itself was questioned:
*"all good but image is purple-ish?"*. It was not a codec problem and it was not a
filter stuck in the wrong place. Measured with `tools/rgbshot.py` (grab one real
key frame from the live stream, report channel means):

| state | measured | what it means |
|---|---|---|
| day-mode ISP, IR-cut out (how it was left) | mean R=152 G=76 B=155 → **R−G +76, B−G +79**, 1.7 % neutral | every pixel tinted: infrared, rendered by a day-mode ISP |
| day-mode ISP, IR-cut in (`ak_drv_ir_set_ircut(1)`) | mean R=G=B=0.2, range 0..255 | **the room is lit only by IR** — with the filter in, nothing arrives |
| **night-mode ISP, IR-cut out** | **mean R=G=B=124.6, range 0..255** | correct: monochrome night picture, full tonal range |

So the camera sits in a dark room lit by its own IR illuminator, the filter is
correctly out, and the ISP was left in **day** mode. The vendor's log says what it
does about that, in its own words:

```
now set to night
[isp_switch:152] switching isp mode -> night
        frame_rate=10 max_exp_time=2250 light_gain=10
```

Our server had no such step at all: it configured the pipeline and left the mode
the conf file booted into. Adding it is three lines — `ak_vi_switch_mode(vi,
VI_MODE_NIGHT)` — and the log then shows the library's own `switching isp mode ->
night` line, with the colour numbers above. Night mode wants a 10 fps sensor
(`*** sensor fps switch 10 to 20 ***`), which our `ak_vi_set_fps(20)` re-asserts;
`rtsp_probe.py` then still reports 20.29 fps, 0 gaps, 1.01 media s per wall s.

**What is deliberately not automated, and why.** Three candidate decision sources,
each measured rather than assumed:

* **The light dependent resistor** (`ak_drv_ir_get_input_level`, in
  `libplat_drv`): reports **DAY in this dark, IR-lit room** (read 1; thresholds
  day min=1000 max=2000, night max=618) because the illuminator lights the
  resistor beside it. Acting on that reading made things worse in two ways — it
  switched the ISP back to day, and it inserted the IR-cut filter, which produced
  the black frame in the table. So `--auto-night` exists but is not the default,
  and moving the mechanical filter is behind `--ir-follow`, off by default: a
  sensor that lies should not be allowed to touch optics. `ptz_daemon` already
  owns that gpio anyway.
* **The library's own light-adaptation thread** (the `change_fps_pthread` and the
  "hight light / low light" blocks in `isp_gc1084.conf`, gated by
  `ak_vi_set_switch_fps_enable`): with it enabled and us not interfering at all,
  it never switched to night in 90 s. It is not the vendor's mechanism.
* **The ISP's image statistics** — the right source in principle
  (`ak_vpss_isp_get_auto_day_night_level`, whose parameters are literally AWB
  day/night counters). Not available here: that symbol exists only in the newer
  SDK dump, and the `libplat_vpss` this board runs does not export it.
  `libakispsdk` does export `Ak_ISP_get_awb_stat_info` /
  `AK_ISP_get_rgb_hist_stat_info`, but **no header in the dump declares them**, so
  using them means guessing struct layouts on a board that cannot be debugged
  interactively. Not done.

Checking the vendor executable itself (`readelf -sW logs/vendor/rtsp`) completes the
picture and corrects a guess made earlier in this file from library imports alone:
`libapp_rtsp.so` really does import no ISP/VI/IR function, but the **`rtsp`
executable** imports `ak_vi_switch_mode` (undefined, referenced twice) and carries
its own 456-byte function called `ak_misc_set_video_day_night` — a symbol no
library on this card exports and no header in the dump declares, because it is the
vendor's own code compiled in. Its strings also name `/sys/user-gpio/IR_LED` and
`/sys/user-gpio/gpio-ircut_a`: the vendor drives the illuminator and the mechanical
filter straight through sysfs, not through `ak_drv_ir`. So the mechanism is ours
after all — switch the ISP mode — plus two gpios, and both nodes turned out to be
world-writable and to answer (see "Day/night from the browser").

What mode is *asked for* is a file now, not a flag. The launcher and the supervisor
pass `--mode-file /mnt/camweb/daynight.conf`; `--force-night night` is only the
fallback for when that file says nothing usable. The web UI writes the file, so the
mode outlives a restart of the server and a reboot of the camera, and can be changed
while the stream is running.

What night mode costs, measured rather than assumed: an IR-lit scene is noisy and
VBR spends for it — main runs at **999 kbps** against the ~500 kbps of the day-mode
picture — and the server sits at **23.3 % of the core** (10 s `/proc/stat` sample)
against 16–21 % before. Still well under the vendor's 29–34 %, and inside the
1500 kbps main target.

### Supervision: what brings the stream server back when it dies

Nothing did. Neither the vendor binary nor `akrtsp` restarts itself, so one crash
means the NVR quietly records a gap and nobody finds out until someone reviews
footage. `src/run_akrtsp.sh` closes that, with the stand-down rules being the part
that matters on a board with **one** capture pipeline:

1. `akrtsp` alive → do nothing.
2. the **vendor** server alive → stand down and exit. Someone chose it with
   `run_takeover.sh --restore`; taking :554 back from a running server would break
   the rollback path, which is the one thing that must keep working.
3. **a handover in flight** → skip this round. `run_takeover.sh` stops `akrtsp` for
   ~2 s on purpose while moving :8554 → :554; racing it produces the second
   process dying on `VIDIOC_S_FMT, Device or resource busy`, i.e. a crash loop that
   looks like `akrtsp` being unreliable when it is really two supervisors fighting.
4. nothing owns :554 → start it, log it, count it. After 3 failed starts in a row,
   start the **vendor** server and give up: a camera recording on the worse server
   beats a camera with a better server that does not come up.

It also keeps one instance (pidfile), rotates its own log at 400 kB because `/tmp`
is tmpfs on a 32 MB box that has been OOM-killed before, and defaults to the same
`AK_ARGS="--mode-file /mnt/camweb/daynight.conf --force-night night"` as the
launcher, so a restarted process gets night mode without anyone remembering — and
picks up whatever mode the UI had set, from the file.

Tested by killing it, not by reading the script: `kill -9` on the running server
left :554 empty, and the supervisor logged `nothing on :554; starting akrtsp
(attempt 1/3)` then `akrtsp up on :554 (pid 21693)` — bound inside one 8 s round.
The NVR reconnected by itself (new ports 48775/48776, Send-Q 0), night mode came
back (`isp -> night (--force-night)`, then R=G=B), and the restarted server measured
20.09 fps, 0 seq gaps, 1.00 media s per wall s. That was a hand-started `/tmp` run;
the same loop now starts by itself at boot from the card (next section), and a
`kill -9` after the boot was recovered the same way — new pid, `/proc/<pid>/exe`
still `/mnt/camrtsp/akrtsp`, night mode re-applied from the control file.

The give-up-and-fall-back path was tested too, with `src/fake_akrtsp.sh` (a binary
that always exits 1) under isolated paths so it could not touch the live instance:
`BIN=/tmp/fake_akrtsp.sh PIDF=/tmp/t_sup.pid SUP=/tmp/t_sup.log NOFALLBACK=1`. With
the real server deliberately down it counted `attempt 1/2`, `attempt 2/2`, and
stopped at `2 failed starts in a row`. That bench run found a bug in the script's
own success test: it had only asked `netstat` whether :554 was listening, which is
true whenever *anything* holds the port — a wedged previous server, or this bench
run sitting next to a live one — so it would have reported a start that never
happened. Success now requires the process it launched to be alive by `comm` **and**
the port to be up. `NOFALLBACK=1` exists for exactly this kind of test: it stops the
supervisor from starting the vendor binary while a real server is still streaming.

### Persistent: the card, not `/tmp`

Everything above ran out of `/tmp`, which is tmpfs: a reboot put
`/mnt/Factory/custom/rtsp` back and took our server and its supervisor away with
it. That is no longer true, and the change is three files on the card plus one line
in the boot script.

```
/mnt/camrtsp/akrtsp            the stripped build (md5 checked after every upload)
/mnt/camrtsp/run_akrtsp.sh     the supervisor; BIN defaults to this path if the
                               binary is here, else /tmp/akrtsp
/mnt/camrtsp/run_takeover.sh   handover + --restore, so rollback survives a reboot
/mnt/camweb/daynight.conf      the wanted ISP mode: one word, night here
```

and in `/mnt/Factory/config.sh`, at the point where the vendor server used to start
(after `sleep 30`, so the ordering versus `net_manage.sh` and `ptz_daemon` is the
one the vendor had):

```sh
BIN=/mnt/camrtsp/akrtsp AK_ARGS="--mode-file /mnt/camweb/daynight.conf --force-night night" \
  /bin/sh /mnt/camrtsp/run_akrtsp.sh 10 3 >/tmp/akrtsp_boot.log 2>&1 &
# /mnt/Factory/custom/rtsp &
```

The vendor line is **commented, not deleted**, and the vendor binary itself is
untouched, so the rollback is a one-character uncomment (or
`sh /mnt/camrtsp/run_takeover.sh --restore` while the box is up — the supervisor
sees the vendor server on :554 and stands down by itself, which is the same rule it
follows when a person chose it by hand). `/mnt/camweb/backup/` holds
`config.sh.orig` (pristine factory) and `config.sh.vendor-rtsp` (the state just
before this change). `config.sh` is uploaded as `config.sh.new`, parsed with
`sh -n` on the camera, and only moved into place if it parses — the same discipline
`deploy.py` uses for the web line.

Verified by rebooting the camera, not by reasoning about the script. From the boot
log and `/proc`: supervisor up at uptime 38 s, `akrtsp up on :554 (pid 1683)` at
58 s, `/proc/1683/exe -> /mnt/camrtsp/akrtsp`, command line carrying
`--mode-file /mnt/camweb/daynight.conf`, status file `mode=night want=night`, no
process with comm `rtsp` anywhere, NVR reconnected on its own, and the stream
measured 20.20 fps, GOP 1.00 s, 0 seq gaps, 1.00 media s per wall s.

### Day/night from the browser: three controls, three mechanisms

"Switch the IR off" and "switch to day" sound like one request and are three
different pieces of hardware. The UI and the API name them separately, because
collapsing them is how you end up with a button that appears to work and a black
picture:

| control | what it is | how it is reached | measured effect on this camera |
|---|---|---|---|
| **Image mode: day / night / auto** | the ISP's colour/IR handling | one word into `--mode-file`, read by the server's light thread every 5 s, applied with `ak_vi_switch_mode` — **no restart, the stream keeps running** | night `R=G=B`, 100 % neutral; day `R−G +55.7, B−G +36.5`, 5 % neutral (the purple picture) |
| **IR illuminator** | the IR LEDs, a gpio | `echo 0\|1 > /sys/user-gpio/IR_LED` (world-writable) | mean luminance 142 → 127 and **75 % of the pixels of a static frame moved** (mean \|diff\| 27.5) — automatic gain takes back some of the brightness, so the mean alone understates it |
| **IR-cut filter** | the mechanical filter in front of the sensor | `act=ircut`, through `ptz_daemon`, which drives the motor | filter in mean **44**, filter out **136**; takes a couple of seconds to travel, so the reply reports the position the filter is *in*, not the one that was asked for |

The server publishes the truth in `--status-file` (`/tmp/akrtsp.status`): its pid,
the mode actually in force, the mode that was asked for, the light sensor, the
configured fps and gop, client count and frame counters — written to a temp name and
renamed so the CGI cannot read it half-written. The CGI puts that next to the two
gpio nodes in `act=status` (`night_set`, `night_mode`, `ldr`, `ir_led`, `ircut`,
`akrtsp`, `ak_pid`), and the page shows **asked** and **in force** side by side:

```
asked: night  |  ISP now: night  |  light sensor: day  |  IR led: on  |  filter: out
```

That line is the honest version of the earlier finding: the light sensor says
**day** in a dark room lit only by this camera's own IR, which is exactly why
`auto` is a button and not the default, and why the sensor is shown rather than
hidden. A control that repaints itself from the last click is worse than no
control, so every one of these re-reads the camera: `act=nightmode&state=day|night|auto`,
`act=irled&state=on|off|get`, `act=ircut&state=0|1`.

Verified in a real browser rather than with curl: `./tools/night-check.mjs` drives
headless Chrome over CDP, clicks the page's own buttons and only passes when the
camera reports the state back — mode reaching the ISP, illuminator off and on,
filter travelling in and out, no page errors, ending with the camera as it was
found. 11/11 twice, screenshot in `logs/night-panel.png`. The frame comparisons
behind the two gpio rows come from `tools/ppmdiff.py`, which diffs two PPMs
pixel-wise: mean brightness cannot show an illuminator doing anything while
automatic exposure compensates, and it did not.

## 5. Problems found that are worth fixing

* **The monitoring cost more than the streaming.** The status endpoint needed to
  know whether `akrtsp` was alive, and the obvious busybox way — walk `/proc`,
  `cat` each `comm` — forks **once per process**, about 100 forks per poll. The page
  polls every few seconds, so with a browser open the camera reached **0 % idle and a
  load average of 11** while `akrtsp` itself was only 24 % of the core: three or four
  `api` processes, each grinding through `/proc`, were starving the thing they were
  watching. Fixed by having the server publish its own `pid` in its status file: the
  CGI now reads one file and confirms the process with the shell's `read` **builtin**
  (no fork), and the supervisor's own `pids_by_comm` loop does the same. Load with
  the page open went 11 → 2.9, and 1.6–2.3 with it closed. Worth keeping as a rule
  for this class of box: on one 400 MHz core, a fork-per-process scan is not a
  diagnostic, it is a workload.
* **You cannot overwrite a running binary.** Uploading a fresh build over
  `/mnt/camrtsp/akrtsp` while it executes fails — `ftpd` answers `553 Error`, which
  is ETXTBSY wearing a different hat. The working order is: upload as
  `akrtsp.new`, then `mv -f` it into place (a rename needs no write permission on the
  running inode, and the old process keeps its own copy until it exits), then restart.
* **A supervisor with the wrong default binary path fell back to the vendor server —
  quietly, and correctly.** After the move to the card, starting `run_akrtsp.sh`
  without `BIN=` in the environment still looked for `/tmp/akrtsp`, found nothing,
  and did exactly what it is designed to do: started the vendor server and exited.
  Nothing was broken, and the log said `no /tmp/akrtsp on disk; falling back to the
  vendor server` — which is the only reason it was noticed at all. `BIN` now defaults
  to whichever install location exists. Two lessons: a default path is a deployment
  decision, and a supervisor that explains what it did and why is what turns a
  surprise into a five-second read of a log line.
* **Two more greps that matched themselves,** after the same trap had already been
  documented twice here. `ps | grep -c "custom/rtsp"` reported the vendor server
  running when nothing was running (the grep's own argv), and
  `pkill -f camweb-chrome` killed **the shell that ran it**, because that string was
  in its command line. The safe forms used here now: compare `/proc/<pid>/comm`
  exactly, and when matching a command line, match on the *executable's* own
  attributes rather than a string the caller also typed.
* **A supervisor that matched its own grep.** `grep -l run_takeover
  /proc/[0-9]*/cmdline` — meant to notice a handover in flight — also matched **the
  grep process itself**, whose argv contains the word it is looking for. The
  supervisor therefore believed a handover was permanently in progress and
  cheerfully stood down while :554 sat empty for 20 s after a killed server. The
  same trap as `ps | grep -w rtsp` matching the watching script, and the same fix:
  the `[r]` bracket class, which matches what it is looking for and not its own
  command line. Worth noting what made this findable at all: the supervisor *logged
  the reason it did nothing* on every round. Had it stayed silent, this would have
  looked like akrtsp refusing to start.
* **Deploying the wrong binary while believing it was the new one.** Two separate
  traps in one afternoon of A/B testing. `make akrtsp` does **not** strip, so
  `akrtsp.stripped` keeps the previous build's bytes until `make check` runs - and
  that is the file the upload sends. And issuing build → upload → restart as three
  parallel commands let the handover `mv` the *previous* `/tmp/akrtsp.new` into
  place while the fresh upload was still landing, so a "new" server ran old code and
  behaved inexplicably. Both are now caught the same way: `md5sum` of the local
  `akrtsp.stripped` and of `/tmp/akrtsp` on the camera are compared after every
  deploy, and deploy chains are run as one sequential command.
* **A frame grab that lies.** `ffmpeg -i rtsp://... -frames:v 1` joins an H.264
  stream mid-GOP and decodes a P-frame with no reference: it comes out near-black
  and reads exactly like "the camera went dark". `tools/rgbshot.py` waits for a
  real key frame (`select=eq(pict_type,I)`) and refuses short PPM reads, which is
  what made the day/night numbers trustworthy.
* **The preview relay's OOM protection was blind to our server.** While go2rtc
  relays, the supervisor pushes the *stream producer* to `oom_score_adj -500` and
  raises go2rtc to `1000`, so the kernel kills the expensive relay and not the
  thing the whole camera depends on. `run_live.sh` found that producer by
  `/proc/<pid>/comm == "rtsp"` — a lookup written before a second producer could
  exist. With `akrtsp` on :554 the protection silently applied to **nothing**:
  our server kept its default score and would have been the first thing OOM ate,
  taking the NVR's stream with it. Same class of bug as the `grep -w rtsp`
  helper: anything that names the vendor process has to name both. Both
  `run_live.sh` and `stop_live.sh` now match `rtsp` **and** `akrtsp` by `comm`,
  and the relay's own test asserts it (`vendor rtsp oom-protected while relaying
  PASS  5142-akrtsp:-500`). Found only because the replacement exists — with the
  vendor server alone, the old code was correct.
* **`stop_live.sh` escalated to SIGKILL faster than the supervisor could clean
  up.** It sent TERM, waited one second, then `kill -9`. The supervisor's TERM
  trap waits up to 5 s for go2rtc to exit before it lifts the producer's OOM
  pin — so on a slower teardown the SIGKILL landed mid-teardown and the producer
  stayed pinned at **-500 forever**, invisible in every status field. The
  fallback reset in `stop_live.sh` did not save it either, because (previous
  bullet) it only matched `rtsp`. Now the escalation waits 6 s, and the fallback
  matches both producers; the relay test went 21/22 → 22/22. General lesson: a
  "belt and braces" killer has to be slower than the thing it is killing, and a
  fallback cleanup has to know every resource the trap was supposed to release.
* **`vm.overcommit_memory` could get stranded at 1.** A superseded supervisor
  correctly leaves the kernel setting alone, but the next one read that 1, saved
  it as *the vendor default*, and later "restored" it - permanently. The saved
  value is now recorded once per boot in `/tmp/live_oc` and never overwritten, so
  a stop always returns the true default. Measured: before 0, during 1, after 0.
* **The header's memory number was raw `MemFree`.** On a box with no
  `MemAvailable` and 20 MB of reclaimable page cache it read
  `3/36 MB` while 22 MB was perfectly usable — alarming and useless. It
  now reports what `live_avail()` computes, the same number the relay's
  own refusal threshold uses, so the header and the guard cannot
  contradict each other.

| # | Finding | Fix |
|---|---|---|
| 1 | **The clock has never been right.** `time_source=172.16.1.99` is not on this network (you are 192.168.10.0/24), so NTP never worked and `date` reads 1970-01-09. Every log line and recording timestamp is useless. | Point `time_source` at your router or a real NTP host. The clock resets to 1970 on every boot until it can sync. |
| 2 | **FTP is on, as root, writable to `/`, in plaintext.** `tcpsvd 0 21 ftpd -w /` is started by `/etc/init.d/rc.local` on *every* boot, hacked or not. `run_ftp=0` does not stop it starting — `gergehack.sh` only *kills* it, and that never runs here. `root`/`anyka` gets write access to the whole filesystem. | `run_ftp=0` **and** reboot, or `killall tcpsvd` now. Nothing here needs FTP except my uploader. |
| 3 | **Telnet is root with no password**, on `0.0.0.0`. And the stock 8080 UI has no login either. | Acceptable on an isolated VLAN only. |
| 4 | **`config.sh` starts audio beeps (`didi.aac`) and a `vpn/vpn.sh` with nothing to connect to.** | Delete both lines: less noise, less surface, slightly less CPU. |
| 5 | **`onvif` is on the card and never started.** Free NVR / Home Assistant integration if wanted. | Add `/mnt/Factory/custom/onvif &` to `config.sh`; test before trusting it. |
| 6 | ~~**Our httpd is unsupervised.** If it dies, the UI is gone until `./deploy.py` runs again.~~ | **Fixed.** `/mnt/camweb/run_web.sh` runs `httpd -f` in a restart loop. Tested: killed the PID, page back in 3 s. |
| 7 | ~~**Nothing of ours starts at boot**, so a reboot returns the box to stock state.~~ | **Fixed.** `config.sh` starts the runner; proven by rebooting the camera and finding the UI on 8080 with nothing run by hand. |
| 8 | **My first UI failed silently.** Naming a preset used `prompt()`, which returns `null` in several in-app browsers, and any non-JSON response rejected with nothing shown — so "Save" looked dead while the camera had in fact written the preset. | Fixed: name field inside the card, two-click delete, token panel, and every bad response prints. Worth remembering for anything built for this class of browser: a control must never be able to fail quietly. |

## 6. Suggested next steps, in order

1. **Use the new UI on :8080 for a few days.** It now owns the stock port and
   survives reboot, but nothing was destroyed: `./deploy.py --revert` restores
   `config.sh` and the factory page in one command, and the pristine copies are
   in `/mnt/camweb/backup/` on the card and in `pull/` here. Use the window to
   find the real travel limits and name your presets.
2. **Fix NTP** — one line, and it makes every other diagnosis easier.
3. **Turn FTP off** — but note `deploy.py` uploads over FTP, so either leave it
   or re-enable it when you need to push changes (`/etc/init.d/rc.local` starts
   `tcpsvd 0 21 ftpd -w /`, and `run_ftp=0` in `gergesettings.txt` does **not**
   stop it: `gergehack.sh`, which is what kills it, never runs on this card).
   Real options: set a root password for FTP, or bind it to the LAN only.
4. **Video — settled, and it is not on the camera.** The `libre_anyka_app` swap
   (option 1 above) does not survive on 36 MB: it binds 554 and 3000, reaches
   `capture start`, then vanishes inside half a minute. On-board ffmpeg is
   OOM-killed on start. So run the relay — `tools/relay.py` on any always-on box
   on the LAN, then `./deploy.py --preview=http://<that-box>:8124/preview.jpg`.
   The existing `rtsp://…:554/vs0` is untouched, so any recorder or NVR keeps
   working; the relay is just a second client, and it measured as +0.5 load on
   the camera.
5. Optional: enable `onvif` for NVR use, and drop the two dead lines in
   `config.sh` (`ak_adec_demo` beeps and `vpn/vpn.sh`) — both are noise, a little
   CPU, and extra surface.
6. **`akrtsp` on :554 is now the permanent arrangement — soak it, then look at the
   two open questions.** Measured against the real NVR with the same method on both
   sides: 18 % of the core against the vendor's 32 %, 20.2 fps against 13.6, GOP
   1.0 s against 2.6 s, correct RTP timestamps against 526× wall time, the kernel
   `aec_task` now idle, and ~1.3 MB more free memory (see "Deployed" above). Night
   mode is asked for in a file the UI writes (see "Day/night from the browser"), a
   supervisor restarts the server and falls back to the vendor binary after three
   failed starts ("Supervision"), and the whole thing is installed on the card and
   starts from `config.sh` ("Persistent: the card, not `/tmp`"), proven by rebooting
   the camera. What used to be open — "will it come back if it dies", "is the
   picture right", "does it survive a reboot" — is answered.
   During the soak, the things worth actually looking at rather than trusting:
   `/tmp/akrtsp_sup.log` should stay empty apart from the one boot line (any
   `starting akrtsp` line means something died); `./tools/aknet.py --watch 15 akrtsp`
   for CPU, `oom_score_adj` and Send-Q on the NVR sockets; load average around
   1.6–2.3 with the NVR attached and no browser, ~2.9 with the page open;
   `./tools/rtsp_probe.py` for fps/GOP/gaps; `./tools/rgbshot.py` for the colour
   signature, since `R−G` drifting positive is the fingerprint of the ISP having
   fallen back to day mode.
   Still genuinely open, in order of usefulness: (a) **UDP transport and RTCP** —
   every real client here is TCP and interleaved, so this is compatibility for
   hypothetical clients, not a fix; (b) **a day/night decision based on the image**
   rather than on a flag, which needs the ISP statistics that `libakispsdk` exports
   without declaring them (see "The purple image") — until then `auto` follows a
   light sensor that is wrong indoors, and the UI says so; (c) **what the IR
   illuminator should do on a schedule** — the gpio is controllable and measured, but
   nothing decides when to touch it.

## 7. Do not touch

* **IR-cut filter.** `/sys/user-gpio/` exposes `IR_LED`, `SPK_PA`, `WHITE_LED`,
  `gpio-ircut_a`, `wifi_en` — note there is no `ircut_b`. On this hardware the
  filter is hold-to-engage on the `a` pin alone with several seconds of travel,
  and releasing the pin parks it half-way with a magenta image. `act=ircut` is
  exposed, one press at a time, and it is the one control worth being careful
  with.
* **`/etc/jffs2` is 700 KB with ~500 KB free.** That is why the Gerge flash copy
  cannot host a card-less UI. Anything added there must be tiny; this UI lives
  on the SD card instead.
* **`cmd_serverd`** — the PTZ daemon is useless without it.
* **`/etc/jffs2/gergesettings.txt`** is not authoritative here. It describes a
  stack that never launches, so editing it changes nothing until `gergehack.sh`
  is actually started.

## 8. Layout

```
anyka-cam/
  README.md              this file
  webui/index.html       the UI
  webui/app.js
  webui/cgi-bin/api      JSON API, ash script
  live/live.conf         preview relay knobs (ports, idle timeout, memory guards)
  live/run_live.sh       on-demand supervisor: guards, restart cap, teardown
  live/stop_live.sh      stop, plus the persistent block/unblock switch
  INSTALL.md             complete install guide for a fresh camera
  NOTICE.md              third-party parts, licenses, what not to expose
  live/go2rtc-armv5      the ARMv5 build this board accepts (see above)
  src/vi_abi.h           the VI channel struct as this libplat_vi.so really reads it
  src/akstream.c         milestone 1: drive capture+encode, dump to a file
  src/akrtsp.c           milestone 2: capture+encode+RTSP server, two channels
  src/run_once.sh        one akstream run, vendor server restored on exit
  src/run_profiles.sh    sweep sizes/rates through akstream
  src/run_gop.sh         the gop<fps SIGFPE reproduction
  src/run_server_test.sh run akrtsp on :8554 with the real clients against it
  src/run_idle_test.sh   akrtsp with no client at all: lifetime and memory
  src/run_takeover.sh    move :554 from the vendor binary to akrtsp; --restore undoes it
                         (AK_ARGS defaults to --mode-file /mnt/camweb/daynight.conf
                         --force-night night: the mode the UI chose wins, the flag is
                         the fallback - the ISP boots in day mode and this room is lit
                         only by IR, see "The purple image")
  src/run_akrtsp.sh      supervisor: restart akrtsp when :554 has no owner, stand down
                         for the vendor server or a handover in flight, fall back to
                         the vendor binary after 3 failed starts, one instance, log to
                         /tmp/akrtsp_sup.log (BIN/LOG/SUP/PIDF/NOFALLBACK from the
                         environment so failure paths can be tested without a live run;
                         BIN defaults to /mnt/camrtsp/akrtsp when that exists, else
                         /tmp/akrtsp - it has to be right in both places)
  src/fake_akrtsp.sh     a server that always fails, for testing the supervisor's
                         restart counting and give-up path safely
  src/run_watch.sh       process CPU from /proc/<pid>/stat over a measured interval
                         (works for akrtsp or the vendor binary; top's %CPU cannot be trusted)
  src/akir_probe.c       read the light sensor and IR-cut thresholds straight off the
                         driver (make irprobe): level 1=day 0=night, day 1000..2000 /
                         night 2..618 here - the measurement that showed the sensor
                         reports DAY in a dark IR-lit room
  tools/rtsp_probe.py    measure an RTSP stream against the wall clock: fps, GOP,
                         seq gaps, RTP timestamps (ffmpeg's -t cannot tell you this)
  tools/rgbshot.py       one real key frame from the live stream, as numbers: mean
                         R/G/B, R-G/B-G, neutral fraction. What separates an IR wash
                         (everything tinted, neutrals included) from swapped chroma
                         (colours rotate, neutrals stay neutral) - and why grabbing
                         "the first frame" instead of a key frame reads as black
  tools/aknet.py         cost and health of the stream server: CPU from
                         /proc/<pid>/stat over a measured interval, oom_score_adj,
                         RSS, Send-Q on the client sockets, wlan0 throughput vs the
                         encoder's own bitrate (--stats, --watch N [comm], --lan)
  tools/akweb.py         who really uses the camera's own UI: parses the CGI action
                         log (/tmp/camweb_req.log), counts live_start/live_stop and
                         PTZ moves per address, and labels this workstation as the
                         operator so test runs are not mistaken for viewers
                         (--once, --watch N, --serve [port])
  tools/cam.py           telnet shell helper
  tools/deploy.py        deploy / verify / promote / revert / stop / --preview
  tools/live.py          install and test the preview relay; --test, --reboot
  tools/night-check.mjs  drive the day/night and IR panel in headless Chrome over
                         CDP: click the page's own buttons and only pass when the
                         camera reports that state back (mode in force, illuminator,
                         filter position), then leave the camera as it was found;
                         logs/night-panel.png
  tools/ppmdiff.py       pixel-wise diff of two rgbshot PPMs: mean |diff| per
                         channel, share of pixels moved, luminance delta. The only
                         way to see an IR illuminator do anything while automatic
                         gain holds the mean brightness still
  tools/browser-check.mjs  drive headless Chrome over CDP: press the page's
                           own button, require a decoded frame and an advancing
                           currentTime, save logs/live-preview.png
  tools/fetch-probe.mjs    run fetch() inside the page: what the relay really
                           hands a browser (this is how the MP4->HLS redirect
                           that curl could not see was found)
  tools/hls-probe.mjs      walk the whole HLS path in-page - playlist, init,
                           segments, then a real MediaSource append
  tools/webrtc-probe.mjs go2rtc WebRTC from Chrome: offer/answer timings,
                         jitter buffer, frames decoded, screenshot
  tools/lv-trace.mjs       run the page's own lvStart() and print the stack and
                           video state every 4 s, for when the chip is too vague
  tools/relay.py         RTSP -> JPEG/MJPEG on a PC, so the UI gets a picture
  tools/loadtest.py      measure the CPU cost of polling this box
  tools/package.sh       build the shareable dist/anyka-cam-*.tar.gz
                         (drops .token/.port/logs/reference, scrubs the token)
  tools/diag.sh          headers, request log, preset JSON as the browser sees it
  tools/.token           access token
  tools/.port            which port ours serves (8080 since promotion)
  pull/                  originals pulled off the camera (vendor content -
                         deliberately NOT in the shared tarball)
  reference/             upstream repo vendored for study
```


Camera side:

```
/mnt/camweb/www/            new docroot (index.html, app.js, cgi-bin/api)
/mnt/camweb/run_web.sh      restart loop for httpd; config.sh starts it at boot
/mnt/camweb/backup/         config.sh.orig + stock_www/ — the undo path, on the card
/mnt/camweb/webui.conf      limits, AUTH_EN, PREVIEW_URL
/mnt/camweb/token           access token
/mnt/camweb/presets/N.conf  recorded positions: "<pan> <tilt> <name>"
/mnt/camweb/live/go2rtc     preview relay binary (only ever runs on demand)
/mnt/camweb/live/live.conf  its knobs: ports, LIVE_IDLE_S, memory guards
/mnt/camweb/live/run_live.sh  supervisor started by the CGI, never at boot
/mnt/camweb/live/stop_live.sh stop + the persistent block switch
/mnt/camweb/live/disable    present = relay blocked (created by --off)

/mnt/camrtsp/akrtsp            our stream server, started at boot (md5-checked)
/mnt/camrtsp/run_akrtsp.sh     its supervisor, started by config.sh
/mnt/camrtsp/run_takeover.sh   handover and --restore, so rollback survives a reboot
/mnt/camweb/daynight.conf      wanted ISP mode: day|night|auto — the UI writes it

/mnt/Factory/config.sh      edited twice: our httpd runner replaced the stock httpd
                            line, and the supervisor replaced `/mnt/Factory/custom/rtsp`
                            (commented, not deleted; the vendor binary is untouched)
/tmp/akrtsp.log             akrtsp's own log          (RAM, cleared on reboot)
/tmp/akrtsp_sup.log         supervisor log            (RAM, cleared on reboot)
/tmp/akrtsp_sup.pid         supervisor pidfile        (RAM)
/tmp/akrtsp.status          what the UI reads: pid, mode in force, mode asked for,
                            light sensor, fps, gop, clients, frame counters (RAM)
/tmp/camweb_web.log         runner log            (RAM, cleared on reboot)
/tmp/camweb_req.log         API request log       (RAM, cleared on reboot)
```
