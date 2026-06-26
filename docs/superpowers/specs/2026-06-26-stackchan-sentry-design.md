# StackChan 哨兵模式 (Sentry Mode) 设计规格

- 日期: 2026-06-26
- 分支: `feature/sentry-mode`
- 设备: M5Stack StackChan (CoreS3 / ESP32-S3)
- 状态: 设计已与用户确认，待用户评审本文档

## 1. 目标与范围

把 StackChan 改造成一个**哨兵 / 安防机器人**：监控指定区域，当摄像头检测到**有人脸靠得太近**时：
1. 本地发出**可见警告**——转头盯住对方 + 红灯闪烁 + 屏幕显示「非请勿动」；
2. **拍下对方面部照片**；
3. 通过 **Telegram** 把照片+报警推送到用户手机。

仅用于监控用户**自有**区域；按当地法规可能需要张贴录像告示。

### 非目标（本期不做）
- 人脸识别白名单（区分主人/熟人）—— 靠布防倒计时规避自拍。
- PIN 码撤防（本期点屏幕即可撤防）。
- 头顶接近寄存器 (0x12) 触发（已确认跳过）。
- microSD 本地存档（后续可加）。
- 自建服务器 / 官方 App 集成（改用 Telegram 直推）。
- 声音警告（本期静音；接口预留）。

## 2. 硬件现状与关键约束

- 主控 ESP32-S3：240MHz 双核，16MB Flash，8MB PSRAM，Wi-Fi。
- 摄像头：0.3MP DVP，V4L2，典型 320x240，`Capture()` 输出 RGB565。
- **「接近传感器」实为头顶电容触摸**（LTR-507 类，I2C `0x68`）：固件仅读触摸寄存器；proximity 寄存器 (0x12) 未实现、量程厘米级、朝上 → **不适合区域侦测**。**结论：用摄像头做侦测。**
- 舵机：两个 SCS(SMS_STS) 串行舵机，yaw ±128°、pitch 3°~87°，Motion API 带弹簧动画。
- 12 颗 RGB 灯（PY32 IO 扩展，左6右6）。
- 扬声器（AW88298）可播 OGG/Opus（本期不用）。
- 联网：固件自带 `esp_http_client` + TLS（`esp_crt_bundle`），可直接 HTTPS 访问外部 API。
- 应用框架：**Mooncake**，每个 app 继承 `mooncake::AppAbility`，生命周期 `onCreate/onOpen/onRunning/onClose`，在 `main.cpp` 用 `GetMooncake().installApp()` 注册。

## 3. 总体架构

新增一个 Mooncake 应用 `app_sentry`（从 `app_template` 复制）。它持有一个状态机，在 `onRunning()` 中按节流推进，重活放在自有 FreeRTOS 工作线程中：

| 状态 | 行为 | 迁移 |
|---|---|---|
| `DISARMED` 待命 | 显示说明，不监控 | 打开 App → `ARMING` |
| `ARMING` 布防倒计时 | 屏幕倒计时（默认10s），让用户离开镜头 | 倒计时结束 → `ARMED` |
| `ARMED` 警戒中 | 每 `DETECT_INTERVAL_MS` 取帧→人脸检测 | 连续 `CONSECUTIVE_HITS` 帧命中「有人脸且够近」→ `ALERTING` |
| `ALERTING` 报警 | 执行报警动作序列 (§5) + 拍照上传 (§6) | 完成 → `COOLDOWN` |
| `COOLDOWN` 冷却 | `COOLDOWN_S` 内不再报警，要求人脸离开后才重新计数 | 冷却结束且无脸 → `ARMED` |

- 任意状态点屏幕 → 撤防回 `DISARMED` / 退出 App。
- 线程：侦测与拍照在工作线程内**串行**，避免与 LVGL/相机缓冲竞争；所有 UI 更新用 `LvglLockGuard` 包裹。相机帧在下次 `Capture()` 时释放，故「命中→对当前帧编码 JPEG」必须在同一帧内完成。

## 4. 侦测逻辑

