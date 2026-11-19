"use strict";
/* Anyka AK3918 camera UI. Vanilla JS on purpose: the box has ~12 MB free
   RAM and already runs at a load average near 2. */
var API = "cgi-bin/api";
var tok = localStorage.getItem("camtok") || "";
var st = { pan: null, tilt: null, pmin: 0, pmax: 360, tmin: 10, tmax: 170, step: 10, preview: "", night: {} };
var presets = [], tour = null, tourIx = -1;
var FOV = parseFloat(localStorage.getItem("camfov") || "62");

function $(id) { return document.getElementById(id); }

/* Text for a card line that might not be there. This is not defensive padding:
   one line of poll() referenced $("rtspalt"), an id that no longer exists in
   index.html, and the assignment threw inside the .then - so every line AFTER it
   in the same .then stopped running, on every single poll, forever. That was the
   header (already painted) looking perfectly healthy while the Live relay line,
   the relay links and the relay switch never updated again, and it is why the
   relay switch could not be seen to be wrong from the page: the page never asked
   the camera about it. A missing element now costs its own line and nothing else. */
function $set(id, text) { var e = $(id); if (e) e.textContent = text; }
function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"]/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
  });
}

var msgT;
function msg(t, bad) {
  var m = $("msg");
  m.textContent = t;
  m.className = "msg show" + (bad ? " bad" : "");
  clearTimeout(msgT);
  msgT = setTimeout(function () { m.className = "msg"; }, 2600);
}

function api(params) {
  var q = [];
  for (var k in params) if (params.hasOwnProperty(k)) q.push(k + "=" + encodeURIComponent(params[k]));
  q.push("token=" + encodeURIComponent(tok));
  /* Same URL every poll: without this a browser is free to answer a preset list
     from its own cache, which is how a successful save can look like a failure. */
  q.push("_=" + Date.now());
  return fetch(API + "?" + q.join("&"), { headers: { "X-Token": tok } })
    .then(function (r) {
      return r.text().then(function (body) {
        var j = null;
        try { j = JSON.parse(body); } catch (e) { /* anything but JSON: surfaced below */ }
        /* A parse failure used to reject silently, so a dead endpoint made buttons
           look like they did nothing. Always say something instead. */
        if (!j) { msg("camera answered HTTP " + r.status + " (not JSON)", true); throw new Error("http" + r.status); }
        if (j.ok === 0 && j.err === "unauthorized") {
          localStorage.removeItem("camtok"); tok = ""; showLogin();
          throw new Error("unauthorized");
        }
        return j;
      });
    });
}

/* ---------- token entry (inline modal, no browser dialogs) ---------- */
function showLogin() {
  $("tokin").value = tok || "";
  $("login").style.display = "flex";
  setTimeout(function () { $("tokin").focus(); }, 30);
}
$("tokgo").onclick = function () {
  var t = $("tokin").value.trim();
  if (!t) { msg("token is empty", true); return; }
  tok = t;
  localStorage.setItem("camtok", tok);
  $("login").style.display = "none";
  msg("unlocked");
  poll(); loadPresets();
};
$("tokcancel").onclick = function () { $("login").style.display = "none"; };
$("tokin").addEventListener("keydown", function (e) { if (e.key === "Enter") $("tokgo").click(); });
$("chtok").onclick = showLogin;

/* ---------- status ---------- */
function fmtUp(s) {
  var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + "d " : "") + h + "h " + m + "m";
}

function renderPos() {
  var p = st.pan == null ? "--" : st.pan, t = st.tilt == null ? "--" : st.tilt;
  $("cpos").textContent = p; $("cpos2").textContent = t;
  $("bpos").textContent = p; $("bpos2").textContent = t;
  if (document.activeElement !== $("gpan")) $("gpan").value = st.pan;
  if (document.activeElement !== $("gtilt")) $("gtilt").value = st.tilt;
}

function poll() {
  var ep = lv.epoch;          /* the epoch this reply must match to speak for the relay switch */
  api({ act: "status" }).then(function (j) {
    if (!j || !j.ok) return;
    /* Drawing is wrapped, because the catch below cannot tell "the camera did not
       answer" apart from "this page threw while drawing" - and it treated both as
       a grey dot. The dead $("rtspalt") reference lived in exactly that blind
       spot: it threw on every poll and the page showed a healthy header plus a
       relay card that had frozen. */
    try {
      renderStatus(j, ep);
    } catch (e) {
      msg("page could not draw part of the status: " + ((e && e.message) ? e.message : e), true);
    }
  }).catch(function () { $("live").className = "dot"; });
}

/* Everything the 3 s status reply is allowed to change on screen. Split out of
   poll() so the try/catch above stays one line deep and nothing hides inside it. */
function renderStatus(j, ep) {
  st.pan = j.pan; st.tilt = j.tilt;
  st.pmin = j.pan_min; st.pmax = j.pan_max; st.tmin = j.tilt_min; st.tmax = j.tilt_max;
  if (j.step) st.step = j.step;
  if (String(st.preview) !== String(j.preview_url || "")) { st.preview = j.preview_url || ""; setupPreview(); }
  renderPos();
  $("live").className = "dot on";
  $set("ip", j.ip || location.hostname);
  $set("uptime", "up " + fmtUp(j.uptime || 0));
  $set("load", "load " + j.load);
  $set("mem", j.mem_free + " MB free of " + j.mem_total + " MB");
  $set("wifi", (j.link || "--").replace("|", " dB  /  ") + " dBm");
  $set("kern", "Linux " + (j.fw || ""));
  var host = j.ip || location.hostname;
  $set("rt1", "rtsp://" + host + ":554/vs0");
  $set("rt2", "rtsp://" + host + ":554/vs1");
  renderNight(j);
  /* There used to be a third RTSP line here, written to $("rtspalt"). That id
     is not in index.html, so this assignment threw and took the relay update
     below it down with it, on every poll, indefinitely. */
  if (typeof j.live_port !== "undefined") lvPollStatus(j, ep);
}

/* ---------- recorded positions ----------
   Names are typed in the card itself and delete is a two-click arm, because
   browser dialog functions are unreliable here (in-app browsers return null,
   which earlier made "Save" look completely dead even though the camera saved). */
var editing = null, armed = null, armT;

function slotOf(i) {
  for (var k = 0; k < presets.length; k++) if (String(presets[k].slot) === String(i)) return presets[k];
  return null;
}

function renderPresets() {
  var el = $("presets"), html = "", used = 0, slots = {};
  presets.forEach(function (p) { slots[p.slot] = p; used++; });
  for (var i = 1; i <= 12; i++) {
    var p = slots[i];
    if (editing === i) {
      var at = (st.pan == null ? "?" : st.pan) + "\u00b0 / " + (st.tilt == null ? "?" : st.tilt) + "\u00b0";
      html += '<div class="pset set edit"><div class="nm">Slot ' + i + " \u2190 " + at + "</div>"
        + '<input type="text" id="pnm" maxlength="24" placeholder="name this position" value="' + esc(p ? (p.name || "") : "") + '">'
        + '<div class="acts"><button class="btn sm pri" data-store="' + i + '">Store</button>'
        + '<button class="btn sm" data-cancel="1">Cancel</button></div></div>';
      continue;
    }
    if (p) {
      html += '<div class="pset set"><div class="nm">' + esc(p.name || ("Slot " + i)) + "</div>"
        + '<div class="co">' + p.pan + "&deg; / " + p.tilt + "&deg;</div>"
        + '<div class="acts"><button class="btn sm pri" data-go="' + i + '">Go</button>'
        + '<button class="btn sm" data-edit="' + i + '">Save</button>'
        + '<button class="btn sm' + (armed === i ? " arm" : "") + '" data-del="' + i + '">'
        + (armed === i ? "Sure?" : "&times;") + "</button></div></div>";
    } else {
      html += '<div class="pset"><div class="nm">Slot ' + i + '</div><div class="co">empty</div>'
        + '<div class="acts"><button class="btn sm" data-edit="' + i + '">Save</button></div></div>';
    }
  }
  el.innerHTML = html;
  $("pcount").textContent = used + " of 12 saved";
  if (editing) { var f = $("pnm"); if (f) { f.focus(); f.select(); } }
}

