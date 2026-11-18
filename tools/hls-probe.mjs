#!/usr/bin/env node
/* Step-by-step replay of the page's preview path, inside the page, with the
   error of every step printed. browser-check.mjs answers "does a frame appear?";
   when the answer is no, that alone does not say whether the network, CORS, the
   codec string, or the SourceBuffer is at fault. This walks the same calls the
   page makes - master playlist, media playlist, init segment, three media
   segments, then a real MediaSource append - and prints each result.

     google-chrome-stable --headless=new --remote-debugging-port=9333 \
       --user-data-dir=/tmp/camweb-chrome about:blank &
     ./hls-probe.mjs
*/
const CAM = process.env.CAM_HOST || "192.168.10.46";
const HTTP_PORT = process.env.CAM_HTTP_PORT || "8080";
const RELAY_PORT = process.env.LIVE_PORT || "1984";
const PAGE = `http://${CAM}:${HTTP_PORT}/`;

const target = await (await fetch("http://127.0.0.1:9333/json/new?" + encodeURIComponent(PAGE), { method: "PUT" })).json();
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let id = 0;
const pending = new Map();
ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const send = (method, params = {}) => new Promise((r) => { const my = ++id; pending.set(my, r); ws.send(JSON.stringify({ id: my, method, params })); });

await send("Runtime.enable");
await new Promise((r) => setTimeout(r, 2500));

const expr = `(async function(){
  var out = [], base = "http://${CAM}:${RELAY_PORT}/api/";
  async function get(u, asText) {
    try {
      var r = await fetch(u, { cache: "no-store" });
      var ct = r.headers.get("content-type") || "?";
      if (asText) { var t = await r.text(); out.push([u.replace(base, ""), r.status, ct, t.length]); return t; }
      var b = await r.arrayBuffer();
      out.push([u.replace(base, ""), r.status, ct, b.byteLength]);
      return b;
    } catch (e) { out.push([u.replace(base, ""), "THREW", String(e), 0]); return null; }
  }
  var master = await get(base + "stream.m3u8?src=cam&mp4", true);
  if (!master) return JSON.stringify({ steps: out, error: "no master playlist" });
  var variant = "", codecs = "";
  master.split("\\n").forEach(function (l) {
    l = l.trim();
    if (/^#EXT-X-STREAM-INF/.test(l)) { var m = /CODECS="([^"]+)"/.exec(l); if (m) codecs = m[1]; }
    else if (l && l.charAt(0) !== "#" && !variant) variant = l;
  });
  var vu = base + variant;
  var media = await get(vu, true);
  if (!media) return JSON.stringify({ steps: out, error: "no media playlist" });
  var map = null, segs = [];
  media.split("\\n").forEach(function (l) {
    l = l.trim();
    var m = /^#EXT-X-MAP:URI="([^"]+)"/.exec(l);
    if (m) { map = m[1]; return; }
    if (l && l.charAt(0) !== "#") segs.push(l);
  });
  var dir = vu.slice(0, vu.lastIndexOf("/") + 1);
  var initB = map ? await get(dir + map, false) : null;
  var segB = [];
  for (var i = 0; i < Math.min(3, segs.length); i++) segB.push(await get(dir + segs[i], false));

  // Now the part only a browser can fail: does a real SourceBuffer accept it?
  var ms = new MediaSource(), v = document.createElement("video");
  v.muted = true; v.src = URL.createObjectURL(ms); document.body.appendChild(v);
  var mse = {};
  await new Promise(function (res) { ms.addEventListener("sourceopen", res); setTimeout(res, 6000); });
  mse.sourceopen = ms.readyState;
  try {
    var sb = ms.addSourceBuffer('video/mp4;codecs="' + (codecs || "avc1.4DE028") + '"');
    mse.addSourceBuffer = "ok";
    sb.addEventListener("error", function () { mse.sbError = true; });
    sb.appendBuffer(initB);
    await new Promise(function (res) { sb.addEventListener("updateend", res); setTimeout(res, 4000); });
    mse.afterInit = { updating: sb.updating, buffered: v.buffered.length, dur: ms.duration };
    for (var k = 0; k < segB.length; k++) {
      if (!sb.updating) sb.appendBuffer(segB[k]);
      await new Promise(function (res) { sb.addEventListener("updateend", res); setTimeout(res, 4000); });
    }
    mse.afterSegs = { buffered: v.buffered.length, dur: ms.duration, vw: v.videoWidth, t: +v.currentTime.toFixed(2) };
    if (v.buffered.length) mse.bufferRange = [+v.buffered.start(0).toFixed(2), +v.buffered.end(v.buffered.length-1).toFixed(2)];
    try { var p = v.play(); if (p && p.catch) p.catch(function (e) { mse.playRejected = String(e); }); } catch (e) { mse.playThrew = String(e); }
    await new Promise(function (res) { setTimeout(res, 3000); });
    mse.afterPlay = { vw: v.videoWidth, vh: v.videoHeight, t: +v.currentTime.toFixed(2), rs: v.readyState };
  } catch (e) { mse.threw = String(e); }
  return JSON.stringify({ codecs: codecs, steps: out, mse: mse });
})()`;

const res = await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
if (!res.result || res.result.value === undefined) { console.log("  evaluate failed:", JSON.stringify(res, null, 2)); ws.close(); process.exit(1); }
const val = JSON.parse(res.result.value);
console.log("  codecs:", val.codecs);
console.log("  requests (url, status, content-type, bytes):");
for (const s of val.steps || []) console.log("   ", s.join("  "));
console.log("  media source:", JSON.stringify(val.mse, null, 2).split("\n").join("\n   ") || "none");
ws.close();
