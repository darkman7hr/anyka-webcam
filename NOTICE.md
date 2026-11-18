# Notices, third-party code and licenses

## What this package contains

| part | origin |
|---|---|
| `webui/` — the camweb UI (`index.html`, `app.js`, `cgi-bin/api`) | written for this camera, as-is |
| `live/` — `live.conf`, `run_live.sh`, `stop_live.sh` | written for this camera, as-is |
| `live/go2rtc-armv5` | **go2rtc v1.9.15** (commit `7c05066`), (c) Alexey Akchurin / AlexxIT, MIT — cross-compiled here for `GOARCH=arm GOARM=5 CGO_ENABLED=0` because the upstream `linux/arm` asset does not run on this ARMv5 board |
| `tools/` — `cam.py`, `deploy.py`, `live.py`, `relay.py`, `loadtest.py`, `diag.sh`, `package.sh`, the `.mjs` browser checks | written for this camera, as-is |
| `pull/` (**not shipped**) | files copied off the running camera (its `config.sh`, the stock page, `ptz.ini`) — vendor firmware content, kept in the author's tree for study and the revert path, deliberately left out of the archive |

## go2rtc

go2rtc is MIT licensed; the full license is in the upstream repository:
<https://github.com/AlexxIT/go2rtc> (v1.9.15, tag commit
`7c05066c90ab6ad1fccb506d5f0408aa31ebe60f`). The binary shipped in `live/` was
built from that tag with:

```
CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=5 go build -trimpath -ldflags "-s -w" -o go2rtc-armv5 .
```

and is unmodified apart from the build flags. Nothing in this package links
against it; it is started as a separate process.

## The SD-card hack

This UI only works on a camera that already carries the AK3918 SD-card hack. That
work is not mine and is not included here — see the upstream project:
<https://github.com/openhwproject/anyka3918-gc1084-camera>. The `reference/`
directory of the working tree (not shipped in the tarball) is a vendored copy of
that repository, kept for study; take it from upstream rather than from a copy.

## Vendor content

The originals taken off the camera's flash are the manufacturer's copyright, so
they are **not** in this archive. The revert path does not need them: `--promote`
saved the real `config.sh` and the whole stock docroot on the card itself, in
`/mnt/camweb/backup/`, which is where `--revert` reads them from.

## Your own responsibilities

* The token in `/mnt/camweb/token` is the only thing between this PTZ-capable
  camera and anyone on the LAN. It is generated per install and is **not** in
  this package. Treat the camera as LAN-trusted: no TLS, no rate limit, plain
  HTTP. Do not expose it.
* Modifying the boot script (`/mnt/Factory/config.sh`, which `--promote` does
  with a saved backup) can brick the interface. The undo path is in
  `INSTALL.md` section 11 — check it works before you depend on the camera.
* The camweb UI, CGIs, shell scripts and Python/JS tools are provided as-is,
  with no warranty and no explicit license grant from the author yet. If you
  want to redistribute or sell a derivative, settle that with the author first.