function loadPresets() {
  api({ act: "presets" })
    .then(function (j) { if (j && j.ok) { presets = j.presets || []; renderPresets(); } })
    .catch(function () { });
}

$("presets").addEventListener("click", function (e) {
  var b = e.target.closest("button"); if (!b) return;
  var d = b.dataset;
  if (d.go) {
    api({ act: "preset_goto", slot: d.go }).then(function (j) {
      if (j && j.ok) {
        msg("Going to " + (j.name || "slot " + d.go));
        st.pan = j.pan; st.tilt = j.tilt; renderPos();
      } else msg((j && j.err) || "failed", true);
    }).catch(function () { });
  } else if (d.edit) {
    editing = parseInt(d.edit, 10); armed = null; renderPresets();
  } else if (d.cancel) {
    editing = null; renderPresets();
  } else if (d.store) {
    var inp = $("pnm");
    api({ act: "preset_save", slot: d.store, name: inp ? inp.value : "" }).then(function (j) {
      editing = null;
      if (j && j.ok) { msg("Saved \u201c" + j.name + "\u201d @ " + j.pan + "\u00b0/" + j.tilt + "\u00b0"); loadPresets(); }
      else msg("save failed: " + ((j && j.err) || "unknown"), true);
    }).catch(function () { editing = null; renderPresets(); });
  } else if (d.del) {
    var slot = parseInt(d.del, 10);
    if (armed !== slot) {
      armed = slot; renderPresets();
      clearTimeout(armT);
      armT = setTimeout(function () { armed = null; renderPresets(); }, 4000);
      msg("click again to clear slot " + slot);
      return;
    }
    armed = null;
    api({ act: "preset_del", slot: d.del }).then(function () { loadPresets(); msg("Slot cleared"); }).catch(function () { });
  }
});

$("presets").addEventListener("keydown", function (e) {
  if (e.target.id !== "pnm") return;
  if (e.key === "Enter") { var b = document.querySelector("[data-store]"); if (b) b.click(); }
  if (e.key === "Escape") { editing = null; renderPresets(); }
});

/* ---------- movement ---------- */
function sendMove(dir) {
  api({ act: "ptz", dir: dir, step: st.step }).then(function (j) {
    if (j && j.ok) { st.pan = j.pan; st.tilt = j.tilt; renderPos(); }
  });
}

var repeatT = null, repeatI = null;
function startRepeat(dir) {
  stopRepeat();
  sendMove(dir);
  repeatT = setTimeout(function () {
    repeatI = setInterval(function () { sendMove(dir); }, 420);
  }, 460);
}
function stopRepeat() { clearTimeout(repeatT); clearInterval(repeatI); repeatT = repeatI = null; }

Array.prototype.forEach.call(document.querySelectorAll(".pad .btn"), function (b) {
  var dir = b.dataset.dir;
  b.addEventListener("pointerdown", function (e) { e.preventDefault(); b.classList.add("held"); startRepeat(dir); });
  b.addEventListener("pointerup", function () { b.classList.remove("held"); stopRepeat(); });
  b.addEventListener("pointerleave", function () { b.classList.remove("held"); stopRepeat(); });
  b.addEventListener("contextmenu", function (e) { e.preventDefault(); });
});
window.addEventListener("blur", stopRepeat);

$("gobtn").onclick = function () {
  var p = parseInt($("gpan").value, 10), t = parseInt($("gtilt").value, 10);
  if (isNaN(p) || isNaN(t)) return msg("Enter both pan and tilt", true);
  api({ act: "goto", pan: p, tilt: t }).then(function (j) {
    if (j && j.ok) {
      msg("Moving to " + j.pan + "\u00b0 / " + j.tilt + "\u00b0");
      st.pan = j.pan; st.tilt = j.tilt; renderPos();
    } else msg((j && j.err) || "failed", true);
  });
};
$("homebtn").onclick = function () { api({ act: "home" }); msg("Homing sent"); };

$("stepsel").addEventListener("click", function (e) {
  var b = e.target.closest("button"); if (!b) return;
  Array.prototype.forEach.call(this.querySelectorAll("button"), function (x) { x.classList.remove("on"); });
  b.classList.add("on");
  st.step = parseInt(b.dataset.s, 10);
});
/* ---------- drag on the image to aim ---------- */
var dragging = false, lastX = 0, lastY = 0, accP = 0, accT = 0, lastSend = 0;
var stage = $("stage");

stage.addEventListener("pointerdown", function (e) {
  if (st.pan == null) return;
  dragging = true; lastX = e.clientX; lastY = e.clientY; accP = 0; accT = 0;
  try { stage.setPointerCapture(e.pointerId); } catch (err) { }
});
stage.addEventListener("pointermove", function (e) {
  if (!dragging) return;
  var r = stage.getBoundingClientRect();
  accP += (e.clientX - lastX) / r.width * FOV;
  accT += (e.clientY - lastY) / r.height * FOV * 0.56;
  lastX = e.clientX; lastY = e.clientY;
  var now = Date.now();
  if (now - lastSend < 260 || (Math.abs(accP) < 2 && Math.abs(accT) < 2)) return;
  lastSend = now;
  var p = Math.round(st.pan + accP), t = Math.round(st.tilt + accT);
  p = Math.max(st.pmin, Math.min(st.pmax, p));
  t = Math.max(st.tmin, Math.min(st.tmax, t));
  accP = 0; accT = 0;
  api({ act: "goto", pan: p, tilt: t }).then(function (j) {
    if (j && j.ok) { st.pan = j.pan; st.tilt = j.tilt; renderPos(); }
  });
});
["pointerup", "pointercancel"].forEach(function (ev) {
  stage.addEventListener(ev, function () { dragging = false; });
});

/* ---------- preview ---------- */
var pvBusy = false, pvFail = 0, pvTimer = null, pvRetry = null;

/* A local override wins over what the camera has in webui.conf, so the relay
   address can be changed per browser without touching the camera. */
function pvUrl() {
  if (localStorage.getItem("campreview") !== null) return (localStorage.getItem("campreview") || "").trim();
  return (st.preview || "").trim();
}

/* What the preview box says when there is no JPEG source.
 *
 * No embedded player can help here, VLC included: a page may not open a raw TCP
 * socket, and RTSP is raw TCP on port 554. That is a browser rule, not a VLC or
 * plugin gap (the old NPAPI VLC plugin died with Chrome 45 / Firefox 52). Any
 * in-page video therefore needs something on the network to repackage the H.264
 * first. Until that exists, the honest affordance is to hand the URL to a real
 * player app on the device doing the viewing. */
