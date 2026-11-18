#!/usr/bin/env node
/* One question: when the camera page itself fetches the relay's fragmented MP4,
   does data actually arrive? live.py proves the bytes with curl; the page uses
   fetch() from a different origin. If bytes arrive here but the MediaSource stays
   empty, the fault is in app.js. If the body ends at once, the fault is on the
   relay side (or in what the browser sends).

     google-chrome-stable --headless=new --remote-debugging-port=9333 \
       --user-data-dir=/tmp/camweb-chrome about:blank &
     ./fetch-probe.mjs
*/
const CAM = process.env.CAM_HOST || "192.168.10.46";
const HTTP_PORT = process.env.CAM_HTTP_PORT || "8080";
const RELAY_PORT = process.env.LIVE_PORT || "1984";
const PAGE = `http://${CAM}:${HTTP_PORT}/`;

const list = await (await fetch("http://127.0.0.1:9333/json/list")).json();
const target = await (await fetch("http://127.0.0.1:9333/json/new?" + encodeURIComponent(PAGE), { method: "PUT" })).json();
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });

let id = 0;
const pending = new Map();
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
};
const send = (method, params = {}) => new Promise((r) => {
  const my = ++id;
  pending.set(my, r);
  ws.send(JSON.stringify({ id: my, method, params }));
});

await send("Runtime.enable");
await new Promise((r) => setTimeout(r, 2500));

const expr = `(async function(){
  var url = "http://${CAM}:${RELAY_PORT}/api/stream.mp4?src=cam&_=" + Date.now();
  var out = {url: url};
  try {
    var r = await fetch(url);
    out.status = r.status;
    out.type = r.type;
    out.headers = {};
    r.headers.forEach(function (v, k) { out.headers[k] = v; });
    var rd = r.body.getReader(), total = 0, chunks = 0, head = null, t0 = Date.now();
    while (Date.now() - t0 < 6000) {
      var o = await rd.read();
      if (o.done) { out.done = true; break; }
      total += o.value.length; chunks++;
      if (!head) head = Array.prototype.slice.call(o.value.slice(0, 24));
    }
    out.bytes = total; out.chunks = chunks;
    out.head = head ? String.fromCharCode.apply(null, head.filter(function (c) { return c >= 32 && c < 127; })) : "";
    if (!out.done) { try { rd.cancel(); } catch (e) {} }
  } catch (e) { out.error = String(e); }
  return JSON.stringify(out);
})()`;

const res = await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
console.log("  in-page fetch of the relay stream:");
console.log("   " + JSON.stringify(res.result && res.result.value ? JSON.parse(res.result.value) : res, null, 2).split("\n").join("\n   "));
ws.close();
