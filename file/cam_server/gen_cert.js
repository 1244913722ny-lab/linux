/**
 * gen_cert.js —— 生成 cam_server 用的自签证书（iOS Safari 摄像头必须 HTTPS）
 *
 * 用法：
 *   node gen_cert.js                         服务器证书已存在则跳过
 *   FORCE=1 node gen_cert.js                 重签服务器证书（CA 复用，手机不用重装）
 *   FORCE=CA=1 node gen_cert.js              连 CA 一起重建（手机上需重新安装 CA）
 *   FORCE=1 EXTRA_SAN=106.14.219.48,foo.com node gen_cert.js
 *                                             重签并追加隧道 IP/域名到 SAN
 *
 * 额外 SAN 也可写在文件 extra_san.txt 里（一行一个，支持逗号分隔），
 * 适合 frp 隧道服务器 IP/DDNS 域名这种"不属于本机网卡"的访问地址。
 *
 * 输出（cam_server/certs/）：
 *   ca.crt.pem      根 CA 证书 —— 发给手机安装（iOS: 下载描述文件 -> 安装 -> 证书信任）
 *   ca.key.pem      根 CA 私钥 —— 只存本地！泄漏 = 任何人都能签你信任的证书
 *   server.crt.pem  服务器证书（CA 签发，SAN = 本机所有 IPv4 + localhost + EXTRA_SAN）
 *   server.key.pem  服务器私钥
 *
 * SAN 为什么必须含 IP/域名：iOS/现代浏览器只认 subjectAltName，完全忽略 CN，
 * 访问地址不在 SAN 里就报 NET::ERR_CERT_COMMON_NAME_INVALID（"证书无效"）。
 * frp 隧道场景：手机访问 https://隧道IP:远程端口，SAN 里必须有那个隧道 IP。
 */
'use strict';

const forge = require('node-forge');
const fs = require('fs');
const path = require('path');
const os = require('os');

const CERT_DIR = path.join(__dirname, 'certs');
const CA_CRT = path.join(CERT_DIR, 'ca.crt.pem');
const CA_KEY = path.join(CERT_DIR, 'ca.key.pem');
const CRT_PATH = path.join(CERT_DIR, 'server.crt.pem');
const KEY_PATH = path.join(CERT_DIR, 'server.key.pem');
const EXTRA_SAN_FILE = path.join(__dirname, 'extra_san.txt');

const CN = 'CamServer Local CA';

if (fs.existsSync(CRT_PATH) && !process.env.FORCE) {
  console.log('服务器证书已存在，跳过（FORCE=1 重签 / FORCE=CA=1 连同 CA 重建）');
  process.exit(0);
}

/* ---------- 收集 SAN：本机 IPv4 + localhost + EXTRA_SAN（环境变量与 extra_san.txt） ---------- */

const ips = ['127.0.0.1'];
for (const name of Object.keys(os.networkInterfaces())) {
  for (const it of os.networkInterfaces()[name]) {
    if ((it.family === 'IPv4' || it.family === 4) && !it.internal) ips.push(it.address);
  }
}

/* 额外地址：frp 隧道 IP、DDNS 域名等不属于本机网卡的访问方式 */
let extra = [];
if (process.env.EXTRA_SAN) extra.push(...process.env.EXTRA_SAN.split(','));
if (fs.existsSync(EXTRA_SAN_FILE)) {
  extra.push(...fs.readFileSync(EXTRA_SAN_FILE, 'utf8').split(/[\s,]+/));
}
const extraClean = [...new Set(extra.map((s) => s.trim()).filter(Boolean))];

/* IPv4 字面量 -> SAN type 7（IPAddress）；其余按域名 type 2（DNS） */
const isIp = (s) => /^\d{1,3}(\.\d{1,3}){3}$/.test(s);
const altNames = [
  ...[...new Set(ips)].map((ip) => ({ type: 7, ip })),
  { type: 2, value: 'localhost' },
  ...extraClean.map((s) => (isIp(s) ? { type: 7, ip: s } : { type: 2, value: s })),
];
console.log('SAN 将包含:', altNames.map((a) => a.ip || a.value).join(', '));
if (extraClean.length === 0) {
  console.log('提示：frp 隧道/DDNS 访问需要 EXTRA_SAN，例如 FORCE=1 EXTRA_SAN=106.14.219.48');
}

const now = new Date();
const expire = new Date(now);
expire.setFullYear(expire.getFullYear() + 10); /* 10 年，免去频繁重签 */

/* ---------- CA：默认复用（手机装过一次就不用再装），FORCE_CA=1 才重建 ---------- */