function pvOffPanel() {
  var host = location.hostname || "192.168.10.46";
  var url = pvUrl();
  var txt = "<b>No stream in this page.</b><br>"
    + "A browser cannot open RTSP &mdash; pages are not allowed raw TCP sockets &mdash; so "
    + "the camera's H.264 has to be converted by something on your network before any "
    + "player can be embedded. VLC's browser plugin is long dead, and a WASM build would "
    + "hit the same socket rule.";
  if (url) txt += "<br><br><code>" + esc(url) + "</code> is not answering. Retrying every 5 s.";
  txt += '<div class="row" style="margin-top:12px">'
    + '<a class="btn sm pri" href="rtsp://' + esc(host) + ':554/vs0">Open main in VLC</a>'
    + '<a class="btn sm" href="rtsp://' + esc(host) + ':554/vs1">Open sub in VLC</a>'
    + "</div>"
    + '<div class="hint" style="margin-top:8px">Opens in the player app installed on this '
    + "device &mdash; VLC, mpv, MX Player and similar handle <code>rtsp://</code>. "
    + "main = 1280&times;720, sub = 640&times;480 at ~75 kbit/s.</div>";
  $("pvoff").innerHTML = txt;
}

function setupPreview() {
  var url = pvUrl();
  if (pvTimer) { clearInterval(pvTimer); pvTimer = null; }
  if (!url) { pvOffPanel(); showPvOff(); return; }
  if ($("pvurl")) $("pvurl").textContent = url;
  pvOffPanel();
  pvFail = 0;
  tickPv();
  if (!pvTimer) pvTimer = setInterval(tickPv, 1200);
}
function showPvOff() {
  $("pv").style.display = "none"; $("xh").style.display = "none"; $("pvoff").style.display = "";
  if (pvRetry) clearTimeout(pvRetry);
  pvRetry = setTimeout(function () { pvFail = 0; tickPv(); }, 5000);
}
function showPvOn() { $("pvoff").style.display = "none"; $("pv").style.display = ""; $("xh").style.display = ""; }
function tickPv() {
  var url = pvUrl();
  if (!url || pvBusy) return;
  pvBusy = true;
  var img = $("pv"), sep = url.indexOf("?") >= 0 ? "&" : "?";
  img.onload = function () { pvBusy = false; pvFail = 0; showPvOn(); };
  img.onerror = function () { pvBusy = false; if (++pvFail >= 2) showPvOff(); };
  img.src = url + sep + "t=" + Date.now();
}

/* ---------- in-page live preview: the camera's own on-demand relay ----------
 *
 * The page cannot open RTSP (no raw TCP sockets in a browser), and the camera
 * has no business decoding its own 720p stream on a 432 MHz ARM926. So:
 *
 *   1. act=live_start asks the camera to launch go2rtc from the card. It is not
 *      started at boot and not part of the vendor boot script; the CGI is the
 *      only thing that starts it, behind the same token as every PTZ command.
 *   2. go2rtc ingests the vendor server's own /vs1 (640x480; /vs0 at 720p
 *      overloaded this board) and re-serves it on :1984 - as WebRTC, and as
 *      HLS/fMP4 over HTTP. Its config sets api.origin="*", which is what lets a
 *      page served on :8080 read a port on :1984 at all.
 *   3. The page prefers WebRTC: first frame in ~8 s, ~0.24 s of jitter buffer,
 *      and no queue that can fall behind. HLS segments through MediaSource is
 *      the fallback for browsers or relays without WebRTC; the viewer's machine
 *      does the decoding either way.
 *   4. act=live_beat every 8 s is the relay's idle timer. Stop beating and the
 *      relay shuts itself down a few minutes later, so nothing lingers on 36 MB
 *      of RAM because somebody left a tab open.
 *
 * go2rtc's MP4 consumer has a reproducible panic when a consumer disconnects
 * (pkg/mp4/consumer.go:82 -> pkg/core/writebuffer.go:37). The camera's
 * supervisor restarts the relay when that happens and the reconnect loop here
 * re-attaches after a backoff, so the viewer sees a hitch, not a dead page. */
var lv = { want: false, playing: false, abort: null, ms: null, sb: null, url: "",
           timer: null, tries: 0, port: 1984, codec: "", idle: 300,
           busy: false, gen: 0, hlsid: "", joined: false,
           pc: null, mode: "",
           /* The relay switch. lv.off is what the camera last told us; lv.epoch
              says which replies may still speak for it; lv.mutating is set while
              a click's request is in the air; lv.last is the last complete relay
              object we were given, so a failed click can still repaint the truth.
              The switch used to be read back out of the button's own label - see
              lvInfo for what that cost. */
           off: false, epoch: 0, mutating: false };



function lvHost() { return location.hostname || "192.168.10.46"; }
function lvBase() { return "http://" + lvHost() + ":" + (lv.port || 1984); }
function mb(kb) { return Math.round((kb || 0) / 102.4) / 10; }

function lvChip(t, cls) {
  var c = $("lvstat");
  c.textContent = t;
  c.style.color = cls === "bad" ? "var(--bad)" : (cls === "ok" ? "var(--ok)" : "");
}

/* Feed the Camera card from any live_* reply: they all answer the same object.
   <auth> means "this reply is the newest word on the switch". It is true for the
   answer to a click and false for the periodic ones.

   That distinction is the whole point. The switch is a file on the card, so the
   page must follow the camera and not its own last click - but a status poll that
   left the page BEFORE a click can land AFTER that click's own reply and print
   the old word back over it. The page used to read the switch's state out of the
   button's label and decide what to send from that, so one such stale reply was
   enough to make the button stuck: every later click then asked for the state the
   camera was already in, and nothing ever came back. The CGI made the window
   wider still by holding the camera's one-connection web server for about 3 s
   while it stopped the relay, which guaranteed polls in flight. Both are fixed
   here and in cgi-bin/api: the state lives in lv.off, and lv.epoch decides who
   is allowed to write it. */
function lvInfo(j, auth) {
  lv.idle = j.idle_s || lv.idle;
  lv.port = j.api_port || lv.port;
  if (auth) lv.off = !!j.off;
  var s = $("lvinfo");
  if (s) {
    if (lv.off) s.textContent = "blocked on this camera (Relay: blocked)";
    else if (!j.running) s.textContent = "idle - starts when a viewer asks (idle timeout " + lv.idle + " s)";
    else s.textContent = "running (pid " + j.pid + ", " + mb(j.rss_kb) + " MB) - " + j.state;
  }
  if ($("lvrtsp")) $("lvrtsp").textContent = lv.off ? "--" : (j.rtsp_out || "--");
  if ($("lvui")) {
    var p = lv.off ? "" : (j.page || (lvBase() + "/"));
    $("lvui").href = p || "#";
    $("lvui").textContent = p || "--";
  }
  var t = $("lvtoggle");
  /* While a click is in the air the button shows what it is doing, not what it
     was. Repainting it from a reply that arrives mid-flight is how it came to
     lie about the state it was in the middle of changing. The button also starts
     out disabled and saying "reading": the page may not print a switch state it
     has not been told. */
  if (t && !lv.mutating) {
    t.textContent = lv.off ? "Relay: blocked" : "Relay: allowed";
    t.disabled = false;
  }
}

/* The ordinary status poll carries the relay's coarse state, so the Camera card
   stays truthful without a second request per poll. lvInfo() refines it with
   pid/RSS whenever a live_* endpoint answers.
   <ep> is lv.epoch as it was when this poll was sent: if the switch has been
   touched since, this reply is stale and gets no say about it. */
