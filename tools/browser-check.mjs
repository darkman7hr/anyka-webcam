#!/usr/bin/env node
/* Verify the in-page live preview in a real browser, end to end.

   tools/live.py checks the camera side. What it cannot check is the part only a
   browser can fail: cross-origin rules between :8080 (the page) and :1984 (the
   relay), MediaSource accepting the codec, and whether a decoded frame actually
   lands on screen. This drives headless Chrome over CDP, presses the page's own
   button, waits for real pixels, and saves a screenshot.

     google-chrome-stable --headless=new --remote-debugging-port=9333 \
       --user-data-dir=/tmp/camweb-chrome about:blank &
     ./browser-check.mjs [token]

   Exit 0 means: the page loaded, the relay started from the page's own request,
   the video element reported a real frame size, and currentTime kept advancing.
*/

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CAM = process.env.CAM_HOST || "192.168.10.46";
const HTTP_PORT = process.env.CAM_HTTP_PORT || "8080";
const PAGE = "http://" + CAM + ":" + HTTP_PORT + "/";
const CDP = "http://127.0.0.1:9333";
const SHOT = process.env.CAM_SHOT || path.join(HERE, "..", "logs", "live-preview.png");
const TOKEN = process.argv[2] || fs.readFileSync(path.join(HERE, ".token"), "utf8").trim();

