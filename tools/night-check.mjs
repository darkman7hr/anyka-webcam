#!/usr/bin/env node
/* Drive the day/night and IR controls in a real browser, and require the camera
   to report back what it actually did.

   The API can be curled (act=nightmode, act=irled, act=ircut) and that proves
   nothing about the page. What only a browser can fail is what this checks:
   app.js parsing, the panel rendering at all, a click reaching the CGI, and - the
   part that matters for a control like this - whether the label on screen ends up
   matching the hardware. A toggle that shows "on" while the illuminator is off is
   worse than no toggle at all.

     google-chrome-stable --headless=new --remote-debugging-port=9333 \
       --user-data-dir=/tmp/camweb-chrome about:blank &
     ./tools/night-check.mjs [token]

   It leaves the camera as it found it: night mode, illuminator on, filter out.
*/

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CAM = process.env.CAM_HOST || "192.168.10.46";
const PORT = process.env.CAM_HTTP_PORT || "8080";
const PAGE = "http://" + CAM + ":" + PORT + "/";
const CDP = "http://127.0.0.1:9333";
const SHOT = process.env.CAM_SHOT || path.join(HERE, "..", "logs", "night-panel.png");
const TOKEN = process.argv[2] || fs.readFileSync(path.join(HERE, ".token"), "utf8").trim();

let id = 0;
const pending = new Map();
let ws;
const consoleErrors = [];
const results = [];