function lvPollStatus(j, ep) {
  lv.port = j.live_port || lv.port;
  var host = j.ip || location.hostname || "192.168.10.46";
  if (!lv.mutating && ep === lv.epoch) lv.off = !!j.live_off;
  var t = $("lvtoggle");
  if (t && !lv.mutating) {
    t.textContent = lv.off ? "Relay: blocked" : "Relay: allowed";
    t.disabled = false;
  }
  var s = $("lvinfo");
  /* Also refreshed while a preview is wanted, and that is not cosmetic: after the
     "Stop relay now" button (or the idle reaper, or a crash) the card used to keep
     saying "running (pid 17846, 11.8 MB)" indefinitely, because the refresh was
     gated on !lv.want. A panel that names a PID that no longer exists is a lie. */
  if (s && (!lv.want || !j.live_running)) {
    if (lv.off) s.textContent = "blocked on this camera (Relay: blocked)";
    else if (j.live_running) s.textContent = "running now - see the Live view panel";
    else s.textContent = "idle - starts when a viewer asks";
  }
  if ($("lvrtsp")) $("lvrtsp").textContent = lv.off ? "--" : "rtsp://" + host + ":" + (j.live_rtsp_port || 8554) + "/cam";
  if ($("lvui")) {
    var p = lv.off ? "" : "http://" + host + ":" + (j.live_port || 1984) + "/";
    $("lvui").href = p || "#";
    $("lvui").textContent = p || "--";
  }
  if (!j.live_running && lv.want && lv.playing) lvChip("relay stopped on the camera", "bad");
}

/* One relay mutation in flight at a time. Both the switch and "Stop relay now"
   go through here, because on this camera each of them is a forked CGI on a
   one-core box and two of them at once have no defined order. The buttons are
   disabled rather than merely ignored so a second click cannot look like an
   ignored one, and lv.epoch is bumped on the way out so anything still in the
   air from before the click cannot speak for the switch afterwards. */
function lvMutate(act) {
  if (lv.mutating) return null;
  lv.mutating = true;
  lv.epoch++;
  $("lvtoggle").disabled = true;
  $("lvoffbtn").disabled = true;
  /* Starting a preview is held too: if it were allowed while a block was in the
     air, the viewer could launch a supervisor that the block's teardown - which
     only chases the pids it saw at entry - would not stop, leaving a relay
     running behind a switch that says blocked. */
  $("lvbtn").disabled = true;
  return api({ act: act }).then(
    function (j) {
      lv.mutating = false;
      lv.epoch++;
      $("lvtoggle").disabled = false;
      $("lvoffbtn").disabled = false;
      $("lvbtn").disabled = false;
      return j;
    },
    function () {
      lv.mutating = false;
      lv.epoch++;
      $("lvtoggle").disabled = false;
      $("lvoffbtn").disabled = false;
      $("lvbtn").disabled = false;
      return null;
    });
}

function lvShow() {

  $("pvoff").style.display = "none";
  $("pv").style.display = "none";
  $("lv").style.display = "";
  $("xh").style.display = "";
}

function lvTeardown() {
  /* gen is bumped on every teardown and every async step checks it. Without
     this, a retry timer and a beat-triggered reconnect can run at once: the
     second attempt points the <video> at a new MediaSource, the first one gets
     detached, its source buffer fires "abort", that attempt calls lvFail, which
     tears down the attempt that was actually working. Two things chasing the
     same <video> on this box always lose. */
  lv.gen++;
  lv.busy = false;
  if (lv.abort) { try { lv.abort.abort(); } catch (e) { /* already gone */ } lv.abort = null; }
  try { if (lv.sb && lv.sb.updating) lv.sb.abort(); } catch (e) { /* mid-append, fine */ }
  try { if (lv.ms && lv.ms.readyState === "open") lv.ms.endOfStream(); } catch (e) { /* not open */ }
  if (lv.pc) { try { lv.pc.close(); } catch (e) { /* already closed */ } lv.pc = null; }
  var v = $("lv");
  v.pause();
  v.removeAttribute("src");
  v.srcObject = null;          // WebRTC attaches a MediaStream, not an object URL
  try { v.load(); } catch (e) { /* nothing was loaded */ }
  if (lv.url) { try { URL.revokeObjectURL(lv.url); } catch (e) { /* already revoked */ } lv.url = ""; }
  lv.ms = null; lv.sb = null; lv.playing = false;
  v.style.display = "none";
}


/* Leave this preview. Deliberately does NOT stop the relay: another viewer may
 * be watching the same camera, and the camera's idle timer reclaims the memory
 * a few minutes after the last heartbeat anyway. "Stop relay now" in the Camera
 * card is the explicit, everyone-at-once version. */
function lvLeave() {
  lv.want = false;
  if (lv.timer) { clearInterval(lv.timer); lv.timer = null; }
  lvTeardown();
  resetPoll();               // back to the normal 3 s status cadence

  $("lvbtn").textContent = "Start live preview";
  lvChip("left - relay idles out in ~" + lv.idle + " s");
  /* Put the stage back the way setupPreview() left it, rather than leaving a
     black box: with no JPEG preview URL configured that is the "no preview
     source" panel again, which is also where the VLC links live. */
  setupPreview();
}


function lvStart() {
  if (lv.busy) return;          // one attempt at a time; see lvTeardown's gen note
  lv.want = true;
  lv.busy = true;
  lv.tries = 0;
  resetPoll();


  $("lvbtn").textContent = "Starting...";
  lvChip("starting relay on the camera...");
  api({ act: "live_start" }).then(function (j) {
    if (!j || !j.ok) {
      lv.want = false;
      $("lvbtn").textContent = "Start live preview";
      lvChip(j && j.err ? j.err : "relay refused", "bad");
      if (j && j.err) msg(j.err, true);
      return;
    }
    lvInfo(j, true);          /* this reply is the camera's current word on the switch */
    if (!lv.timer) lv.timer = setInterval(lvBeat, 8000);
    if (!j.started && j.running) { lvUp(0); return; }
    // live_start answers at once - on a one-CPU box with a one-connection
    // httpd, a request that polls for the port holds up the whole page. So the
    // page asks "is it up?" the same cheap way it asks everything else.
    lvWaitUp(0);
  }).catch(function () {
    lv.want = false;
    $("lvbtn").textContent = "Start live preview";
    lvChip("could not reach the camera API", "bad");
  });
}

function lvUp(ms) {
  $("lvbtn").textContent = "Stop live preview";
  lvChip("relay up in " + ms + " ms, connecting...");
  lvConnect();
}

function lvWaitUp(tries) {
  if (!lv.want) return;
  if (tries > 30) {
    lv.want = false;
    lv.busy = false;
    $("lvbtn").textContent = "Start live preview";

    api({ act: "live_status" }).then(function (s) {
      lvChip("relay did not come up: " + ((s && s.state) || "unknown"), "bad");
    });
    return;
  }
  setTimeout(function () {
    api({ act: "live_status" }).then(function (j) {
      if (!lv.want) return;
      if (j && j.ok) {
        lvInfo(j, false);     /* a periodic read: it may be stale, so no say on the switch */
        if (j.running) { lvUp(tries * 1500); return; }
        // A supervisor that is alive but has no relay has already given its
        // reason in state; showing it beats a bare timeout.
        if (j.sup && j.state && /refused|failed|missing|not listening|disabled/.test(j.state)) {
          lv.want = false;
          lv.busy = false;
          $("lvbtn").textContent = "Start live preview";

          lvChip("camera refused the relay: " + j.state, "bad");
          return;
        }
      }
      lvChip("waiting for the relay on the camera (" + (tries * 1.5).toFixed(0) + " s)...");
      lvWaitUp(tries + 1);
    }).catch(function () { lvWaitUp(tries + 1); });
  }, 1500);
}


