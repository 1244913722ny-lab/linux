#!/usr/bin/env node
'use strict';
/*
 * cam_server.js —— 手机网页相机推流 -> 板子 camera_demo 的桥接服务（Node 单文件）
 *
 * 背景：
 *   file/app/camera_demo.c 是 TCP 客户端，按 [4 字节大端长度][JPEG 数据] 的帧协议
 *   循环收帧并解码显示。手机浏览器只会 HTTP/WebSocket，当不了 TCP 服务端，
 *   所以需要本服务做协议转换：
 *
 *     手机浏览器 --(WebSocket, JPEG blob)--> 本服务 --(TCP, [4B 大端长度][JPEG])--> 板子
 *
 * 组成（全部在这一个文件里）：
 *   1. HTTPS :8443       返回内嵌网页（相机预览 / 开始推流 / 暂停 / 拍一张）
 *   2. WebSocket /ws     收手机发来的 JPEG 二进制帧（wss://…:8443/ws）
 *   3. TCP    :8888      等板子 camera_demo 连入，把 ws 帧加长度头后原样转发
 *
 * 为什么有 HTTPS：
 *   iOS Safari 的 getUserMedia 只允许安全上下文，http://局域网IP 直接拒绝
 *   （失败还是静默的：navigator.mediaDevices 为 undefined）。跑一次
 *       node gen_cert.js
 *   生成自签证书后，本服务自动进入 HTTPS 模式：https:8443 全功能 +
 *   http:8080 只留 /ca.crt（手机装 CA 描述文件）、/models/（detector 加载模型）、
 *   以及一个 iOS 引导页。没有证书时维持纯 http 模式（Android Chrome 可用）。
 *
 * 运行：
 *   npm install          （依赖 ws / node-forge 等，见 package.json）
 *   node gen_cert.js     （生成自签证书，只需一次；PC 换网段后 FORCE=1 重跑）
 *   npm start            或 node cam_server.js
 *
 * 网络前提：
 *   - 手机与 PC 同一 Wi-Fi，手机浏览器访问 https://<PC局域网IP>:8443
 *   - 板子填 10.0.2.2:8888（QEMU 用户态 NAT 下 PC 对 guest 的地址；
 *     若 QEMU 用网桥模式则填 PC 局域网 IP，且本服务必须监听 0.0.0.0 —— 已是默认）
 *   - PC 防火墙需放行 8080 与 8443 与 8888
 *   - QEMU 启动脚本必须给 vexpress-a9 插上网卡，例如：
 *       -netdev user,id=n1 -device lan9118,netdev=n1
 *     板子起来后 udhcpc 拿到 10.0.2.15/网关 10.0.2.2
 *
 * 端口可用环境变量覆盖：CAM_HTTP_PORT / CAM_HTTPS_PORT / CAM_TCP_PORT
 */

const http = require('http');
const https = require('https');
const net = require('net');
const os = require('os');
const fs = require('fs');
const path = require('path');
const { WebSocketServer } = require('ws');
const Detector = require('./detector');

const HTTP_PORT = parseInt(process.env.CAM_HTTP_PORT || '8080', 10);
const HTTPS_PORT = parseInt(process.env.CAM_HTTPS_PORT || '8443', 10);
const TCP_PORT = parseInt(process.env.CAM_TCP_PORT || '8888', 10);
/* 与 camera_demo.c 的 CAM_JPEG_MAX 保持一致：单帧上限 512KB */
const MAX_FRAME = 512 * 1024;

/* ------------------------- HTTPS 证书（iOS 摄像头的前提） ------------------------- */

/* gen_cert.js 的输出；三件齐全才算 HTTPS 模式，否则退回纯 http */
const CERT_DIR = path.join(__dirname, 'certs');
const CA_PEM = path.join(CERT_DIR, 'ca.crt.pem');
const CRT_PEM = path.join(CERT_DIR, 'server.crt.pem');
const KEY_PEM = path.join(CERT_DIR, 'server.key.pem');
const hasTLS = fs.existsSync(CA_PEM) && fs.existsSync(CRT_PEM) && fs.existsSync(KEY_PEM);

/* ------------------------- 板子侧 TCP 连接管理 ------------------------- */