function send(method, params = {}) {
  return new Promise((resolve, reject) => {
    const msgId = ++id;
    pending.set(msgId, { resolve, reject });
    ws.send(JSON.stringify({ id: msgId, method, params }));
    setTimeout(() => {
      if (pending.has(msgId)) { pending.delete(msgId); reject(new Error(method + " timed out")); }
    }, 30000);
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function check(name, ok, detail) {
  results.push(ok);
  console.log("  " + name.padEnd(44) + " " + (ok ? "PASS" : "FAIL") + "  " + (detail || ""));
}

async function evalJs(expr) {
  const r = await send("Runtime.evaluate", { expression: expr });
  return r && r.result && r.result.value !== undefined ? r.result.value : null;
}

/* Poll until the page reports what we asked for. The labels come from the
   camera's own status endpoint, so "the label changed" means the camera agreed,
   not that a click painted a wish. */
async function until(expr, timeoutMs, what) {
  const stop = Date.now() + timeoutMs;
  let last = null;
  while (Date.now() < stop) {
    last = await evalJs(expr);
    if (last) return last;
    await sleep(1500);
  }
  console.log("     timed out waiting for " + what + "; last saw: " + last);
  return null;
}


async function main() {
  let list;
  try {
    list = await (await fetch(CDP + "/json/list")).json();
  } catch (e) {
    console.log("no chrome debugging port at " + CDP + " - start it first (see the header)");
    return 2;
  }
  let target = list.find((t) => t.type === "page");
  if (!target) target = await (await fetch(CDP + "/json/new?" + encodeURIComponent(PAGE), { method: "PUT" })).json();

  ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pending.has(m.id)) {
      const { resolve, reject } = pending.get(m.id);
      pending.delete(m.id);
      if (m.error) reject(new Error(m.error.message)); else resolve(m.result);
      return;
    }
    if (m.method === "Runtime.exceptionThrown") {
      const d = m.params.exceptionDetails;
      consoleErrors.push("exception: " + (d.exception && d.exception.description ? d.exception.description.split("\n")[0] : d.text));
    } else if (m.method === "Runtime.consoleAPICalled" && m.params.type === "error") {
      consoleErrors.push("console: " + (m.params.args || []).map((a) => a.value || a.description || "").join(" ").slice(0, 160));
    }
  };

  await send("Runtime.enable");
  await send("Page.enable");
  await send("Log.enable");

  await send("Page.navigate", { url: PAGE });
  await sleep(2500);
  await send("Runtime.evaluate", { expression: "localStorage.setItem('camtok', " + JSON.stringify(TOKEN) + ")" });
  await send("Page.navigate", { url: PAGE });
  await sleep(3000);

  /* One CGI at a time on this box and the page polls several endpoints, so app.js
     can take 15-20 s to be live. A click on a button whose handler has not
     arrived looks exactly like a control that does nothing. */
  const ready = await until(
    "(typeof renderNight === 'function' && document.getElementById('ndinfo')) ? 'ready' : null",
    90000, "app.js to load");
  check("page script loaded", ready === "ready", String(ready));

  const info = "document.getElementById('ndinfo').textContent";
  const shown = await until(info + ".indexOf('ISP now:') >= 0 ? " + info + " : null", 40000, "the status poll to fill the panel");
  check("panel shows camera state", !!shown, shown || "");

  const start = (await evalJs("(st.night && st.night.night_mode) || ''")) || "";
  check("starts in a known ISP mode", start === "night" || start === "day", "mode=" + start);

  await evalJs("document.getElementById('ndday').click()");
  const d = await until("(st.night&&st.night.night_mode==='day') ? document.getElementById('ndinfo').textContent : null", 60000, "ISP mode day");
  check("Day button reaches the ISP", !!d, d || "");

  await evalJs("document.getElementById('ndnight').click()");
  const n = await until("(st.night&&st.night.night_mode==='night') ? document.getElementById('ndinfo').textContent : null", 60000, "ISP mode night");
  check("Night button reaches the ISP", !!n, n || "");

  await evalJs("document.getElementById('irbtn').click()");
  const iro = await until("(st.night&&st.night.ir_led==='0') ? document.getElementById('irbtn').textContent : null", 30000, "IR led off");
  check("IR illuminator off", !!iro, iro || "");
  await evalJs("document.getElementById('irbtn').click()");
  const irn = await until("(st.night&&st.night.ir_led==='1') ? document.getElementById('irbtn').textContent : null", 30000, "IR led on");
  check("IR illuminator back on", !!irn, irn || "");

  await evalJs("document.getElementById('cutbtn').click()");
  const cin = await until("(st.night&&st.night.ircut==='1') ? document.getElementById('cutbtn').textContent : null", 40000, "filter in");
  check("IR-cut filter travels in", !!cin, cin || "");
  await evalJs("document.getElementById('cutbtn').click()");
  const cout = await until("(st.night&&st.night.ircut==='0') ? document.getElementById('cutbtn').textContent : null", 40000, "filter out");
  check("IR-cut filter back out", !!cout, cout || "");

  const el = await send("Page.getLayoutMetrics");
  const box = el.cssContentSize || el.contentSize;
  await send("Emulation.setDeviceMetricsOverride", { width: Math.round(box.width), height: 1500, deviceScaleFactor: 1, mobile: false });
  const tall = await send("Page.captureScreenshot", { format: "png", clip: { x: 0, y: 0, width: Math.round(box.width), height: 1500, scale: 1 } });
  fs.writeFileSync(SHOT, Buffer.from(tall.data, "base64"));
  check("panel screenshot saved", fs.existsSync(SHOT), SHOT);

  check("no page errors", consoleErrors.length === 0, consoleErrors.slice(0, 3).join(" | "));
  const final = await evalJs("(st.night && JSON.stringify({mode:st.night.night_mode,asked:st.night.night_set,ir:st.night.ir_led,cut:st.night.ircut})) || 'no state'");
  console.log("\n  left the camera at: " + final);

  const pass = results.filter(Boolean).length;
  console.log(pass + "/" + results.length + " checks passed");
  return pass === results.length ? 0 : 1;
}

main().then((rc) => { try { ws.close(); } catch (e) {} process.exit(rc); })
  .catch((e) => { console.log("failed: " + e.message); try { ws.close(); } catch (x) {} process.exit(1); });