function lvBeat() {
  if (!lv.want) return;
  api({ act: "live_beat" }).then(function (j) {
    if (!j || !j.ok) return;
    lvInfo(j, false);         /* a periodic read: it may be stale, so no say on the switch */
    if (!j.running) {
      lvChip("relay stopped on the camera (" + (j.state || "?") + ")", "bad");
      if (lv.want) { lv.playing = false; setTimeout(function () { if (lv.want) lvStart(); }, 1500); }
      return;
    }
    lvChip("live - relay " + mb(j.rss_kb) + " MB, camera " + mb(j.avail_kb || j.free_kb) + " MB available", "ok");
    // Stay near the live edge: a stalled or hidden tab must not buffer the
    // camera's stream for ten minutes out of spite.
    var v = $("lv");
    if (lv.playing && v.buffered && v.buffered.length) {
      var end = v.buffered.end(v.buffered.length - 1);
      if (end - v.currentTime > 6) { try { v.currentTime = Math.max(v.currentTime, end - 0.7); } catch (e) { /* seek denied */ } }
    }
  }).catch(function () { /* transient; the next beat, or lvFail, covers it */ });
}

/* The codec string comes from the relay rather than being guessed: the vendor
   stream is H.264 Main/L4.0 (avc1.4DE028) and addSourceBuffer throws on anything
   it cannot decode, so asking is cheaper than being wrong. */
function lvCodecProbe() {
  return fetch(lvBase() + "/api/stream.m3u8?src=cam&_=" + Date.now()).then(function (r) {
    if (!r.ok) throw new Error("m3u8 http " + r.status);
    return r.text();
  }).then(function (t) {
    var m = /CODECS="([^"]+)"/.exec(t);
    lv.codec = m ? m[1] : "avc1.4d401f";
    return lv.codec;
  });
}

function lvAttach() {
  var MS = window.ManagedMediaSource || window.MediaSource;
  if (!MS) { lv.busy = false; throw new Error("no MediaSource in this browser"); }
  var gen = lv.gen;
  lv.joined = false;
  var ms = new MS();
  lv.ms = ms;
  lv.url = URL.createObjectURL(ms);
  return new Promise(function (res, rej) {
    ms.addEventListener("sourceopen", function () {
      if (gen !== lv.gen) { rej(new Error("superseded")); return; }
      try {
        var sb = ms.addSourceBuffer('video/mp4;codecs="' + lv.codec + '"');
        lv.sb = sb;
        sb.addEventListener("error", function () { if (gen === lv.gen) lvFail("source buffer error"); });
        sb.addEventListener("abort", function () { if (gen === lv.gen) lvFail("source buffer aborted"); });
        res();
      } catch (e) { rej(e); }
    });
    /* Order matters, and this comment exists because the wrong order deadlocks
       silently: sourceopen only fires once a media element attaches to the
       MediaSource, so the object URL has to be on the <video> before waiting.
       Do NOT call v.load() after assigning src - that re-runs the loading
       algorithm, detaches the MediaSource that has just opened, and the source
       buffer answers with "abort". Assigning src is already a load. */
    var v = $("lv");
    v.src = lv.url;
    setTimeout(function () { rej(new Error("sourceopen timed out")); }, 8000);
  }).then(function () {
    if (gen !== lv.gen) return;
    lvShow();
    lv.playing = true;
    var p = $("lv").play();
    if (p && p.catch) p.catch(function () { /* autoplay policy; the button press is the gesture */ });
    return lvHls(gen);
  });
}



function lvWaitIdle() {
  return new Promise(function (res) {
    var sb = lv.sb;
    if (!sb || !sb.updating) return res();
    var done = function () { sb.removeEventListener("updateend", done); res(); };
    sb.addEventListener("updateend", done);
  });
}

/* One long fMP4 response, appended to MediaSource chunk by chunk. appendBuffer
   while the buffer is updating throws, hence lvWaitIdle. */
/* The preview is fed by go2rtc's HLS variant. Two reasons this is the right path
   rather than a workaround.

   1. It is forced: go2rtc 301-redirects every WebKit-family User-Agent (Chrome,
      headless Chrome, Safari) away from /api/stream.mp4 to
      /api/stream.m3u8?...&mp4, and fetch() follows that silently. The direct MP4
      URL therefore hands a 93-byte #EXTM3U playlist to the SourceBuffer and the
      video element never reports a frame - it showed up as "relay closed the
      stream" with buffered.length stuck at 0. User-Agent cannot be overridden
      from a page. tools/fetch-probe.mjs proved it by running fetch() inside the
      page instead of trusting curl (curl and Firefox do get real MP4).

   2. It is the better format anyway. Measured on the camera: the master playlist
      points at hls/playlist.m3u8?id=..., which carries an fMP4 init segment
      (674 B, ftyp+moov) and media segments (moof+mdat, 20-70 kB, 0.5 s apart) -
      exactly what a SourceBuffer takes. The segment index is in the URL
      (?id=...&n=N), so the page pulls them in order without re-reading the
      playlist every time.

   The camera only muxes and serves bytes; the H.264 decode happens in the
   browser, which is where it belongs on a 36 MB single-core box. */
/* Every fetch here carries the abort controller, so lvTeardown can cut a preview
   off cleanly instead of leaving segment requests running against the relay. */
function lvOpts() {
  if (window.AbortController && !lv.abort) lv.abort = new AbortController();
  var o = { cache: "no-store" };   /* not lvOpts(): a global find-and-replace once
                                      made this call itself, and the preview died
                                      with "Maximum call stack size exceeded" */
  if (lv.abort) o.signal = lv.abort.signal;
  return o;
}

function lvHls(gen) {
  if (!lv.want || gen !== lv.gen) return Promise.resolve();
  return fetch(lvBase() + "/api/stream.m3u8?src=cam&mp4", lvOpts()).then(function (r) {
    if (!r.ok) throw new Error("master playlist http " + r.status);
    if (!/mpegurl/.test(r.headers.get("content-type") || "")) {
      throw new Error("relay answered " + (r.headers.get("content-type") || "unknown") + ", not a playlist");
    }
    return r.text();
  }).then(function (txt) {
    var variant = "";
    txt.split("\n").forEach(function (ln) {
      ln = ln.trim();
      if (/^#EXT-X-STREAM-INF/.test(ln)) {
        var m = /CODECS="([^"]+)"/.exec(ln);
        if (m && !lv.codec) lv.codec = m[1];
      } else if (ln && ln.charAt(0) !== "#" && !variant) {
        variant = ln;
      }
    });
    if (!variant) throw new Error("empty master playlist");
    return lvPull(gen, lvBase() + "/api/" + variant);
  });
}