let caCert, caKeys;
let caRegenerated = false;
if (fs.existsSync(CA_CRT) && fs.existsSync(CA_KEY) && !process.env.FORCE_CA) {
  caCert = forge.pki.certificateFromPem(fs.readFileSync(CA_CRT, 'utf8'));
  caKeys = { privateKey: forge.pki.privateKeyFromPem(fs.readFileSync(CA_KEY, 'utf8')) };
  console.log('复用已有 CA（手机无需重装；FORCE_CA=1 强制重建）');
} else {
  caRegenerated = true; /* 新建的 CA 必须连同证书一起落盘，否则 crt/key 不匹配 */
  console.log('生成 CA 密钥对（RSA2048）...');
  const generated = forge.pki.rsa.generateKeyPair(2048);
  caKeys = generated;
  caCert = forge.pki.createCertificate();
  caCert.publicKey = caKeys.publicKey;
  caCert.serialNumber = '01';
  caCert.validity.notBefore = now;
  caCert.validity.notAfter = expire;
  /* setIssuer/setSubject 只接受属性数组（[{name,value},...]）；
   * cert.subject 是 Name 对象（.attributes 才是数组）——直接传对象会得到空 issuer */
  caCert.setSubject([{ name: 'commonName', value: CN }]);
  caCert.setIssuer([{ name: 'commonName', value: CN }]); /* 自签 */
  caCert.setExtensions([
    { name: 'basicConstraints', cA: true }, /* CA 标志，iOS 才会认作可信任根 */
  ]);
  caCert.sign(caKeys.privateKey, forge.md.sha256.create());
}

/* ---------- 服务器证书：每次重签，serial 随机（避免 iOS/浏览器缓存旧证） ---------- */

/* node-forge 的 serialNumber 约定是 hex 字符串（certificateToPem 按 hex 编解码），
 * 传十进制大数会 PEM 往返后 TBS 不一致，表现为 ca.verify 报
 * "Encryption block is invalid"——签名其实没问题，是序列号编解码不对称 */
const serialHex = Array.from({ length: 8 }, () =>
  Math.floor(Math.random() * 256).toString(16).padStart(2, '0')
).join('');

console.log('生成服务器密钥对并让 CA 签发...');
const srvKeys = forge.pki.rsa.generateKeyPair(2048);
const srvCert = forge.pki.createCertificate();
srvCert.publicKey = srvKeys.publicKey;
srvCert.serialNumber = serialHex;
srvCert.validity.notBefore = now;
srvCert.validity.notAfter = expire;
srvCert.setSubject([{ name: 'commonName', value: 'cam-server' }]);
srvCert.setIssuer([{ name: 'commonName', value: CN }]);
srvCert.setExtensions([
  { name: 'basicConstraints', cA: false },
  { name: 'keyUsage', digitalSignature: true, keyEncipherment: true },
  { name: 'extKeyUsage', serverAuth: true },
  { name: 'subjectAltName', altNames },
]);
srvCert.sign(caKeys.privateKey, forge.md.sha256.create());

if (!caCert.verify(srvCert)) {
  console.error('内部错误：CA 无法验证刚签发的服务器证书');
  process.exit(1);
}

fs.mkdirSync(CERT_DIR, { recursive: true });
/* CA 重建过就必须覆盖旧文件（旧 crt + 新 key = 公钥不匹配，验签报
 * "Encryption block is invalid"——这个报错极易误判为签名问题） */
if (caRegenerated || !fs.existsSync(CA_CRT)) fs.writeFileSync(CA_CRT, forge.pki.certificateToPem(caCert));
if (caRegenerated || !fs.existsSync(CA_KEY)) fs.writeFileSync(CA_KEY, forge.pki.privateKeyToPem(caKeys.privateKey));
fs.writeFileSync(CRT_PATH, forge.pki.certificateToPem(srvCert));
fs.writeFileSync(KEY_PATH, forge.pki.privateKeyToPem(srvKeys.privateKey));

console.log('完成，输出目录:', CERT_DIR);
console.log('  ca.crt.pem      ', fs.statSync(CA_CRT).size, 'bytes  <- 手机安装这个');
console.log('  ca.key.pem      ', fs.statSync(CA_KEY).size, 'bytes  <- CA 私钥，勿外泄');
console.log('  server.crt.pem  ', fs.statSync(CRT_PATH).size, 'bytes');
console.log('  server.key.pem  ', fs.statSync(KEY_PATH).size, 'bytes');
console.log('重启 cam_server 生效；换了访问地址（新网段/新隧道 IP）后重跑 FORCE=1 再重启。');