let id = 0;
const pending = new Map();
let ws;

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
const results = [];
const consoleErrors = [];
function check(name, ok, detail) {
  results.push(ok);
  console.log("  " + name.padEnd(42) + " " + (ok ? "PASS" : "FAIL") + "  " + (detail || ""));
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
  if (!target) {
    target = await (await fetch(CDP + "/json/new?" + encodeURIComponent(PAGE), { method: "PUT" })).json();
  }

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
      const desc = d.exception && d.exception.description ? d.exception.description.split("\n")[0] : d.text;
      consoleErrors.push("exception: " + desc);
    } else if (m.method === "Runtime.consoleAPICalled" && m.params.type === "error") {
      consoleErrors.push("console: " + (m.params.args || []).map((a) => a.value || a.description || "").join(" ").slice(0, 160));
    }
  };

  await send("Runtime.enable");
  await send("Page.enable");
  await send("Log.enable");

  /* The token lives in localStorage for this origin and the page reads it at
     load, so: load, store the token, load again. That is what a first-time
     visitor does by typing it into the token dialog. */
  await send("Page.navigate", { url: PAGE });
  await sleep(2500);
  await send("Runtime.evaluate", { expression: "localStorage.setItem('camtok', " + JSON.stringify(TOKEN) + ")" });
  await send("Page.navigate", { url: PAGE });
  await sleep(3000);

  const head = await send("Runtime.evaluate", {
    expression: "JSON.stringify({ver:(document.getElementById('uiver')||{}).textContent," +
      "hasBtn:!!document.getElementById('lvbtn'),hasVideo:!!document.getElementById('lv')})",
  });
  const h = JSON.parse(head.result.value);
  check("page is the new UI", h.hasBtn && h.hasVideo, JSON.stringify(h));

  const stateExpr = "(function(){var v=document.getElementById('lv'),c=document.getElementById('lvstat');" +
    "return JSON.stringify({chip:c?c.textContent:'',vw:v.videoWidth,vh:v.videoHeight," +
    "t:+v.currentTime.toFixed(2),rs:v.readyState,paused:v.paused," +
    "buf:v.buffered.length?+v.buffered.end(v.buffered.length-1).toFixed(2):0});})()";
  const ev = async () => JSON.parse((await send("Runtime.evaluate", { expression: stateExpr })).result.value);

  /* Wait for app.js to be live before pressing the button. On a one-core box with
     one CGI at a time the page's own script can take 15-20 s to arrive, and a
     click into a button with no handler is indistinguishable from a broken
     preview - which is exactly the false failure this check used to report. */
  const readyExpr = "(typeof lvStart === 'function' && typeof lv === 'object') ? 'ready' : ('waiting:' + document.readyState)";
  let ready = "waiting";
  for (let i = 0; i < 45; i++) {
    const r = await send("Runtime.evaluate", { expression: readyExpr });
    ready = r && r.result && r.result.value !== undefined ? r.result.value : "waiting";
    if (ready === "ready") break;
    if (i % 5 === 4) console.log("     page not ready yet: " + ready);
    await sleep(2000);
  }
  check("page script loaded before pressing", ready === "ready", ready);

  await send("Runtime.evaluate", { expression: "document.getElementById('lvbtn').click()" });
  console.log("  pressed the page's Start live preview button; waiting for pixels...");

  let first = null, second = null, chip = "";
  const deadline = Date.now() + 150000;
  while (Date.now() < deadline) {
    await sleep(4000);
    const s = await ev();
    chip = s.chip;
    console.log("   chip=" + JSON.stringify(s.chip) + "  " + s.vw + "x" + s.vh +
      "  t=" + s.t + "  readyState=" + s.rs + "  buffered=" + s.buf);
    if (s.vw > 0 && s.t > 0) {
      first = s;
      await sleep(4000);
      second = await ev();
      break;
    }
  }
  check("video decoded in the page", !!first && first.vw >= 640, first ? first.vw + "x" + first.vh : "no frame");
  check("playback advancing (live, not stalled)", !!second && second.t > first.t,
    first && second ? first.t + " -> " + second.t + " s" : "");
  const info = await send("Runtime.evaluate", { expression:
    "(function(){var e=document.getElementById('lvinfo');return e?e.textContent:''})()" });
  const infoTxt = info && info.result && info.result.value !== undefined ? info.result.value : "";
  check("UI says the relay is running", /running \(pid/.test(infoTxt), infoTxt);
  check("no page errors", consoleErrors.length === 0, consoleErrors.slice(0, 3).join(" | "));

  fs.mkdirSync(path.dirname(SHOT), { recursive: true });
  const shot = await send("Page.captureScreenshot", { format: "png" });
  fs.writeFileSync(SHOT, Buffer.from(shot.data, "base64"));
  console.log("  screenshot: " + SHOT);

  /* Leave the camera as it was found, and exercise the stop path through the
     page rather than by killing something behind its back. */
  await send("Runtime.evaluate", { expression: "document.getElementById('lvoffbtn').click()" });
  /* The card's own text only changes on the next status poll, which on this box
     can be several seconds away - reading it once made a working stop button look
     broken. Poll it. */
  let afterTxt = "";
  for (let i = 0; i < 10; i++) {
    await sleep(3000);
    const afterInfo = await send("Runtime.evaluate", { expression:
      "(function(){var e=document.getElementById('lvinfo');return e?e.textContent:''})()" });
    afterTxt = afterInfo && afterInfo.result && afterInfo.result.value !== undefined ? afterInfo.result.value : "";
    if (/idle|blocked/.test(afterTxt)) break;
  }
  const after = await ev();
  /* Assert on the camera, not on a span of text: the page's wording only updates
     on the next status poll, which can be many seconds away on this box. If the
     relay's port is closed, the stop button did its job. */
  let relayGone = false;
  for (let i = 0; i < 10; i++) {
    try {
      const r = await fetch(`http://${CAM}:1984/api/streams`, { signal: AbortSignal.timeout(3000) });
      if (!r.ok) relayGone = true;
    } catch (e) { relayGone = true; break; }
    await sleep(3000);
  }
  check("stop button freed the camera (port closed)", relayGone, afterTxt || after.chip);
  check("page says the relay is idle", /idle|blocked/.test(afterTxt), afterTxt);

  const bad = results.filter((r) => !r).length;
  console.log("\n" + (results.length - bad) + "/" + results.length + " browser checks passed");
  ws.close();
  return bad ? 1 : 0;
}

main().then((rc) => process.exit(rc)).catch((e) => { console.log("error: " + e.message); process.exit(2); });