function lvPull(gen, variantUrl) {
  var dir = variantUrl.slice(0, variantUrl.lastIndexOf("/") + 1);
  var q = /[?&]id=([^&]+)/.exec(variantUrl);
  lv.hlsid = q ? q[1] : "";
  var initUri = null, n = 0, miss = 0, tries = 0;

  function grabInit() {
    if (!initUri) return Promise.resolve();
    return fetch(dir + initUri, lvOpts()).then(function (r) {
      if (!r.ok) throw new Error("init segment http " + r.status);
      return r.arrayBuffer();
    }).then(function (buf) {
      if (gen !== lv.gen || !lv.sb) return;
      return lvWaitIdle().then(function () {
        if (gen !== lv.gen || !lv.sb || lv.sb.updating) return;
        lv.sb.appendBuffer(new Uint8Array(buf));
      });
    });
  }

  function resync() {
    return fetch(variantUrl, lvOpts()).then(function (r) {
      if (!r.ok) throw new Error("media playlist http " + r.status);
      return r.text();
    }).then(function (txt) {
      var last = null, first = null;
      txt.split("\n").forEach(function (ln) {
        ln = ln.trim();
        var m = /^#EXT-X-MAP:URI="([^"]+)"/.exec(ln);
        if (m) { initUri = m[1]; return; }
        if (ln && ln.charAt(0) !== "#") {
          var q2 = /[?&]n=(\d+)/.exec(ln);
          if (q2) { var nv = parseInt(q2[1], 10); if (first === null) first = nv; last = nv; }
        }
      });
      if (last === null) return;
      /* Second attempt onward joins at the live edge. Starting at the head of the
         window is right on a fresh relay and wrong on one that has been running a
         while: go2rtc keeps only a short window, so the head can already be gone,
         and joining there means timing out through the miss budget before the
         first frame - the 20-40 s of 0x0 a long-running session used to show. */
      if (lv.joined || tries > 1) {
        // We have frames already and drifted: jump to the live edge.
        n = last;
      } else {
        /* First join takes the OLDEST segment the relay is still publishing.
           Joining at the newest one is what made a preview sit at 0:00 with a
           fraction of a second buffered: segments start wherever the encoder
           pleases, so the newest one usually begins mid-GOP and the decoder has
           nothing it can start from until a later keyframe. Starting at the head
           of the window plays immediately, and lvFollowLive() pulls the view
           back to the edge as soon as it is running. */
        n = first !== null ? first : last;
      }
      tries++;
      return grabInit();
    });
  }

  function append(buf) {
    if (gen !== lv.gen || !lv.sb) return Promise.resolve();
    return lvWaitIdle().then(function () {
      if (gen !== lv.gen || !lv.sb || lv.sb.updating) return;
      try {
        lv.sb.appendBuffer(new Uint8Array(buf));
      } catch (e) {
        // Out of buffered room: drop what is behind the play head rather than
        // killing a preview that is otherwise working.
        try {
          var v = $("lv");
          if (v.buffered && v.buffered.length) lv.sb.remove(0, Math.max(0, v.currentTime - 2));
        } catch (e2) { /* updating; the next segment will fit */ }
      }
      lvFollowLive();
    });
  }

  function pull() {
    if (gen !== lv.gen || !lv.want || !lv.hlsid) return;
    fetch(dir + "segment.m4s?id=" + lv.hlsid + "&n=" + n, lvOpts()).then(function (r) {
      if (!r.ok) {
        /* A few misses just means the encoder has not produced this one yet.
           Many means our index drifted outside the relay's window, so resync. */
        miss++;
        if (miss > 8) {
          miss = 0;
          resync().then(function () { setTimeout(pull, 200); }).catch(function (e) {
            if (gen === lv.gen) lvFail("hls resync: " + e.message);
          });
          return;
        }
        setTimeout(pull, 250);
        return;
      }
      return r.arrayBuffer().then(function (buf) {
        return append(buf).then(function () {
          n++; miss = 0; lv.joined = true;
          /* Always hand the next pull to a timer. Returning a fresh promise from
             each then() and looping that way builds a chain deep enough that V8
             throws "Maximum call stack size exceeded" - which is exactly what a
             long-running preview hit. */
          setTimeout(pull, 50);
        });
      });
    }).catch(function (e) {
      if (gen === lv.gen) lvFail("hls: " + e.message);
    });
  }

  return resync().then(pull);
}

/* Keep the play head near the live edge. A preview that quietly falls 20 s
   behind is worse than none: it shows the past and reads as a freeze. */
function lvFollowLive() {
  var v = $("lv");
  if (!v || !v.buffered || !v.buffered.length) return;
  var end = v.buffered.end(v.buffered.length - 1);
  if (end - v.currentTime > 2.5) { try { v.currentTime = end - 0.6; } catch (e) { /* mid-seek */ } }
}

/* ---------- WebRTC first, HLS as the fallback -------------------------
   Measured on this camera against go2rtc 1.9.15 with tools/webrtc-probe.mjs:
   offer gathered in 0.14 s, answer in 4.0 s (go2rtc waits for a key frame from
   the vendor encoder before it answers), first frame 8.0 s after the press,
   640x480, jitter buffer 235 ms, 103 frames received / 97 decoded / 0 dropped,
   0 packets lost, 0 NACKs - and currentTime advanced 6.00 s in 6 s of wall
   clock.

   That last number is the reason this path exists. The HLS path below pulls
   0.5 s segments one at a time from a single core that is simultaneously
   encoding, serving the page and answering CGIs. When a pull takes longer than
   the segment lasts, the buffer falls behind and nothing on either side pulls it
   back, so "live" quietly drifts to a minute or more behind reality. A WebRTC
   receiver renders what arrives; it has no queue to fall behind.

   Video only, deliberately: the camera's audio is G.711 a-law (PCMA), which
   Chrome's WebRTC stack cannot decode, and offering it changes the negotiation
   for nothing. */
function lvWebrtc(gen) {
  if (!window.RTCPeerConnection) {
    return Promise.reject(new Error("no RTCPeerConnection in this browser"));
  }
  return new Promise(function (resolve, reject) {
    var pc = new RTCPeerConnection({ iceServers: [] });   // LAN: no STUN needed
    lv.pc = pc;
    var done = false, frameTimer = null, guard = null;

    function fin(err) {
      if (done) return;
      done = true;
      clearTimeout(guard);
      if (frameTimer) clearInterval(frameTimer);
      if (err) {
        try { pc.close(); } catch (e) { /* already closed */ }
        if (lv.pc === pc) lv.pc = null;
        reject(err);
      } else {
        resolve(pc);
      }
    }

    guard = setTimeout(function () { fin(new Error("webrtc timed out")); }, 30000);

    pc.oniceconnectionstatechange = function () {
      var s = pc.iceConnectionState;
      if (!done && (s === "failed" || s === "closed")) { fin(new Error("ice " + s)); return; }
      /* Once running, a dead ICE is a lost preview, not a failed connect: go
         through lvFail so the backoff/retry logic and the gen guard apply. */
      if (done && s === "disconnected" && gen === lv.gen && lv.want) lvFail("ice disconnected");
    };

    pc.ontrack = function (e) {
      var v = $("lv");
      var ms = new MediaStream();
      ms.addTrack(e.track);
      v.srcObject = ms;
      lvShow();
      var p = v.play();
      if (p && p.catch) p.catch(function () { /* autoplay policy; the button press is the gesture */ });
      /* "Connected" is not "picture". Resolve on the first real decoded frame,
         the same bar browser-check.mjs holds the HLS path to. */
      frameTimer = setInterval(function () {
        if (gen !== lv.gen) { fin(new Error("superseded")); return; }
        if (v.videoWidth > 0) fin(null);
      }, 400);
    };

    pc.addTransceiver("video", { direction: "recvonly" });

    pc.createOffer()
      .then(function (o) { return pc.setLocalDescription(o); })
      .then(function () {
        /* go2rtc's API takes the whole offer and answers with a complete SDP -
           no trickle - so the candidates have to be gathered before the POST.
           Capped: host candidates appear immediately on a LAN and waiting for
           "complete" forever is what a slow relay would punish. */
        return new Promise(function (res) {
          if (pc.iceGatheringState === "complete") { res(); return; }
          var t = setTimeout(res, 2500);
          pc.onicegatheringstatechange = function () {
            if (pc.iceGatheringState === "complete") { clearTimeout(t); res(); }
          };
        });
      })
      .then(function () {
        if (gen !== lv.gen) throw new Error("superseded");
        return fetch(lvBase() + "/api/webrtc?src=cam", {
          method: "POST",
          body: pc.localDescription.sdp,
          headers: { "content-type": "application/sdp" },
          cache: "no-store"
        });
      })
      .then(function (r) {
        /* go2rtc answers 201, not 200 - "== 200" here reports a working relay
           as broken, which is exactly what the first probe did. */
        if (r.status < 200 || r.status >= 300) throw new Error("relay answered webrtc http " + r.status);
        return r.text();
      })
      .then(function (sdp) {
        if (gen !== lv.gen) throw new Error("superseded");
        return pc.setRemoteDescription({ type: "answer", sdp: sdp });
      })
      .catch(function (e) { fin(new Error(e && e.message ? e.message : String(e))); });
  });
}