- 模型：**`espressif/human_face_detect` v0.5.0**（自动带入 `esp-dl ~3.3.x`；要求 IDF ≥5.3，我们 5.5.4 满足）。默认模型 **MSRMNP_S8_V1 ≈ 187KB**，以 RODATA 嵌入 app 二进制，**无需新增分区**（OTA 槽各 ~5MB 够用）。
- 用法（已对照 esp-dl 当前 master 核实）：构造 `dl::image::img_t`（指向相机帧 + 宽高 + `pix_type`）→ **只构造一次** `HumanFaceDetect`（含模型权重，很重）→ `auto& results = detect.run(img);` 返回 `std::list<dl::detect::result_t>&`，每个 `result_t` 有 `box`[x1,y1,x2,y2]、`score`、`box_area()`。在 **≥8KB 栈**的独立任务里跑；默认模型单帧 ~44ms（>20FPS）。resize 由 run() 自动完成，返回的 box 已映射回输入帧尺寸。
- ⚠️ **相机像素格式**：sdkconfig 当前为 `CAMERA_GC0308_DVP_YUV422`（YUYV），非 RGB565。esp-dl 直接支持 `DL_IMAGE_PIX_TYPE_YUYV`。Phase 1 需确认 `Capture()/GetFrameData()` 给出的是原始 YUYV 还是已转 RGB565 的显示缓冲，据此设 `pix_type`（RGB565 用 `DL_IMAGE_PIX_TYPE_RGB565LE`）。
- 取帧：`auto* cam = hal_bridge::board_get_camera(); cam->Capture();` 然后 `cam->GetFrameData()/GetFrameWidth()/GetFrameHeight()/GetFrameFormat()`。
- 命中判据：
  - 存在人脸且 `score ≥ MIN_FACE_SCORE`（默认 0.50）；
  - **「够近」**：人脸框面积 / 整帧面积 ≥ `FACE_AREA_THRESHOLD_PCT`（默认 18%），取最大的人脸；
  - 去抖：连续 `CONSECUTIVE_HITS`（默认 2）帧满足。
- 帧率：约 3 FPS，省电省算力。

## 5. 报警动作序列（用户所选：转头 + 红灯 + 屏幕，静音）

触发后按序执行：
1. **转头对准**：由人脸中心算归一化坐标 → `GetStackChan().motion().lookAtNormalized(nx, ny, speed)`，把对方摆到画面中央，利于拍清脸。
2. **红灯闪烁**：`GetHAL().showRgbColor(255,0,0)` 闪数次（或 NeonLight `setColor` 动画）。
3. **屏幕警告**：切到红色警告画面，显示 `WARNING_TEXT`（默认「非请勿动」）。
4. **拍照**：对触发帧用 `image_to_jpeg_cb`（quality 80）编码 JPEG。
5. **上传 Telegram**（§6）。
6. **复位**：灯/屏幕恢复，进入 `COOLDOWN`。

声音预留：未来可 `hal_bridge::app_play_sound(OGG_...)` 加蜂鸣/语音。

## 6. Telegram 投递

- 端点：`https://api.telegram.org/bot<TOKEN>/sendPhoto`
- 方法：HTTP POST，`multipart/form-data`，字段 `chat_id`、`photo`(JPEG 二进制)、`caption`(「⚠ 哨兵报警」+ 时间戳)。
- 实现：`auto http = Board::GetInstance().GetNetwork()->CreateHttp(0); http->SetHeader("Content-Type","multipart/form-data; boundary=...");`，分块写入 body，`http->Open("POST", url)`，校验 `http->GetStatusCode()`。TLS 用内置 `esp_crt_bundle`。
- 发送前用 `Hal::getWifiStatus()` 确认联网；失败重试 2 次并在屏幕提示。
- 时间戳：PCF8563 RTC / SNTP（若已校时）。

## 7. 配置项（新增 Kconfig，写入 `firmware/sdkconfig.defaults.local`，git 忽略）

| Kconfig | 默认 | 说明 |
|---|---|---|
| `SENTRY_TELEGRAM_BOT_TOKEN` | "" | Bot token（@BotFather 获取）|
| `SENTRY_TELEGRAM_CHAT_ID` | "" | 接收报警的 chat id |
| `SENTRY_WARNING_TEXT` | "非请勿动" | 屏幕警告文字 |
| `SENTRY_FACE_AREA_THRESHOLD_PCT` | 18 | 「够近」人脸占画面面积百分比 |
| `SENTRY_MIN_FACE_SCORE` | 50 | 最小人脸置信度（百分比）|
| `SENTRY_CONSECUTIVE_HITS` | 2 | 连续命中帧数 |
| `SENTRY_DETECT_INTERVAL_MS` | 350 | 侦测间隔 |
| `SENTRY_ARM_GRACE_S` | 10 | 布防倒计时 |
| `SENTRY_COOLDOWN_S` | 30 | 报警冷却 |

