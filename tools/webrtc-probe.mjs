#!/usr/bin/env node
/* Does go2rtc's WebRTC path work on this board, and how fast is it?

   The HLS path is structurally behind: the page pulls 0.5 s segments one at a
   time from a loaded single core, and if a pull takes longer than the segment
   lasts, the buffer falls behind and nothing ever skips forward. WebRTC cannot
   drift like that - the receiver renders what arrives, and go2rtc waits for a
   key frame on its own before it starts sending. This probe answers whether the
   path exists on this build, whether Chrome completes ICE with it across the
   LAN, and what it costs the camera.

     google-chrome-stable --headless=new --remote-debugging-port=9333 \
       --user-data-dir=/tmp/camweb-chrome about:blank &
     ./webrtc-probe.mjs [--audio]

   Printed: offer/answer/ICE/first-frame timings, decoded frame size, jitter
   buffer delay, and a screenshot in logs/webrtc-preview.png.
*/
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CAM = process.env.CAM_HOST || "192.168.10.46";
const HTTP_PORT = process.env.CAM_HTTP_PORT || "8080";
const RELAY_PORT = process.env.LIVE_PORT || "1984";
const WANT_AUDIO = process.argv.includes("--audio");
const PAGE = `http://${CAM}:${HTTP_PORT}/`;
const SHOT = process.env.CAM_SHOT || path.join(HERE, "..", "logs", "webrtc-preview.png");

const target = await (await fetch("http://127.0.0.1:9333/json/new?" + encodeURIComponent(PAGE), { method: "PUT" })).json();
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
let id = 0;
const pending = new Map();
ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const send = (method, params = {}) => new Promise((r) => { const my = ++id; pending.set(my, r); ws.send(JSON.stringify({ id: my, method, params })); });

await send("Runtime.enable");
await new Promise((r) => setTimeout(r, 2000));