let boardSock = null;    // 当前连入的板子 socket（同时只服务一路）
let latestFrame = null;  // 板子未连接期间缓存的最近一帧，连上立即补发

/* ------------------------- 目标检测（coco-ssd） ------------------------- */

const detector = new Detector();
let phoneWs = null; // 当前手机连接（检测结果回传预览用）

/* 把一帧 JPEG 按 [4B 大端长度][JPEG] 写给板子；板子未连接返回 false */
function writeFrame(jpeg) {
  if (!boardSock) return false;
  const header = Buffer.alloc(4);
  header.writeUInt32BE(jpeg.length, 0); // 大端长度前缀，与 camera_demo.c 解析一致
  boardSock.write(Buffer.concat([header, jpeg]));
  /* 注：未处理写背压（write 返回 false 时说明内核缓冲满）。
   * 手机帧率低、局域网带宽大，第一版够用；真出现背压再在这里缓存排队。 */
  return true;
}

const boardServer = net.createServer((sock) => {
  console.log(`[板子] 已连接 ${sock.remoteAddress}:${sock.remotePort}`);
  boardSock = sock;
  sock.on('error', (e) => console.warn('[板子] socket 错误:', e.message));
  sock.on('close', () => {
    console.log('[板子] 已断开');
    if (boardSock === sock) boardSock = null;
  });
  /* 板子刚连上时若已有最新帧，立刻补发一帧，免去干等手机下一帧 */
  if (latestFrame) {
    console.log(`[板子] 补发缓存的最近一帧（${latestFrame.length} 字节）`);
    writeFrame(latestFrame);
  }
});

/* ------------------------- 手机网页（内嵌） ------------------------- */

