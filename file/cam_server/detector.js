'use strict';
/*
 * detector.js —— coco-ssd 目标检测 + SVG 画框 + JPEG 重编码
 *
 * 为什么是这套组合（选型记录）：
 *   - @tensorflow/tfjs + tfjs-backend-wasm：纯 JS/WASM，Windows/Node 22 零编译安装。
 *     （@tensorflow/tfjs-node 原生绑定在 Node 22 无预编译包、本地编译失败，已放弃）
 *   - @tensorflow-models/coco-ssd：detect() 直接返回框/类别/置信度，免去手写 NMS。
 *     注意：纯 tfjs 的 fetch 走 undici，不支持 file://，模型必须经 HTTP 加载
 *     （cam_server 用 /models 路由 serve，见 init() 的 modelUrl 参数）。
 *   - sharp：JPEG 解码 + SVG 叠加（画框/文字矢量锐利）+ 重新编码，一个包搞定。
 *     模型权重在 models/ssdlite_mobilenet_v2/（注意：权重文件名无 .bin 后缀，
 *     与 model.json 的 weightsManifest.paths 一致，多文件少字都会导致解码失败）。
 *
 * 用法：
 *   const detector = new Detector();
 *   await detector.init('http://127.0.0.1:8080/models/ssdlite_mobilenet_v2/model.json');
 *   detector.onDetections = (payload) => { ... };  // 检测结果回调（回传手机预览）
 *   const out = await detector.processFrame(jpegBuffer); // null=忙丢帧；jpeg=处理结果
 */

const tf = require('@tensorflow/tfjs');
require('@tensorflow/tfjs-backend-wasm');
const cocoSsd = require('@tensorflow-models/coco-ssd');
const sharp = require('sharp');

/* 类别配色表（与手机端预览画框保持一致） */
const COLORS = [
  '#FF3B30', '#34C759', '#007AFF', '#FF9500',
  '#AF52DE', '#5AC8FA', '#FFCC00', '#FF2D55',
  '#8E8E93', '#FF6B6B', '#4ECDC4', '#55606B',
];

class Detector {
  constructor() {
    this.model = null;          // coco-ssd 模型
    this.enabled = false;       // 检测开关（手机端 ws 文本消息控制）
    this.scoreThreshold = 0.5;  // 置信度阈值（手机端可调）
    this.busy = false;          // 正在处理一帧；忙时 processFrame 返回 null（丢帧）
    this.onDetections = null;   // 有新检测结果时的回调（用于回传手机预览）
    this.stats = { frames: 0, boxes: 0, lastMs: 0 };
  }

  /* 初始化：modelUrl 由调用方提供（cam_server 用 127.0.0.1:HTTP_PORT 的 /models 路由）。
   * 失败不抛致命错误：模型为 null 时 processFrame 退化为纯透传，服务仍可用。 */
  async init(modelUrl) {
    await tf.setBackend('wasm');
    await tf.ready();
    const t0 = Date.now();
    this.model = await cocoSsd.load({ modelUrl });
    console.log(`[检测] 模型加载完成（${Date.now() - t0}ms，后端 ${tf.getBackend()}）`);
    return this;
  }

  setEnabled(on) {
    this.enabled = !!on;
    console.log(`[检测] 开关: ${this.enabled ? '开' : '关'}`);
  }

  setThreshold(t) {
    const v = Number(t);
    if (!isFinite(v)) return;
    this.scoreThreshold = Math.max(0.05, Math.min(0.95, v));
    console.log(`[检测] 阈值: ${this.scoreThreshold}`);
  }

  /*
   * 处理一帧 JPEG。
   * 返回：Buffer(处理后的 JPEG，可能带框) | null(检测忙，调用方应丢弃此帧)
   * 检测关闭 / 模型未加载 / 处理出错：返回原始 jpeg（透传，保证不断流）。
   */
  async processFrame(jpeg) {
    if (!this.enabled || !this.model) return jpeg;
    if (this.busy) return null; /* PC 忙：丢中间帧，最新帧优先，避免延迟滚雪球 */
    this.busy = true;
    try {
      const t0 = Date.now();

      /* 1) JPEG 解码为 RGBA 像素 */
      const { data, info } = await sharp(jpeg)
        .ensureAlpha()
        .raw()
        .toBuffer({ resolveWithObject: true });
      const W = info.width, H = info.height;

      /* 2) coco-ssd 检测（ImageData-like 对象喂 fromPixels） */
      const dets = await this.model.detect(
        { width: W, height: H, data: new Uint8Array(data) },
        20,
        this.scoreThreshold,
      );

      /* 3) 有框：画框后重编码；无框：原帧透传（省一次编码） */
      const out = dets.length
        ? await this._drawAndEncode(data, W, H, dets)
        : jpeg;

      this.stats.frames++;
      this.stats.boxes += dets.length;
      this.stats.lastMs = Date.now() - t0;

      /* 4) 回传手机预览（含空结果，手机端据此清框） */
      if (this.onDetections) {
        this.onDetections({
          w: W,
          h: H,
          detections: dets.map((d) => ({ bbox: d.bbox, class: d.class, score: d.score })),
        });
      }
      return out;
    } catch (e) {
      console.warn('[检测] 处理失败，本帧透传:', e.message);
      return jpeg;
    } finally {
      this.busy = false;
    }
  }

  /* SVG 叠加 + JPEG 重编码 */
  async _drawAndEncode(raw, W, H, dets) {
    const svg = this._buildSvg(W, H, dets);
    return sharp(raw, { raw: { width: W, height: H, channels: 4 } })
      .composite([{ input: svg, top: 0, left: 0 }])
      .jpeg({ quality: 70 })
      .toBuffer();
  }

  /* 生成画框 SVG：矩形 + 顶部色带 + 「类别 置信度」。
   * 线宽/字号随图宽缩放（320 与 640 都清晰）。 */
  _buildSvg(W, H, dets) {
    const esc = (s) => String(s).replace(/[&<>"']/g, (c) => (
      { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;' }[c]
    ));
    const sw = Math.max(2, Math.round(W / 160));   /* 线宽 */
    const fs = Math.max(11, Math.round(W / 22));   /* 字号 */
    const bandH = fs + 8;                          /* 色带高 */
    let body = '';
    dets.forEach((d, i) => {
      const [x, y, w, h] = d.bbox;
      const label = `${d.class} ${Math.round(d.score * 100)}%`;
      const labelW = label.length * fs * 0.62 + 10;
      /* 按类别名 hash 取色：同一类别跨帧颜色稳定，无需维护类别表 */
      const color = COLORS[(d.class.charCodeAt(0) + d.class.length + i) % COLORS.length];
      body += `<rect x="${x}" y="${y}" width="${w}" height="${h}" fill="none" stroke="${color}" stroke-width="${sw}"/>`;
      body += `<rect x="${x}" y="${y - bandH}" width="${labelW}" height="${bandH}" fill="${color}"/>`;
      body += `<text x="${x + 5}" y="${y - 6}" font-family="sans-serif" font-size="${fs}" fill="#ffffff">${esc(label)}</text>`;
    });
    return Buffer.from(
      `<svg width="${W}" height="${H}" xmlns="http://www.w3.org/2000/svg">${body}</svg>`,
    );
  }
}

module.exports = Detector;
