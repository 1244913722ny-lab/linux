# 项目长期备忘（linux-study / file/app LVGL QEMU demo）

## 构建
- 编译必须在 `file/app` 目录执行 **`make all`**（不能只敲 `make`）：Makefile 的 `$(foreach $(eval compile_rule))` 把第一条生成的规则 `build/out/main.c.o` 设成了默认目标，`make` 无参数只编 main.c 就停。
- WSL 编译：`wsl -e bash -lc "cd /mnt/d/linux-study/linux/file/app && make all"`（WSL 内已装 arm-linux-gnueabihf-gcc；工作区里的 gcc-linaro-7.5.0 是 Linux 二进制，Windows 下不能直接跑）。
- 部署/运行：`file/uboot-start` 下 `./cpLVGL.sh` + `./start_uboot.sh`，telnet 127.0.0.1 4321，`/mnt/sdcard` 里 `./lvgl_demo`。
- LVGL v8.3.11，LV_COLOR_DEPTH=32（lv_img TRUE_COLOR = 4B/px ARGB），中文靠 FreeType 运行时加载 /mnt/sdcard/zw.ttf 挂默认字体。

## 项目约定
- 新 demo 模式：写 `xxx_demo.c/.h`（open/close 签名同内置 demo），main.c 的 `g_demos[]` 注册表加磁贴条目，Makefile APP_SRCS 加文件名；open() 在 `lv_scr_act()` 上建 UI，close() 用 `lv_obj_clean(lv_scr_act())`；反复进入需在 open 里完整复位静态状态。
- 网络/阻塞 IO 一律后台线程 + 主线程 lv_timer 轮询刷新（LVGL 非线程安全）；停止线程用 __atomic stop 标志 + shutdown 唤醒阻塞 recv + pthread_join；close 先停线程再停 timer 再释放缓冲。
- 禁用 transform_zoom/angle 做逐帧动画（fbdev 局部刷新下中间帧不提交，见 2026-10-19 日志）；缩放用 tjpgd scale 或静态尺寸。
- LVGL extra 库默认全关（lv_conf.h 只开了 FREETYPE）；用 sjpg/tjpgd 需 `#define LV_USE_SJPG 1`（整个 sjpg 目录被该宏包裹，包括 tjpgd.c）。
- **tjpgd 坑（2026-10-19 花屏事故）**：jd_decomp 的输出回调是**按 MCU 分块回调**的，`rect->left` 是**块内起点**而非整幅图列号。在回调里同时做"放大坐标映射"会写错列 + 越界读块缓冲 → 满屏花屏。正确做法：回调只做 1:1 写入"源区"，放大交给回调外的独立一遍整图循环（scale_up_nearest：每目标行 `sy=dy*sh/dh`，水平用累加器 `acc+=sw; while(acc>=dw){acc-=dw; sx++;}` 推进源列 + 钳位）。帧缓冲分两段 `[0,dw*dh)` 显示区 + `[dw*dh, dw*dh+w*h)` 源区；dsc.w/h 与 dsc.data 必须都指向显示区且尺寸匹配，否则 LVGL 只画左上角一小块。
- **QEMU 里没有实体键盘输入**：需要键盘输入的输入框一律配 LVGL 自带的屏幕虚拟键盘。lv_conf.h 已开 `LV_USE_KEYBOARD 1` + `LV_USE_BTNMATRIX 1`（lv_keyboard 在 src/extra/widgets/keyboard/，整体被 LV_USE_KEYBOARD 包裹，不开则 create/set_textarea 等接口不存在；声明链 lvgl.h → src/extra/lv_extra.h → widgets/lv_widgets.h → keyboard/lv_keyboard.h）。数字模式 LV_KEYBOARD_MODE_NUMBER 按键 = 0-9/点/退格/OK/左右光标，够填 IP 和端口。键盘默认隐藏，点输入框才弹（`lv_keyboard_set_textarea` 绑定 + `LV_KEYBOARD_MODE_NUMBER`），点 OK（监听键盘 LV_EVENT_READY）或"连接"收起；弹出时把设置面板 `lv_obj_align(TOP_MID)` 让位，收起时 `lv_obj_center` 回中，避免遮挡"连接"按钮。按键字体用 `&lv_font_montserrat_24`（纯 ASCII 紧凑；别用中文默认字体，按键太宽排不下）。
- QEMU 网络走用户态 NAT，连 127.0.0.1 永远可用；getaddrinfo 在 -static 下填域名需 glibc NSS 共享库，填 IP 字面量无此问题（ld 会警告，功能不受影响）。
- **本机 WSL 可能被安全策略拉黑**：2026-10-19 上午 `wsl.exe` 命中 Security Center → Program Blacklist，导致我无法跑 `make all` 交叉编译验证，需用户在 WSL 里自行编译确认。

## Camera demo 帧协议（手机/PC 推流服务端按此发送）
`[4 字节大端长度 N][N 字节 JPEG]` 循环发送，0 < N ≤ 512KB；板子作 TCP 客户端，界面填服务器 IP:8888 连接后解码显示。

