#!/usr/bin/env node
/* Prove the relay switch is a switch and not a one-way door.

   This control failed in a way curling could never show: the camera moved the
   file both ways correctly, while the page printed a word that no longer matched
   it, so every later click asked for the state the camera was already in and the
   switch looked stuck. The cause was two-fold - the CGI held the camera's
   one-connection web server for ~3 s, and app.js decided which way to go by
   reading the label off its own button, which a status poll that had left before
   the click could rewrite.

   So every check here compares what the button says with what the camera says,
   and one check forces the race on purpose (a status poll fired while a click is
   in flight) and requires the label to still be right afterwards.

     google-chrome-stable --headless=new --remote-debugging-port=9333 \
       --user-data-dir=/tmp/camweb-chrome about:blank &
     ./tools/switch-check.mjs [token]

   It leaves the switch as it found it.
*/

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CAM = process.env.CAM_HOST || "192.168.10.46";
const PORT = process.env.CAM_HTTP_PORT || "8080";
const PAGE = "http://" + CAM + ":" + PORT + "/";
const API = PAGE + "cgi-bin/api";
const CDP = "http://127.0.0.1:9333";
const TOKEN = process.argv[2] || fs.readFileSync(path.join(HERE, ".token"), "utf8").trim();

let id = 0;
const pending = new Map();
let ws;
const consoleErrors = [];
const results = [];