## 8. 代码改动点（文件级）

新增：
- `firmware/main/apps/app_sentry/app_sentry.{h,cpp}` —— 应用主体 + 状态机。
- `firmware/main/apps/app_sentry/sentry_detector.{h,cpp}` —— 封装 esp-dl 人脸检测 + 「够近」判定。
- `firmware/main/apps/app_sentry/telegram_client.{h,cpp}` —— multipart 上传封装。
- `firmware/main/apps/app_sentry/view/` —— LVGL 警告/状态 UI。
- `firmware/main/assets/assets_bin/icon_sentry.bin` —— 启动器图标。

修改：
- `firmware/main/apps/apps.h` —— `#include "app_sentry/app_sentry.h"`。
- `firmware/main/main.cpp` —— `GetMooncake().installApp(std::make_unique<AppSentry>())`。
- `firmware/main/Kconfig.projbuild` —— 新增 §7 配置项。
- `firmware/main/idf_component.yml` —— 增加依赖：
  ```yaml
  espressif/human_face_detect:
    version: "^0.5.0"
    rules:
    - if: target in [esp32s3]
  ```
- `firmware/main/CMakeLists.txt` —— 注册新源文件 + 图标资源（如需要）。

## 9. 构建与烧录工作流

- 工具链：ESP-IDF v5.5.4（装于 `C:\esp\esp-idf`）。
- 依赖：`cd firmware; python fetch_repos.py`（拉 xiaozhi-esp32 v2.2.4 等，约 500MB）。
- 配置：`idf.py set-target esp32s3`；token/chat_id 等写入 `firmware/sdkconfig.defaults.local`。
- 编译：`idf.py build`。
- **烧录（首次必须 USB-C）**：`idf.py -p COMx flash monitor`（COMx 在设备插入后识别；需 CP210x/CH340 驱动）。
- 无线更新（后续可选）：固件支持 OTA（`ota_0/ota_1` 各 5MB）；设 `CONFIG_OTA_URL` + 提供 `.bin` 后可 Wi-Fi 推送；调试日志仍建议 USB。
- 注意：Windows 路径要短（已用 `C:\esp`）；杀软可排除 `build/` 目录加速。

## 10. 风险与先验证（de-risk）

1. **esp-dl 集成（风险已大幅降低）**：研究已确认默认模型仅 ~187KB(RODATA)、**无需改分区**、单帧 ~44ms。Phase 1 仍先单独跑通 `human_face_detect`：串口打印 box/score/面积占比，并确认相机像素格式(YUYV vs RGB565)，再接报警逻辑。
2. **相机独占**：avatar/视频也用相机；哨兵运行时独占，同一帧内完成检测+编码。
3. **帧格式**：`Capture()` 输出 RGB565（可能因旋转交换宽高）；需按实际 `GetFrameFormat()`/宽高构造 esp-dl 输入。
4. **Telegram 可达性**：设备所在网络需能访问 `api.telegram.org`；失败有重试+提示。
5. **误报**：布防倒计时 + 连续命中去抖 + 冷却共同降噪。

## 11. 实施阶段

- **Phase 0**：工具链就绪，`fetch_repos`，烧录**原厂固件**，找到 COM 口，串口看日志（验证整链路）。
- **Phase 1（去风险）**：新增 `app_sentry` 骨架并注册；集成 esp-dl；取帧→人脸检测→串口打印框/面积。
- **Phase 2**：报警动作（转头 + 红灯 + 屏幕「非请勿动」）。
- **Phase 3**：拍照 → Telegram 上传（先硬编码 token 跑通，再移到 Kconfig）。
- **Phase 4**：状态机完善（布防倒计时/撤防/冷却）+ 实地调阈值；可选 OTA。

## 12. 验收标准

- 打开哨兵 App → 10s 倒计时 → 警戒。
- 一个人走到镜头前（约对应 18% 面积的距离），2 帧内：StackChan 转头盯住、红灯闪、屏幕显示「非请勿动」、手机 Telegram 收到带人脸照片，全程 ≤ 数秒。
- 30s 冷却内不刷屏；人离开后可再次触发。
- 点屏幕可撤防。