## 手机网页推流桥接 file/cam_server/（camera_demo 官方图像来源）
- Node 单文件 `cam_server.js`（依赖仅 `ws`，`npm install` 后 `npm start`）：http:8080 返回内嵌网页（getUserMedia 相机 + canvas 抓帧 + toBlob jpeg + ws.send 二进制帧）；ws /ws 收帧；net:8888 等板子连入后加 4B 大端长度头原样转发；板子离线缓存最新帧、连上补发。
- 网页功能：开始/暂停推流、拍一张、前后置切换、320x240~640x480 与 5/10/15fps 可调、ws 断线自动重连、微信内置浏览器检测提示。
- 网络：手机与 PC 同 Wi-Fi 访问 http://<PC同网段IP>:8080（PC 有多个 IPv4，选同网段）；QEMU NAT 下板子填 10.0.2.2:8888；防火墙放行 8080/8888。
- **坑：start_uboot.sh 的 qemu 命令行目前没插网卡**（只有注释里的旧式写法），需加 `-netdev user,id=n1 -device lan9118,netdev=n1`，板子 udhcpc 拿 10.0.2.15。
- **iOS Safari 摄像头坑（2026-10-19）**：Apple 要求 getUserMedia 必须安全上下文，`http://局域网IP` 直接被拒；且失败方式是 `navigator.mediaDevices` 为 undefined → 同步 TypeError（发生在 return promise 之前，调用方 .catch 收不到）→ 页面静默无提示。已在 openCamera 开头加显式检查（显示"需 HTTPS"）并对三个调用点（btnStart/btnSnap/facing.onchange）包 try/catch。iOS 长期方案：自签证书 + iOS 信任（或 frp 隧道 https）；Android Chrome 允许 http 局域网 IP 可直接用。
- **HTTPS 方案（已实施）**：`gen_cert.js`（node-forge 纯 JS 生成，无需 openssl）→ `certs/`（ca.crt.pem + server.crt.pem + server.key.pem，SAN 含本机所有 IPv4+localhost，10 年有效期）。cam_server 检测到 certs/ 自动进 HTTPS 模式：**https:8443** 全功能（推流页 + wss）；**http:8080** 只留 /ca.crt（iOS 装描述文件）、/models/（detector 经 http://127.0.0.1:8080 加载模型）、iOS 引导页（三步：装证书→证书信任→访问 https）；无证书则纯 http 模式（Android 可用）。端口环境变量 CAM_HTTP_PORT/CAM_HTTPS_PORT/CAM_TCP_PORT 覆盖。iOS 装证书流程：Safari 打开 http://IP:8080 → 点"下载 CA 证书"（允许下载描述文件）→ 设置→通用→VPN与设备管理 安装 → 设置→通用→关于本机→证书信任设置 开开关 → 访问 https://IP:8443。PC 换网段后 `FORCE=1 node gen_cert.js` 重生成并重装 CA。
- **frp 隧道 HTTPS（2026-10-19）**：手机经 frp 访问 `https://隧道IP:远程端口` 时报证书无效——SAN 只有局域网 IP。`gen_cert.js` 支持 **EXTRA_SAN**（环境变量或 extra_san.txt，逗号/换行分隔，IP 自动 SAN type 7、域名 type 2）：`FORCE=1 EXTRA_SAN=106.14.219.48 node gen_cert.js` 重签。**CA 默认复用**（certs/ca.key.pem 持久化 CA 私钥），只重签服务器证书时 iOS 不用重装 CA；`FORCE_CA=1` 才重建 CA（iOS 需重装）。
- **node-forge 坑（2026-10-19 实测）**：① `setIssuer/setSubject` 只接受属性数组，传 `cert.subject`（Name 对象）会得到空 issuer、信任链断裂；② `privateKeyToPem` 要传 `keys.privateKey` 不是 keypair 对象（key.n undefined → _bnToBytes 报 "Cannot read properties of undefined (reading 'toString')"）；③ **CA 重建时必须覆盖旧 ca.crt.pem**——旧 crt + 新 key = 公钥不匹配，跨进程读 PEM 验签报 "Encryption block is invalid"（极易误判为签名算法问题，实际是文件没覆盖）；④ node-forge serialNumber 建议 hex 串（随机 serial 防缓存用 `Math.floor(Math.random()*256).toString(16).padStart(2,'0')` 拼）。
- **测试环境坑**：Git Bash 的 curl 默认走系统代理，`--resolve` 测试本地服务会 CONNECT 502（exit 56）——要么 `--noproxy '*'`，要么直接用 Node https.request+ca 校验证书（等价浏览器行为）。
- 已 E2E 自测（ws→TCP 长度头/内容一致、重连补发、HTTP 页面 200/404）。

## 实时目标检测（PC Node + COCO-SSD，板子零改动）
- `detector.js`：`@tensorflow/tfjs`+`tfjs-backend-wasm`+`@tensorflow-models/coco-ssd`+`sharp`；processFrame()：JPEG→RGBA→detect→SVG 画框(类别+置信度,按类别名 hash 配色)→sharp composite→JPEG；busy 丢帧；检测关/出错透传。~85ms/帧。
- 模型本地化：`models/ssdlite_mobilenet_v2/`（model.json + group1-shard1of5..5of5，共 18MB）；cam_server HTTP `/models/` 路由 serve，detector 经 `http://127.0.0.1:8080/models/...` 加载。
- 控制协议：ws 文本 JSON `{"cmd":"detect","enable":bool}` / `{"cmd":"config","scoreThreshold":0.1~0.8}`，回 `{"type":"detect_state"}`；检测结果 `{"type":"detections",w,h,detections:[{bbox,class,score}]}` 回传手机预览画框。
- 网页：object-fit 必须 **fill**（与 drawImage 拉伸一致，overlay 坐标才对齐）；overlay canvas 按 dpr 画框，颜色表与 PC 端一致。
- **tfjs 三坑**：① tfjs-node 在 Node 22 无预编译装不上→纯 WASM 栈；② 纯 tfjs fetch(undici) 不支持 file://→必须 HTTP 加载模型；③ 权重文件名**无 .bin 后缀**（与 weightsManifest.paths 一致），存错名会 404→空 buffer→解码报 "should have 1 values but has 0"。