function send(method, params = {}) {
  return new Promise((resolve, reject) => {
    const mid = ++id;
    pending.set(mid, { resolve, reject });
    ws.send(JSON.stringify({ id: mid, method, params }));
    setTimeout(() => {
      if (pending.has(mid)) { pending.delete(mid); reject(new Error(method + " timed out")); }
    }, 30000);
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function check(name, ok, detail) {
  results.push(!!ok);
  console.log("  " + name.padEnd(46) + " " + (ok ? "PASS" : "FAIL") + "  " + (detail || ""));
}

async function evalJs(expr) {
  const r = await send("Runtime.evaluate", { expression: expr });
  return r && r.result && r.result.value !== undefined ? r.result.value : null;
}

/* What the camera really has on the card, asked directly. This is the only
   referee: comparing the label against itself would prove nothing.
   Briefly cached, because every answer costs a forked CGI on a one-core box and
   the loops below would otherwise ask twice per second while also competing with
   the page's own polls. */
let truthAt = 0, truthVal = null;
async function truth() {
  if (Date.now() - truthAt < 1200 && truthVal) return truthVal;
  try {
    const j = await (await fetch(API + "?act=status&token=" + encodeURIComponent(TOKEN) +
                                 "&_=" + Date.now(), { headers: { "X-Token": TOKEN } })).json();
    truthVal = { off: !!j.live_off, running: !!j.live_running };
    truthAt = Date.now();
    return truthVal;
  } catch (e) { return null; }
}

/* CDP's Runtime.evaluate hands back a remote object reference, not the value, so
   an expression that returns an object reads as null here. Everything the harness
   inspects therefore comes back as a flat string and is unpacked below. */
async function label() {
  const s = await evalJs("(function(){var t=document.getElementById('lvtoggle');" +
                         "return t ? (t.textContent + '|' + t.disabled) : null})()");
  if (typeof s !== "string") return null;
  const i = s.lastIndexOf("|");
  return { text: s.slice(0, i), dis: s.slice(i + 1) === "true" };
}

async function until(fn, timeoutMs, what) {
  const stop = Date.now() + timeoutMs;
  let last = null;
  while (Date.now() < stop) {
    last = await fn();
    if (last) return last;
    await sleep(1200);
  }
  console.log("     timed out waiting for " + what + "; last saw: " + JSON.stringify(last));
  return null;
}

async function main() {
  /* The page's starting position is a static fact about the HTML, so check it
     there rather than racing the first poll: a button that prints a switch state
     the page has not been told is the same class of lie. */
  const html = await (await fetch(PAGE + "?_=" + Date.now())).text();
  check("html starts the switch honest",
        /id="lvtoggle"\s+disabled>Relay: reading/.test(html),
        (html.match(/id="lvtoggle"[^<]*/) || [""])[0]);

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
      consoleErrors.push("exception: " + (d.exception && d.exception.description
        ? d.exception.description.split("\n")[0] : d.text));
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

  const ready = await until(
    () => evalJs("(typeof lvMutate === 'function' && document.getElementById('lvtoggle')) ? 'ready' : null"),
    90000, "app.js to load");
  check("page script loaded", ready === "ready", String(ready));

  /* Before blaming the switch, prove the page is hearing from the camera at all:
     every status poll is a forked CGI on a 432 MHz single core and this harness
     asks the same camera from outside too, so the first poll can take tens of
     seconds to land. A check that gives up before that reports a dead control
     that is merely slow - which is the same confusion the bug itself started in. */
  const polling = await until(() => evalJs("(st.pan !== null) ? 'polling' : null"), 90000, "the first status poll");
  check("page's status poll lands", polling === "polling", String(polling));

  const lab = await until(async () => {
    const l = await label();
    return (l && !l.dis && /Relay: (allowed|blocked)/.test(l.text)) ? l : null;
  }, 90000, "the switch to be told its state by the camera");
  check("switch enabled only once the camera told it", !!lab, lab ? lab.text : "");

  const t0 = await truth();
  check("label matches the camera to start with",
        !!t0 && (("Relay: blocked" === lab.text) === t0.off),
        "label=" + lab.text + " card off=" + (t0 ? t0.off : "?"));
  const wasOff = t0 ? t0.off : false;

  /* The reported bug: click, then click back. */
  await evalJs("document.getElementById('lvtoggle').click()");
  const l1 = await until(async () => {
    const l = await label();
    if (!l || l.dis) return null;
    const tr = await truth();
    return (tr && (("Relay: blocked" === l.text) === tr.off) && tr.off !== wasOff) ? { l, tr } : null;
  }, 45000, "the first click to reach the card and the label to follow");
  check("click reaches the card and the label follows",
        !!l1, l1 ? l1.l.text + " card off=" + l1.tr.off : "");

  await evalJs("document.getElementById('lvtoggle').click()");
  const l2 = await until(async () => {
    const l = await label();
    if (!l || l.dis) return null;
    const tr = await truth();
    return (tr && (("Relay: blocked" === l.text) === tr.off) && tr.off === wasOff) ? { l, tr } : null;
  }, 45000, "the second click to put the switch back");
  check("the switch comes back (the reported bug)",
        !!l2, l2 ? l2.l.text + " card off=" + l2.tr.off : "");

  /* Force the race: status polls fired while a click is in flight. The click now
     takes about a second and the page polls on its own timer, so this window is
     ordinary rather than contrived. The old code let such a reply print the old
     word over the new one, and from then on every click asked for the state the
     camera was already in. */
  await evalJs("document.getElementById('lvtoggle').click()");
  await evalJs("poll(); poll();");
  await sleep(1200);
  await evalJs("poll();");
  const l3 = await until(async () => {
    const l = await label();
    if (!l || l.dis) return null;
    const tr = await truth();
    return (tr && (("Relay: blocked" === l.text) === tr.off)) ? { l, tr } : null;
  }, 45000, "label and card to agree after polls during the click");
  check("a poll landing mid-click cannot rewrite the switch",
        !!l3, l3 ? l3.l.text + " card off=" + l3.tr.off : "");

  /* Two clicks in the same tick: the second must be refused rather than queued,
     so the card ends in exactly one changed state. */
  const before = await truth();
  await evalJs("var t=document.getElementById('lvtoggle'); t.click(); t.click();");
  await sleep(4000);
  const after = await truth();
  check("a double click changes the state once",
        !!before && !!after && after.off === !before.off,
        "before off=" + (before ? before.off : "?") + " after off=" + (after ? after.off : "?"));

  const settled = await until(async () => {
    const l = await label();
    if (!l || l.dis) return null;
    const tr = await truth();
    return (tr && (("Relay: blocked" === l.text) === tr.off)) ? { l, tr } : null;
  }, 30000, "the switch to settle after the double click");
  check("label still matches the card after all that",
        !!settled, settled ? settled.l.text + " card off=" + settled.tr.off : "");

  /* Leave it as it was found. */
  const now = await truth();
  if (now && now.off !== wasOff) {
    await evalJs("document.getElementById('lvtoggle').click()");
    const back = await until(async () => {
      const tr = await truth();
      return (tr && tr.off === wasOff) ? tr : null;
    }, 30000, "the switch to be restored");
    check("switch restored as found", !!back, "off=" + (back ? back.off : "?"));
  } else {
    check("switch restored as found", true, "off=" + (now ? now.off : "?"));
  }

  check("no page errors", consoleErrors.length === 0, consoleErrors.join(" | "));

  const pass = results.filter(Boolean).length;
  console.log("\n  " + pass + "/" + results.length + " checks passed");
  ws.close();
  return pass === results.length ? 0 : 1;
}

main().then((rc) => process.exit(rc)).catch((e) => { console.log("harness: " + e.message); process.exit(2); });