const script = `(async function(){
  var t0 = Date.now(), log = [];
  function mark(k, extra) { log.push(k + " +" + (Date.now() - t0) + "ms" + (extra ? "  " + extra : "")); }
  var url = "http://${CAM}:${RELAY_PORT}/api/webrtc?src=cam";
  var pc = new RTCPeerConnection({ iceServers: [] });
  var v = document.createElement("video");
  v.autoplay = true; v.muted = true; v.playsInline = true;
  v.style.cssText = "position:fixed;left:0;top:0;width:640px;height:480px;background:#000;z-index:99999";
  document.body.appendChild(v);
  var states = [];
  pc.oniceconnectionstatechange = function () { states.push("ice=" + pc.iceConnectionState); };
  pc.onconnectionstatechange = function () { states.push("conn=" + pc.connectionState); };
  pc.ontrack = function (e) {
    mark("track", e.track.kind);
    var ms = new MediaStream(); ms.addTrack(e.track); v.srcObject = ms;
  };
  pc.addTransceiver("video", { direction: "recvonly" });
  ${WANT_AUDIO ? 'pc.addTransceiver("audio", { direction: "recvonly" });' : ""}
  var answerSdp = "";
  try {
    var offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await new Promise(function (res) {
      var done = false, fin = function () { if (!done) { done = true; res(); } };
      pc.onicegatheringstatechange = function () { if (pc.iceGatheringState === "complete") fin(); };
      if (pc.iceGatheringState === "complete") fin();
      setTimeout(fin, 3000);
    });
    mark("offer gathered", (pc.localDescription.sdp.match(/^m=.*$/gm) || []).length + " m-lines");
    var r = await fetch(url, { method: "POST", body: pc.localDescription.sdp,
                               headers: { "content-type": "application/sdp" }, cache: "no-store" });
    var txt = await r.text();
    mark("answer", "http=" + r.status + " type=" + (r.headers.get("content-type") || "?") + " bytes=" + txt.length);
    // go2rtc answers a WebRTC offer with 201, not 200 - checking ==200 here
    // reports a working endpoint as a failure.
    if (r.status < 200 || r.status >= 300) return JSON.stringify({ log: log, states: states, error: "http " + r.status + " " + txt.slice(0, 300) });
    answerSdp = txt;
    if (answerSdp.charAt(0) === "{") { try { answerSdp = JSON.parse(txt).sdp; } catch (e) {} }
    await pc.setRemoteDescription({ type: "answer", sdp: answerSdp });
    mark("remote set");
  } catch (e) {
    return JSON.stringify({ log: log, states: states, error: String(e) });
  }

  var deadline = Date.now() + 25000, firstFrame = 0;
  while (Date.now() < deadline) {
    await new Promise(function (r) { setTimeout(r, 500); });
    if (v.videoWidth > 0 && !firstFrame) { firstFrame = Date.now(); mark("first frame", v.videoWidth + "x" + v.videoHeight); break; }
  }
  var t = v.currentTime;
  await new Promise(function (r) { setTimeout(r, 6000); });
  var advanced = v.currentTime - t;

  var stats = await pc.getStats();
  var rtp = null, cand = null;
  stats.forEach(function (s) {
    if (s.type === "inbound-rtp") rtp = s;
    if (s.type === "candidate-pair" && s.state === "succeeded") cand = s;
  });
  var res = {
    log: log, states: states,
    firstFrameMs: firstFrame ? firstFrame - t0 : null,
    videoWidth: v.videoWidth, videoHeight: v.videoHeight,
    currentTime: +v.currentTime.toFixed(2), advancedIn6s: +advanced.toFixed(2),
    readyState: v.readyState, answerMlines: (answerSdp.match(/^m=.*$/gm) || []).join(" | ")
  };
  if (cand) res.candidatePair = cand.protocol + " " + (cand.availableOutgoingBitrate || "");
  if (rtp) {
    res.kind = rtp.kind;
    res.framesReceived = rtp.framesReceived;
    res.framesDecoded = rtp.framesDecoded;
    res.framesDropped = rtp.framesDropped;
    res.bytesReceived = rtp.bytesReceived;
    res.jitterBufferAvgMs = (rtp.jitterBufferDelay && rtp.jitterBufferEmittedCount)
      ? Math.round(1000 * rtp.jitterBufferDelay / rtp.jitterBufferEmittedCount) : null;
    res.jitterBufferTargetMs = rtp.jitterBufferTarget ? Math.round(1000 * rtp.jitterBufferTarget) : null;
    res.totalProcessingMs = rtp.totalProcessingDelay ? Math.round(1000 * rtp.totalProcessingDelay) : null;
    res.packetsLost = rtp.packetsLost; res.nackCount = rtp.nackCount;
  }
  return JSON.stringify(res);
})()`;

const out = await send("Runtime.evaluate", { expression: script, awaitPromise: true, returnByValue: true });
if (out.exceptionDetails) {
  console.log("page exception:", JSON.stringify(out.exceptionDetails).slice(0, 900));
}
const val = (out.result && out.result.result && out.result.result.value) !== undefined
  ? out.result.result.value
  : (out.result && out.result.value);
if (val === undefined) console.log("raw evaluate response:", JSON.stringify(out).slice(0, 500));
let data = {};
try { data = JSON.parse(val || "{}"); } catch (e) { console.log("raw:", String(val).slice(0, 400)); }
console.log("timeline:");
(data.log || []).forEach((l) => console.log("   " + l));
console.log("states:  ", (data.states || []).join(" "));
if (data.error) console.log("ERROR:   ", data.error);
const { log, states, error, ...rest } = data;
console.log("result:");
Object.entries(rest).forEach(([k, v]) => { if (v !== "" && v !== null && v !== undefined) console.log("   " + k + " = " + v); });

const shot = await send("Page.captureScreenshot", { format: "png" });
if (shot.result && shot.result.data) {
  const fs = await import("node:fs");
  fs.mkdirSync(path.dirname(SHOT), { recursive: true });
  fs.writeFileSync(SHOT, Buffer.from(shot.result.data, "base64"));
  console.log("screenshot: " + SHOT);
}
await send("Target.closeTarget", { targetId: target.id });
ws.close();
process.exit(data.firstFrameMs ? 0 : 1);