const PAGE_HTML = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<title>相机推流</title>
<style>
:root { color-scheme: light; }
* { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: -apple-system, "PingFang SC", "Microsoft YaHei", sans-serif;
  background: #f5f6f8; color: #1f2329;
  max-width: 520px; margin: 0 auto; padding: 12px;
}
h1 { font-size: 17px; font-weight: 600; display: flex; align-items: center; gap: 8px; }
.dot { width: 8px; height: 8px; border-radius: 50%; background: #c9cdd4; }
.dot.on { background: #22c55e; }
.stage {
  position: relative; margin-top: 12px; border-radius: 12px;
  overflow: hidden; background: #000; aspect-ratio: 4 / 3;
}
video { width: 100%; height: 100%; object-fit: fill; display: block; }
/* 检测框叠加层：与 video 同样 fill 铺满，坐标按检测帧(抓帧 canvas)尺寸线性映射 */
#overlay { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
.mask {
  position: absolute; inset: 0; display: flex; flex-direction: column;
  align-items: center; justify-content: center; gap: 14px;
  color: #9ca3af; font-size: 13px; background: #1f2329;
}
/* 遮罩内的开启按钮：未开相机时第一眼就能看到的入口 */
.mask-btn {
  padding: 12px 30px; border: none; border-radius: 10px;
  background: #185fa5; color: #fff; font-size: 15px; font-weight: 500;
}
.mask-btn:disabled { background: #9fc2e0; }
.panel {
  margin-top: 12px; background: #fff; border-radius: 12px;
  padding: 12px; display: flex; flex-direction: column; gap: 10px;
}
.row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; font-size: 13px; color: #5f6368; }
select {
  padding: 6px 8px; border: 1px solid #d8dce1; border-radius: 8px;
  background: #fff; font-size: 13px; color: #1f2329;
}
.btns { gap: 8px; }
.btns button {
  flex: 1; padding: 11px 0; border: none; border-radius: 10px;
  font-size: 14px; font-weight: 500;
}
#btnStart { background: #185fa5; color: #fff; }
#btnStart:disabled { background: #9fc2e0; }
#btnPause, #btnSnap { background: #e8eaed; color: #3c4043; }
.btns button:disabled { opacity: .45; }
.stats { display: flex; gap: 14px; font-size: 12px; color: #5f6368; flex-wrap: wrap; }
.stats b { color: #1f2329; font-weight: 600; }
.hint {
  font-size: 12px; color: #854f0b; background: #faeeda;
  border-radius: 8px; padding: 8px 10px; line-height: 1.7; display: none;
}
</style>
</head>
<body>
<h1>相机推流 <span id="wsState" class="dot"></span><span id="wsText" style="font-size:12px;color:#5f6368;font-weight:400">连接中…</span></h1>

<div class="stage" id="stage">
  <video id="video" playsinline autoplay muted></video>
  <canvas id="overlay"></canvas>
  <div id="camOff" class="mask">
    <span id="maskText">相机未开启</span>
    <button id="btnOpenCam" class="mask-btn">开启相机</button>
  </div>
</div>
<canvas id="canvas" width="320" height="240" hidden></canvas>

<div class="panel">
  <div class="row">
    <label>摄像头
      <select id="facing">
        <option value="environment">后置</option>
        <option value="user">前置</option>
      </select>
    </label>
    <label>分辨率
      <select id="res">
        <option value="320x240">320×240</option>
        <option value="480x360" selected>480×360</option>
        <option value="640x480">640×480</option>
      </select>
    </label>
    <label>帧率
      <select id="fps">
        <option value="5">5</option>
        <option value="10" selected>10</option>
        <option value="15">15</option>
      </select> fps
    </label>
  </div>
  <div class="row">
    <label><input type="checkbox" id="chkDetect"> 目标检测</label>
    <label style="flex:1">阈值 <input type="range" id="thr" min="0.1" max="0.8" step="0.05" value="0.5" style="vertical-align:middle"><b id="thrVal">0.5</b></label>
  </div>
  <div class="row btns">
    <button id="btnStart">开始推流</button>
    <button id="btnPause" disabled>暂停推流</button>
    <button id="btnSnap">拍一张</button>
  </div>
  <div class="stats">
    <span>实测 <b id="stFps">0</b> fps</span>
    <span>已发 <b id="stFrames">0</b> 帧</span>
    <span>已拍 <b id="stSnaps">0</b> 张</span>
    <span>最近帧 <b id="stSize">0</b> KB</span>
    <span>最近检出 <b id="stBoxes">0</b> 个</span>
  </div>
  <div id="hint" class="hint"></div>
</div>

<script>
'use strict';
var $ = function (id) { return document.getElementById(id); };
var video = $('video'), canvas = $('canvas'), ctx = canvas.getContext('2d');
var ws = null, stream = null, timer = null, streaming = false;
var st = { frames: 0, snaps: 0, size: 0, fps: 0, boxes: 0 };
var fpsWindow = []; /* 最近 10 帧的发送时刻，用于实测 fps */

function hint(msg) { $('hint').textContent = msg; $('hint').style.display = msg ? 'block' : 'none'; }

/* ---------- WebSocket：断线 1 秒后自动重连 ---------- */
function setWs(ok) {
  $('wsState').className = 'dot' + (ok ? ' on' : '');
  $('wsText').textContent = ok ? '已连接桥接服务' : '连接中…';
}
function connect() {
  var proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws');
  ws.binaryType = 'arraybuffer';
  ws.onopen = function () { setWs(true); };
  ws.onclose = function () { setWs(false); setTimeout(connect, 1000); };
  ws.onerror = function () {};
  ws.onmessage = function (ev) {
    /* 只处理文本消息：detections（PC 检测结果）/ detect_state（状态同步） */
    var msg;
    try { msg = JSON.parse(ev.data); } catch (e) { return; }
    if (msg.type === 'detections') {
      st.boxes = msg.detections.length;
      drawDetections(msg);
      refreshStats();
    } else if (msg.type === 'detect_state') {
      if ('enable' in msg) $('chkDetect').checked = !!msg.enable;
      if ('scoreThreshold' in msg) {
        $('thr').value = msg.scoreThreshold;
        $('thrVal').textContent = msg.scoreThreshold;
      }
    }
  };
}
connect();

/* ---------- 检测框叠加层（与 PC 端画框逻辑保持一致） ---------- */
var overlay = $('overlay'), octx = overlay.getContext('2d');
var ovW = 0, ovH = 0;
/* 类别配色表：与 detector.js 的 COLORS 一致，hash 规则也一致 */
var COLORS = ['#FF3B30','#34C759','#007AFF','#FF9500','#AF52DE','#5AC8FA','#FFCC00','#FF2D55','#8E8E93','#FF6B6B','#4ECDC4','#55606B'];
function boxColor(cls, i) { return COLORS[(cls.charCodeAt(0) + cls.length + i) % COLORS.length]; }

function fitOverlay() {
  var r = $('stage').getBoundingClientRect();
  var dpr = window.devicePixelRatio || 1;
  ovW = r.width; ovH = r.height;
  overlay.width = r.width * dpr;
  overlay.height = r.height * dpr;
  octx.setTransform(dpr, 0, 0, dpr, 0, 0);
}
window.addEventListener('resize', fitOverlay);
fitOverlay();

/* 按 PC 回传的检测结果在预览上画框。
 * video 用 object-fit:fill 铺满 stage，与抓帧 drawImage(video,0,0,w,h) 行为一致，
 * 因此检测帧坐标可按 ovW/ovH 对 msg.w/msg.h 线性映射，无裁切偏移。 */
function drawDetections(msg) {
  octx.clearRect(0, 0, ovW, ovH);
  if (!msg.detections.length) return;
  var sx = ovW / msg.w, sy = ovH / msg.h;
  msg.detections.forEach(function (d, i) {
    var x = d.bbox[0] * sx, y = d.bbox[1] * sy;
    var w = d.bbox[2] * sx, h = d.bbox[3] * sy;
    var c = boxColor(d.class, i);
    octx.strokeStyle = c;
    octx.lineWidth = 2;
    octx.strokeRect(x, y, w, h);
    var label = d.class + ' ' + Math.round(d.score * 100) + '%';
    octx.font = '12px sans-serif';
    var tw = octx.measureText(label).width + 8;
    octx.fillStyle = c;
    octx.fillRect(x, y - 18, tw, 18);
    octx.fillStyle = '#fff';
    octx.fillText(label, x + 4, y - 5);
  });
}

/* ---------- 相机 ---------- */
function openCamera() {
  /* iOS Safari 在非安全上下文（http://局域网IP）下 navigator.mediaDevices 为 undefined，
   * 直接 .getUserMedia 会同步抛 TypeError —— 发生在 return promise 之前，
   * 调用方的 .catch 收不到，页面会静默无提示。这里先显式检查并给明确原因。 */
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    var why = window.isSecureContext
      ? '当前浏览器不支持相机 API'
      : '页面不是安全上下文：iOS 必须用 HTTPS 打开（Android Chrome 可用 http 局域网 IP）';
    hint('无法开启相机：' + why);
    $('maskText').textContent = '无法开启相机（需 HTTPS）';
    return Promise.reject(new Error(why));
  }
  var parts = $('res').value.split('x');
  var w = +parts[0], h = +parts[1];
  canvas.width = w; canvas.height = h; /* 抓帧输出尺寸 = 选择的分辨率 */
  var constraints = {
    audio: false,
    video: {
      facingMode: { ideal: $('facing').value },
      width: { ideal: w }, height: { ideal: h }
    }
  };
  return navigator.mediaDevices.getUserMedia(constraints).then(function (s) {
    if (stream) stream.getTracks().forEach(function (t) { t.stop(); });
    stream = s;
    video.srcObject = s;
    $('camOff').style.display = 'none';
    /* 等首帧就绪，保证后面 drawImage 有内容 */
    if (video.readyState >= 2) return;
    return new Promise(function (r) { video.onloadeddata = r; });
  }).catch(function (e) {
    var msg = '相机打开失败：' + e.message + '（请允许相机权限；微信内置浏览器不支持，请用系统浏览器）';
    hint(msg);
    /* 遮罩上的文字同步更新——从遮罩按钮点进来时，用户视线在预览区 */
    $('maskText').textContent = '相机打开失败，点按钮重试';
    $('btnOpenCam').disabled = false;
    throw e;
  });
}

/* ---------- 抓一帧并发送（持续流与"拍一张"共用） ---------- */
function grabAndSend() {
  ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
  canvas.toBlob(function (blob) {
    if (!blob) return;
    if (blob.size > 500 * 1024) { hint('帧过大（' + (blob.size / 1024 | 0) + 'KB），请降低分辨率或帧率'); return; }
    if (!ws || ws.readyState !== 1) return; /* 桥接断开：静默丢弃，重连后继续 */
    ws.send(blob); /* 二进制帧，桥接加 4 字节长度头后转给板子 */
    st.frames++; st.size = blob.size;
    var now = performance.now();
    fpsWindow.push(now);
    if (fpsWindow.length > 10) fpsWindow.shift();
    if (fpsWindow.length >= 2) {
      var span = (fpsWindow[fpsWindow.length - 1] - fpsWindow[0]) / 1000;
      st.fps = span > 0 ? ((fpsWindow.length - 1) / span).toFixed(1) : '0';
    }
    refreshStats();
  }, 'image/jpeg', 0.6);
}

/* ---------- 持续推流循环（每次按当前选择的帧率排下一帧） ---------- */
function loop() {
  if (!streaming) return;
  grabAndSend();
  timer = setTimeout(loop, 1000 / +$('fps').value);
}

function refreshStats() {
  $('stFps').textContent = st.fps;
  $('stFrames').textContent = st.frames;
  $('stSnaps').textContent = st.snaps;
  $('stSize').textContent = (st.size / 1024).toFixed(1);
  $('stBoxes').textContent = st.boxes;
}

/* ---------- 目标检测开关 / 阈值（发控制消息给 PC） ---------- */
$('chkDetect').onchange = function () {
  if (ws && ws.readyState === 1) {
    ws.send(JSON.stringify({ cmd: 'detect', enable: this.checked }));
  } else {
    hint('桥接未连接，开关将在重连后生效');
  }
};
$('thr').oninput = function () { $('thrVal').textContent = this.value; };
$('thr').onchange = function () {
  if (ws && ws.readyState === 1) {
    ws.send(JSON.stringify({ cmd: 'config', scoreThreshold: +this.value }));
  }
};

/* ---------- 按钮 ---------- */
/* 遮罩上的「开启相机」：与「开始推流」完全同一条路径（开相机 + 开始推流），
 * 未开相机时用户在预览黑窗上第一眼就能看到入口 */
$('btnOpenCam').onclick = function () { $('btnStart').click(); };
$('btnStart').onclick = function () {
  var go = function () {
    streaming = true;
    $('btnStart').disabled = true;
    $('btnPause').disabled = false;
    hint('');
    loop();
  };
  if (stream) go();
  else {
    $('btnOpenCam').disabled = true; /* 请求权限期间防重复点击 */
    try {
      openCamera().then(go).catch(function () {});
    } catch (e) {
      hint('相机打开失败：' + e.message);
      $('btnOpenCam').disabled = false;
    }
  }
};
$('btnPause').onclick = function () {
  streaming = false;
  clearTimeout(timer);
  $('btnStart').disabled = false;
  $('btnPause').disabled = true;
  /* 暂停只是停止发送，板子画面定格在最后一帧（camera_demo 不再收到新帧） */
};
$('btnSnap').onclick = function () {
  /* 单拍一帧：未开相机则先开相机再拍；建议在"暂停推流"后使用，板子会定格显示这一帧 */
  if (stream) { grabAndSend(); st.snaps++; refreshStats(); }
  else {
    try {
      openCamera().then(function () { grabAndSend(); st.snaps++; refreshStats(); }).catch(function () {});
    } catch (e) { hint('相机打开失败：' + e.message); }
  }
};

/* ---------- 参数即时生效 ---------- */
$('facing').onchange = function () {
  if (!stream) return;
  try { openCamera().catch(function () {}); } catch (e) { hint('切换摄像头失败：' + e.message); }
};
$('res').onchange = function () {
  var p = $('res').value.split('x');
  canvas.width = +p[0]; canvas.height = +p[1];
};

/* ---------- 微信内置浏览器检测 ---------- */
if (/MicroMessenger/i.test(navigator.userAgent)) {
  hint('检测到微信内置浏览器，摄像头不可用。请点右上角「…」选择「用浏览器打开」。');
}
</script>
</body>
</html>
`;

/* iOS 引导页：http:// 访问时返回，指引装 CA 证书 -> 信任 -> 用 https 访问。
 * __HTTPS_URL__ 由服务端按实际监听端口填充。 */
const GUIDE_HTML = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>请先安装证书（iOS）</title>
<style>
:root { color-scheme: light; }
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: -apple-system, "PingFang SC", "Microsoft YaHei", sans-serif; background: #f5f6f8; color: #1f2329; max-width: 520px; margin: 0 auto; padding: 20px 16px 40px; line-height: 1.8; }
h1 { font-size: 19px; margin-bottom: 12px; }
p { font-size: 14px; color: #3c4043; margin-bottom: 14px; }
ol { margin: 0 0 16px 20px; }
li { font-size: 14px; margin-bottom: 14px; }
b { color: #1f2329; }
a.ca { display: inline-block; background: #185fa5; color: #fff; text-decoration: none; padding: 10px 20px; border-radius: 10px; font-size: 15px; font-weight: 500; }
.url { font-family: ui-monospace, Menlo, monospace; background: #eceff3; border-radius: 6px; padding: 2px 8px; font-size: 13px; word-break: break-all; }
.note { font-size: 12px; color: #5f6368; background: #fff; border-radius: 10px; padding: 12px 14px; }
</style>
</head>
<body>
<h1>iOS 需要先安装一次证书</h1>
<p>Apple 规定摄像头只能在 HTTPS 页面使用。按下面三步操作一次，以后直接点书签就能推流：</p>
<ol>
  <li><b>安装证书</b><br>点 <a class="ca" href="/ca.crt">下载 CA 证书</a><br>弹出「此网站正在尝试下载配置文件」时点<b>允许</b>，然后到 <b>设置 → 通用 → VPN与设备管理</b> 安装刚下载的描述文件。</li>
  <li><b>信任证书</b><br><b>设置 → 通用 → 关于本机 → 证书信任设置</b>，打开 <b>CamServer Local CA</b> 的开关。</li>
  <li><b>打开推流页</b><br>访问 <span class="url">__HTTPS_URL__</span>（建议添加到主屏幕）。</li>
</ol>
<p class="note">Android 手机（Chrome）不受此限：http 局域网 IP 被视作可信，可直接访问 <span class="url">http://同网段IP:${HTTP_PORT}</span> 推流；证书装不装都能用 https。</p>
</body>
</html>`;

/* ------------------------- 静态文件（仅 models/ 目录） ------------------------- */

/* detector 的模型权重经本地 HTTP 加载（纯 tfjs 的 fetch 不支持 file://）。
 * 只开放 __dirname 下的文件，防目录穿越。 */
function serveStatic(res, urlPath) {
  const filePath = path.normalize(path.join(__dirname, urlPath));
  if (!filePath.startsWith(__dirname)) {
    res.writeHead(403);
    res.end('forbidden');
    return;
  }
  fs.readFile(filePath, (err, buf) => {
    if (err) {
      res.writeHead(404);
      res.end();
      return;
    }
    const mime = path.extname(filePath) === '.json' ? 'application/json' : 'application/octet-stream';
    res.writeHead(200, { 'Content-Type': mime });
    res.end(buf);
  });
}

/* ------------------------- HTTP/HTTPS 路由 ------------------------- */

/*
 * servePage=true：/ 返回推流网页（https:8443 用；无证书的纯 http 模式也给推流页，
 *   Android Chrome 的 http 局域网 IP 是可信的，能开相机）。
 * servePage=false：/ 返回 iOS 引导页（有证书时 http:8080 的角色——手机装 CA 前打不开 https）。
 * 两个模式都保留 /models/（detector 经 http 加载模型）与 /ca.crt（iOS 装 CA 入口）。
 */
function requestHandler(req, res, servePage) {
  const urlPath = req.url.split('?')[0];

  if (urlPath.startsWith('/models/')) {
    return serveStatic(res, urlPath);
  }
  if (urlPath === '/ca.crt') {
    if (!fs.existsSync(CA_PEM)) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      return res.end('证书未生成，先在 PC 上执行 node gen_cert.js\n');
    }
    res.writeHead(200, {
      'Content-Type': 'application/x-x509-ca-cert',
      'Content-Disposition': 'attachment; filename="cam-server-ca.crt"',
    });
    return res.end(fs.readFileSync(CA_PEM));
  }

  if (servePage) {
    if (urlPath === '/' || urlPath === '/index.html') {
      res.writeHead(200, {
        'Content-Type': 'text/html; charset=utf-8',
        'Cache-Control': 'no-store',
      });
      return res.end(PAGE_HTML);
    }
    res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
    return res.end('404 Not Found\n');
  }

  /* 引导模式：http 非 models/ca.crt 路径 -> iOS 引导页。
   * host 头自带端口且是 http 端口，只取 hostname，端口用 HTTPS_PORT */
  const hostname = (req.headers.host || '').split(':')[0];
  res.writeHead(200, {
    'Content-Type': 'text/html; charset=utf-8',
    'Cache-Control': 'no-store',
  });
  res.end(GUIDE_HTML.replace('__HTTPS_URL__', `https://${hostname || '<PC_IP>'}:${HTTPS_PORT}`));
}

/* ------------------------- WebSocket（收手机帧） ------------------------- */

/*
 * 手机端文本控制消息（JSON）：
 *   {"cmd":"detect","enable":true}        开关目标检测
 *   {"cmd":"config","scoreThreshold":0.5} 调置信度阈值
 * 服务器回 {"type":"detect_state", ...} 供页面校准控件状态。
 */
function handleControl(ws, text) {
  let msg;
  try {
    msg = JSON.parse(text);
  } catch (e) {
    return;
  }
  if (msg.cmd === 'detect') {
    detector.setEnabled(msg.enable);
    ws.send(JSON.stringify({ type: 'detect_state', enable: detector.enabled }));
  } else if (msg.cmd === 'config') {
    if ('scoreThreshold' in msg) {
      detector.setThreshold(msg.scoreThreshold);
      ws.send(JSON.stringify({ type: 'detect_state', scoreThreshold: detector.scoreThreshold }));
    }
  } else {
    console.log('[手机] 未知控制消息:', text.slice(0, 64));
  }
}

/* wss 挂哪个 server 由启动区决定（HTTPS 模式下挂 https server）。
 * connection 处理逻辑与协议无关，抽成函数复用。 */
function attachWs(server) {
  const wss = new WebSocketServer({ server, path: '/ws' });
  wss.on('connection', (ws, req) => {
  console.log(`[手机] 已连接 ${req.socket.remoteAddress}`);
  phoneWs = ws;
  let n = 0;
  ws.on('message', (data, isBinary) => {
    if (!isBinary) {
      handleControl(ws, String(data));
      return;
    }
    const jpeg = Buffer.isBuffer(data) ? data : Buffer.from(data);
    if (jpeg.length === 0 || jpeg.length > MAX_FRAME) {
      console.warn(`[手机] 帧大小异常（${jpeg.length} 字节），丢弃`);
      return;
    }
    /* 检测处理：返回 null = PC 忙丢帧；否则为待发帧（可能带框） */
    detector.processFrame(jpeg).then((out) => {
      if (out === null) return; /* 忙：丢中间帧，最新帧优先 */
      latestFrame = out;        /* 缓存最新待发帧，板子随时连上可补发 */
      n++;
      if (!writeFrame(out)) {
        /* 板子未连接：帧已进缓存，等板子连上补发，不影响手机端 */
      }
      if (n % 50 === 0) {
        console.log(`[手机] 已转发 ${n} 帧（检测累计 ${detector.stats.frames} 帧 / ${detector.stats.boxes} 框，最近 ${detector.stats.lastMs}ms）`);
      }
    }).catch((e) => console.warn('[手机] 帧处理异常:', e.message));
  });
  ws.on('close', () => {
    console.log(`[手机] 已断开（本次 ${n} 帧）`);
    if (phoneWs === ws) phoneWs = null;
  });
  ws.on('error', (e) => console.warn('[手机] ws 错误:', e.message));
  });
  return wss;
}

/* 检测结果回传手机预览（含空结果，手机端据此清框） */
detector.onDetections = (payload) => {
  if (phoneWs && phoneWs.readyState === 1) {
    phoneWs.send(JSON.stringify(Object.assign({ type: 'detections' }, payload)));
  }
};

/* ------------------------- 启动 ------------------------- */

/* 收集 PC 的局域网 IPv4，启动横幅里列出来，手机选同网段的连 */
function localIps() {
  const ips = [];
  const ifaces = os.networkInterfaces();
  for (const name of Object.keys(ifaces)) {
    for (const it of ifaces[name]) {
      if ((it.family === 'IPv4' || it.family === 4) && !it.internal) ips.push(it.address);
    }
  }
  return ips;
}

/* 模型经本地 /models 路由加载（约 18MB，首次加载 ~0.5s）。
 * http/https 都 serve /models/；detector 的 fetch 不支持 file://，固定走 http://127.0.0.1。
 * 加载失败不致命：服务降级为纯转发，目标检测不可用。 */
function initDetector() {
  detector
    .init(`http://127.0.0.1:${HTTP_PORT}/models/ssdlite_mobilenet_v2/model.json`)
    .catch((e) => console.warn('[检测] 模型加载失败，降级为纯转发:', e.message));
}

if (hasTLS) {
  /* HTTPS 模式（iOS 可用）：https:8443 全功能 + wss；http:8080 只留引导/证书/模型 */
  const httpsServer = https.createServer(
    { cert: fs.readFileSync(CRT_PEM), key: fs.readFileSync(KEY_PEM) },
    (req, res) => requestHandler(req, res, true)
  );
  const httpGuide = http.createServer((req, res) => requestHandler(req, res, false));
  attachWs(httpsServer); /* wss 只挂 https，手机走 wss://…:8443/ws */

  httpsServer.listen(HTTPS_PORT, '0.0.0.0', () => {
    const ips = localIps();
    console.log('==============================================');
    console.log('模式：HTTPS（iOS 摄像头可用）');
    console.log(`推流页面  https://${ips[0] || '<PC_IP>'}:${HTTPS_PORT}   （本机 IPv4: ${ips.join(', ') || '无'}）`);
    console.log(`iOS 首次  http://${ips[0] || '<PC_IP>'}:${HTTP_PORT} 打开，按页面指引安装并信任 CA 证书`);
    console.log(`板子连接  0.0.0.0:${TCP_PORT}   （QEMU NAT 下板子填 10.0.2.2:${TCP_PORT}）`);
    console.log('前提：手机与 PC 同一 Wi-Fi；防火墙放行 8080/8443/8888；QEMU 已插 lan9118 网卡');
    console.log('==============================================');
  });
  httpGuide.listen(HTTP_PORT, '0.0.0.0', () => {
    console.log(`[引导] http ${HTTP_PORT} 已监听（iOS 引导页 + /ca.crt + /models/）`);
  });
  initDetector();
} else {
  /* 无证书：纯 http 模式。Android Chrome 可直接用；iOS 打开后点开相机按钮会提示需 HTTPS */
  const httpServer = http.createServer((req, res) => requestHandler(req, res, true));
  attachWs(httpServer);
  httpServer.listen(HTTP_PORT, '0.0.0.0', () => {
    const ips = localIps();
    console.log('==============================================');
    console.log('模式：纯 HTTP（未检测到证书，iOS 将无法开相机）');
    console.log(`手机访问  http://${ips[0] || '<PC_IP>'}:${HTTP_PORT}   （本机 IPv4: ${ips.join(', ') || '无'}）`);
    console.log(`板子连接  0.0.0.0:${TCP_PORT}   （QEMU NAT 下板子填 10.0.2.2:${TCP_PORT}）`);
    console.log('iOS 需要 HTTPS：在 PC 上执行 node gen_cert.js 生成证书后重启本服务');
    console.log('前提：手机与 PC 同一 Wi-Fi；防火墙放行以上端口；QEMU 已插 lan9118 网卡');
    console.log('==============================================');
  });
  initDetector();
}

boardServer.listen(TCP_PORT, '0.0.0.0', () => {
  console.log(`[板子] TCP ${TCP_PORT} 已监听，等待 camera_demo 连接…`);
});