function lvConnect() {
  /* No busy check here: whoever calls this already owns the single flight
     (lvStart -> lvWaitUp -> lvUp -> here, or the visibility handler going
     through lvStart). lvStart is the only entry that claims the flight. */
  var gen = lv.gen;
  var t0 = Date.now();
  lvWebrtc(gen).then(function () {
    if (gen !== lv.gen) return;
    lv.mode = "webrtc";
    lv.playing = true;
    lv.busy = false;
    lv.tries = 0;
    lvChip("live (WebRTC) - picture in " + ((Date.now() - t0) / 1000).toFixed(1) + " s", "ok");
  }).catch(function (e) {
    if (gen !== lv.gen || e.message === "superseded") return;
    /* Fallback, not a failure: an older relay build, a browser without
       RTCPeerConnection, or a network that blocks UDP still gets the segment
       path. It is the slower one, which is why it is the fallback. */
    lv.mode = "hls";
    lvChip("webrtc unavailable (" + e.message + ") - using HLS segments", "");
    lvCodecProbe().then(lvAttach).catch(function (e2) {
      if (gen === lv.gen && e2.message !== "superseded") lvFail(e2.message);
    });
  });
}


function lvFail(why) {
  if (!lv.want) return;
  lv.playing = false;
  lv.tries++;
  if (lv.tries > 8) {
    lvChip("preview keeps failing (" + why + ") - giving up", "bad");
    lvLeave();
    return;
  }
  var dly = Math.min(15000, 1000 * lv.tries);
  lvChip("preview lost (" + why + ") - retry " + lv.tries + " in " + Math.round(dly / 1000) + " s", "bad");
  lvTeardown();          // clears busy, bumps gen
  setTimeout(function () { if (lv.want) lvStart(); }, dly);
}


$("lvbtn").onclick = function () { if (lv.want) lvLeave(); else lvStart(); };

$("lvoffbtn").onclick = function () {
  /* Leave the preview first. "Stop relay now" is an explicit request to free the
     camera, but the page still had lv.want set, so its 8 s beat saw running=0 and
     politely started the relay straight back up - the button and the page fought,
     and a status check a minute later showed running=1 with state=stopped. */
  if (lv.mutating) return;
  var self = this, was = self.textContent;
  lvLeave();
  var p = lvMutate("live_stop");
  if (!p) return;
  self.textContent = "Stopping...";
  p.then(function (j) {
    self.textContent = was;
    if (!j || !j.ok) { msg("relay still up - the camera did not answer the stop request", true); return; }
    lvInfo(j, true);
    lvChip("relay stopped - camera freed", "ok");
    msg("relay stopped on the camera");
  });
};

/* The switch. Which way to go is decided from lv.off - what the camera last told
   us - and NOT from the word printed on the button. That was the bug that made it
   one-way: a status poll that left before the click could land after the click's
   reply and print the old word back, and from then on every click asked for the
   state the camera was already in, so the switch never came back. The camera also
   used to take ~3 s to answer, holding its one-connection web server and
   guaranteeing such a poll was in flight; it answers in about a second now. */
$("lvtoggle").onclick = function () {
  if (lv.mutating) return;
  var self = this, block = !lv.off;
  var p = lvMutate(block ? "live_off" : "live_on");
  if (!p) return;
  self.textContent = block ? "Blocking..." : "Allowing...";
  p.then(function (j) {
    if (!j || !j.ok) {
      /* Say what failed, and put the button back to the last state the camera
         actually reported. A silent return here is what made this look dead. */
      self.textContent = lv.off ? "Relay: blocked" : "Relay: allowed";
      msg((j && j.err) ? j.err : "the camera did not answer the relay switch", true);
      return;
    }
    lvInfo(j, true);
    if (lv.off) { if (lv.want) lvLeave(); msg("relay blocked on this camera until re-enabled", true); }
    else msg("relay allowed - it still only runs while a viewer asks");
  });
};

/* A backgrounded tab stops beating, so the camera takes the memory back even if
   the tab is never closed; coming back restarts the preview by itself. */
document.addEventListener("visibilitychange", function () {
  if (document.hidden) {
    if (lv.timer) { clearInterval(lv.timer); lv.timer = null; }
  } else if (lv.want && !lv.timer) {
    lv.timer = setInterval(lvBeat, 8000);
    lvBeat();
    if (!lv.playing) lvStart();     // lvStart is the guarded entry point
  }

});

/* ---------- camera panel ---------- */

/*
 * Day/night and IR. Three separate controls because they are three separate
 * things on this board, and the page has to show what is true rather than what
 * was last clicked:
 *   night_set   what was asked for (the control file on the card)
 *   night_mode  what the ISP is actually in, reported by akrtsp itself
 * If they differ, the change is in flight (akrtsp polls every 5 s) - or the
 * stream server is not running, which the line says out loud.
 */
/*
 * Light bar for the header chip, built from the gain the ISP's auto-exposure loop
 * is spending. Gain, not luminance, is what carries the light information on this
 * board: AE holds its luminance target (about 50) whatever the room does, and what
 * changes is the price it pays to get there - measured about 4.5k with the room
 * light on, 12.5k in day mode and pegged at its 16384 ceiling in night mode with
 * the room dark. Log scale, because the useful range is a factor of four and the
 * peg at the top is a cliff.
 */
function lightbar(gain) {
  var g = Math.max(1, gain || 1);
  var darkish = (Math.log(g) - Math.log(2000)) / (Math.log(16384) - Math.log(2000));
  var n = Math.round((1 - darkish) * 10);
  var s = "", i;
  if (n < 0) n = 0;
  if (n > 10) n = 10;
  for (i = 0; i < 10; i++) s += (i < n ? "#" : ".");
  return s;
}

function kfmt(v) {
  return v >= 1000 ? (v / 1000).toFixed(1) + "k" : String(v);
}

function renderNight(j) {
  var b, gain = -1, lumi = -1;
  st.night = j;

  /*
   * The header light chip. The hardware light sensor behind j.ldr was measured
   * reading 1 (day) with the room light switched on and off, so it cannot be shown
   * as "the light"; the gain the ISP is spending can, and it is the same number the
   * auto policy in akrtsp decides on.
   */
  if (typeof j.ae_again === "number" && j.ae_again > 0) gain = j.ae_again;
  if (typeof j.ae_lumi === "number" && j.ae_lumi >= 0) lumi = j.ae_lumi;
  b = $("lightval");
  if (b) b.textContent = (gain < 0 ? "n/a" : kfmt(gain) + " " + lightbar(gain));

  if (!$("ndinfo")) return;                 // page without this panel

  var set = j.night_set || "unset";
  Array.prototype.forEach.call([["ndday", "day"], ["ndnight", "night"], ["ndauto", "auto"]],
    function (p) { b = $(p[0]); if (b) b.className = "btn sm" + (set === p[1] ? " pri" : ""); });

  var ldr = (j.ldr === 1) ? "day" : (j.ldr === 0 ? "night" : "n/a");
  var led = (j.ir_led === "1") ? "on" : (j.ir_led === "0" ? "off" : "?");
  var cut = (j.ircut === "1") ? "in" : (j.ircut === "0" ? "out" : "?");
  var note = "";
  if (!j.akrtsp) note = " - stream server not running";
  else if (set !== "unset" && set !== "auto" && set !== j.night_mode) note = " - applying";
  var auto = "";
  if (set === "auto") {
    auto = "  |  auto: " + (j.auto_state || "-");
    if (j.auto_reason && j.auto_reason !== "-")
      auto += " (" + String(j.auto_reason).replace(/_/g, " ") + ")";
    if (j.auto_gain > 0) auto += " @ gain " + j.auto_gain;
  }
  $("ndinfo").textContent = "asked: " + set + "  |  ISP now: "
    + (j.night_mode || "unset") + auto
    + "  |  ae gain " + (j.ae_again === undefined ? "?" : j.ae_again)
    + "  |  scene " + (lumi < 0 ? "n/a" : lumi + "/255")
    + "  |  exp " + (j.ae_exp === undefined ? "?" : j.ae_exp)
    + "  |  IR led: " + led + "  |  filter: " + cut + note;
  $("ndinfo").title = "the board's own light sensor reports " + ldr
    + "; it does not respond on this unit and is not used for anything";

  b = $("irbtn"); if (b) b.textContent = "IR illuminator: " + led;
  b = $("cutbtn"); if (b) b.textContent = "IR-cut filter: " + cut;
}

