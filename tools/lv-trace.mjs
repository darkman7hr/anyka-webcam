#!/usr/bin/env node
/* Run the page's own live-view code and print the real stack behind the chip.
   The panel reports failures as a one-line chip ("preview lost (...)"), which is
   right for a user and useless for finding a bug. This installs error handlers
   first, calls lvStart() the way the button does, then dumps stacks plus the
   video element's state.

     google-chrome-stable --headless=new --remote-debugging-port=9333 \
       --user-data-dir=/tmp/camweb-chrome about:blank &
     ./lv-trace.mjs
*/
const CAM = process.env.CAM_HOST || "192.168.10.46";
const HTTP_PORT = process.env.CAM_HTTP_PORT || "8080";
const PAGE = `http://${CAM}:${HTTP_PORT}/`;

const target = await (await fetch("http://127.0.0.1:9333/json/new?" + encodeURIComponent(PAGE), { method: "PUT" })).json();
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let id = 0;
const pending = new Map();
ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const send = (method, params = {}) => new Promise((r) => { const my = ++id; pending.set(my, r); ws.send(JSON.stringify({ id: my, method, params })); });
const ev = async (expression) => {
  const r = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
  const v = r.result && r.result.result ? r.result.result.value : undefined;
  return v === undefined ? r : v;
};

await send("Runtime.enable");

/* Wait for app.js to actually exist in this page. The camera serves one CGI at a
   time on one core, so the page can take 15-20 s to come up; calling lvStart()
   before that silently evaluates an undefined function and the trace looks like a
   preview failure when it is really a page that has not loaded. */
for (let i = 0; i < 30; i++) {
  const ready = await ev("(typeof lvStart === 'function' && typeof lv === 'object') ? 'ready' : 'waiting'");
  if (ready === "ready") break;
  await new Promise((r) => setTimeout(r, 2000));
  if (i === 29) { console.log("  app.js never loaded in this page"); ws.close(); process.exit(1); }
}

await ev(`(function(){
  window.__errs = [];
  window.addEventListener("error", function (e) {
    window.__errs.push(String((e.error && e.error.stack) || e.message));
  });
  window.addEventListener("unhandledrejection", function (e) {
    window.__errs.push("rejection: " + String((e.reason && e.reason.stack) || e.reason));
  });
  /* The page reports a broken preview through lvFail(why), and that string is the
     only place the reason exists. Wrap it rather than guessing. */
  var f = window.lvFail;
  if (f) window.lvFail = function (why) { window.__errs.push("lvFail: " + why); return f(why); };
  var a = window.lvAttach;
  if (a) window.lvAttach = function () {
    window.__errs.push("lvAttach: entered, MediaSource=" + (window.MediaSource ? "yes" : "no"));
    try { return a.apply(this, arguments); }
    catch (e) { window.__errs.push("lvAttach threw: " + (e && e.stack || e)); throw e; }
  };
  return "watching";
})()`);

const started = await ev("(function(){ try { lvStart(); return 'called'; } catch (e) { return 'threw: ' + e; } })()");
console.log("  lvStart(): " + started);

const state = `(function(){
  var v = document.getElementById("lv"), c = document.getElementById("lvstat");
  return JSON.stringify({
    chip: c ? c.textContent : "",
    vw: v.videoWidth, vh: v.videoHeight, t: +v.currentTime.toFixed(2), rs: v.readyState,
    buf: v.buffered.length ? [+v.buffered.start(0).toFixed(2), +v.buffered.end(v.buffered.length-1).toFixed(2)] : [],
    paused: v.paused, ended: v.ended, seeking: v.seeking, vol: v.volume,
    vErr: v.error ? (v.error.name + "/" + v.error.code) : "",
    msDur: lv.ms ? String(lv.ms.duration) : "-",
    sbUpd: lv.sb ? lv.sb.updating : "-",
    sbErr: lv.sb ? !!lv.sb.error : "-",
    src: (v.currentSrc || "").slice(0, 24),
    want: lv.want, busy: lv.busy, gen: lv.gen, codec: lv.codec, hlsid: lv.hlsid,
    msState: lv.ms ? lv.ms.readyState : "none", sbState: lv.sb ? "present" : "none",
    errs: (window.__errs || []).slice(-6)
  }, null, 1);
})()`;

for (let i = 1; i <= 14; i++) {
  await new Promise((r) => setTimeout(r, 4000));
  let raw = await ev(state);
  if (typeof raw !== "string") { console.log("  state eval failed: " + JSON.stringify(raw)); break; }
  const s = JSON.parse(raw);
  console.log(`  t=${i * 4}s  ${s.vw}x${s.vh}  currentTime=${s.t}  readyState=${s.rs}  buffered=${JSON.stringify(s.buf)}`);
  console.log(`        chip=${JSON.stringify(s.chip)}  paused=${s.paused} seeking=${s.seeking} ended=${s.ended} vErr=${s.vErr}
        ms=${s.msState} dur=${s.msDur} sbUpdating=${s.sbUpd} sbErr=${s.sbErr} src=${s.src} codec=${s.codec} n/hlsid=${s.hlsid}`);
  if (s.errs.some((e) => !/^lvAttach: entered/.test(e))) {
    console.log("        errors:");
    s.errs.forEach((e) => console.log("          " + e.split("\n").slice(0, 4).join("\n          ")));
    break;
  }
  if (s.vw > 0 && s.t > 0) { console.log("        -> frames are decoding"); break; }
}
ws.close();