function nightSet(mode) {
  api({ act: "nightmode", state: mode }).then(function (j) {
    if (!j || !j.ok) { msg((j && j.err) || "image mode change failed", true); return; }
    st.night = j;
    renderNight(j);
    msg("image mode set to " + mode + " - the ISP switches within a few seconds");
  });
}
$("ndday").onclick = function () { nightSet("day"); };
$("ndnight").onclick = function () { nightSet("night"); };
$("ndauto").onclick = function () {
  nightSet("auto");
  /* Say what auto actually looks at, because the obvious guess is wrong on this
     board: the light sensor reads DAY whether the room light is on or off, and
     trusting it is what produced the magenta picture. auto follows the ISP's
     auto-exposure gain instead, and the visible sign of that is the illuminator
     going out for a few seconds while the camera checks for ambient light. */
  msg("auto now follows the ISP auto-exposure gain (this board's light sensor does not respond); " +
      "the IR illuminator blinks off briefly while it probes for ambient light");
};

$("irbtn").onclick = function () {
  var on = st.night.ir_led === "1";
  api({ act: "irled", state: on ? "off" : "on" }).then(function (j) {
    if (!j || !j.ok) { msg((j && j.err) || "illuminator change failed", true); return; }
    st.night = j;
    renderNight(j);
    msg("IR illuminator " + (j.ir_led === "1" ? "on" : "off"));
  });
};

$("cutbtn").onclick = function () {
  var inPos = st.night.ircut === "1";
  api({ act: "ircut", state: inPos ? 0 : 1 }).then(function (j) {
    if (!j || !j.ok) { msg((j && j.err) || "filter change failed", true); return; }
    /* The CGI waits for the motor and reports whether the node actually reached
       the position, so "moving" is not guessed here. A filter that was commanded
       and did not travel is a black picture when the illuminator is on, and the
       page must say so instead of showing the position that was asked for. */
    if (j.moved === 0)
      msg("IR-cut filter did NOT reach " + (inPos ? "out" : "in") +
          " - the command was accepted and the motor did not move", true);
    else
      msg("IR-cut filter " + (inPos ? "out" : "in"));
    /* The motor is slower than the response: refresh after it has arrived, so
       the label cannot show a position the filter is not in yet. */
    setTimeout(poll, 3500);
  });
};



var ledOn = false;
$("ledbtn").onclick = function () {
  ledOn = !ledOn;
  api({ act: "led", state: ledOn ? 1 : 0 });
  this.textContent = "White LED " + (ledOn ? "on" : "off");
};
$("shotbtn").onclick = function () {
  var u = (localStorage.getItem("campreview") || st.preview || "").trim();
  if (!u) return msg("No preview URL set - a snapshot needs the preview source", true);
  var url = u.replace(/[?&]t=\d+/, "") + (u.indexOf("?") >= 0 ? "&" : "?") + "t=" + Date.now();
  fetch(url).then(function (r) {
    if (!r.ok) throw new Error("HTTP " + r.status);
    return r.blob();
  }).then(function (b) {
    if (!b.size) throw new Error("empty frame");
    var d = new Date(), p = function (n) { return (n < 10 ? "0" : "") + n; };
    var a = document.createElement("a");
    a.href = URL.createObjectURL(b);
    a.download = "cam-" + d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate())
      + "-" + p(d.getHours()) + p(d.getMinutes()) + p(d.getSeconds()) + ".jpg";
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 5000);
    msg("snapshot saved, " + b.size + " bytes");
  }).catch(function (e) {
    msg("snapshot failed: " + e.message, true);
  });
};
$("pvsave").onclick = function () {
  var u = $("pvset").value.trim();
  localStorage.setItem("campreview", u);
  st.preview = u;
  setupPreview();
  msg("Saved in this browser. For every browser, set PREVIEW_URL in /mnt/camweb/webui.conf");
};
$("tourbtn").onclick = function () {
  if (tour) {
    clearInterval(tour); tour = null;
    $("tourbtn").textContent = "Start patrol"; $("tourbtn").classList.remove("pri");
    return;
  }
  var have = presets.slice().sort(function (a, b) { return a.slot - b.slot; });
  if (!have.length) return msg("Save at least one position first", true);
  var sec = Math.max(3, parseInt($("toursec").value, 10) || 12);
  tourIx = -1;
  $("tourbtn").textContent = "Stop patrol";
  $("tourbtn").classList.add("pri");
  var stepTour = function () {
    tourIx = (tourIx + 1) % have.length;
    api({ act: "preset_goto", slot: have[tourIx].slot }).then(function (j) {
      if (j && j.ok) {
        st.pan = j.pan; st.tilt = j.tilt; renderPos();
        msg("Patrol: " + (j.name || have[tourIx].name));
      }
    });
  };
  stepTour();
  tour = setInterval(stepTour, sec * 1000);
};
var rebootArmed = false, rebootT;
$("rebootbtn").onclick = function () {
  var b = this;
  if (!rebootArmed) {
    rebootArmed = true;
    b.textContent = "Sure? click again";
    clearTimeout(rebootT);
    rebootT = setTimeout(function () { rebootArmed = false; b.textContent = "Reboot camera"; }, 4000);
    return;
  }
  rebootArmed = false;
  b.textContent = "Reboot camera";
  msg("Rebooting\u2026");
  api({ act: "reboot" }).catch(function () { });
};
document.addEventListener("click", function (e) {
  var c = e.target.closest(".copy"); if (!c) return;
  var t = c.textContent;
  if (navigator.clipboard) navigator.clipboard.writeText(t).then(function () { msg("Copied"); });
  else msg(t);
});

/* ---------- boot ---------- */
if (!tok) showLogin();
renderPresets();
poll();
loadPresets();

/* The box runs at 100% CPU with nothing of ours running (measured: ~100 jiffies
   per second at HZ=100, i.e. one full core, and a single status request can take
   4 s to serve). busybox httpd serves one CGI at a time, so an eager poller
   here is not free: back off to 3 s, and stop polling entirely when the tab is
   in the background. Presets are refetched on demand instead of on a timer. */
var pollTimer = null;
function startPoll() { if (!pollTimer) pollTimer = setInterval(poll, lv.want ? 8000 : 3000); }

/* While a live view is running the page slows its own status poll from 3 s to
   8 s. Every poll is a forked CGI running awk/sed on a one-core 36 MB box, and
   go2rtc wants that core too: with four concurrent starts plus a decoding
   browser the camera stopped answering on :8080 for about a minute. The live
   beat already carries the relay's state, so nothing is lost. */
function resetPoll() {
  if (pollTimer) clearInterval(pollTimer);
  pollTimer = setInterval(poll, lv.want ? 8000 : 3000);
}

function stopPoll() { if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } }
document.addEventListener("visibilitychange", function () {
  if (document.hidden) stopPoll(); else { poll(); startPoll(); }
});
startPoll();

