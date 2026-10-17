# Mosaico · Octop 屏显伙伴 项目说明书

> 面向二次开发/维护者的技术说明，描述**当前代码的真实形态**（Octop 后端、纯屏显）。标注 `文件:行号` 可直接跳转核对。
> 改造前的设计报告：[docs/OCTOP_MIGRATION_CN.md](OCTOP_MIGRATION_CN.md)；施工计划与实测证据：仓库根 [OCTOP_WORK_PLAN_CN.md](../OCTOP_WORK_PLAN_CN.md)；构建与验收：[docs/BUILDING.md](BUILDING.md)；手机配置：[docs/SETUP_CN.md](SETUP_CN.md)。
> 文档更新：2026-10-17。改造前的 Muse 形态说明书已不在构建文档里，仅作历史留档（见 §14）。

## 1. 这是什么

一块 **ESP32-S31（ESP-Mosaico 裸机）** 上的按住说话（push-to-talk）AI 伙伴固件：

- 480×480 RGB565 **CO5300 QSPI LCD + cst9220 电容触摸**，不是墨水屏。
- 顶部一颗**红色 AI 键**：按住录音、松开发送；对话期间再按住即打断。
- **大脑、耳朵、显示内容都来自同一个自托管 Octop**：`POST /api/voice/stt` 转写语音，`WS /api/agents/{a}/chat/ws` 流式对话。
- **不发声**。`companion_tts.c` / `companion_qwen.c` / `companion_pcm.c` 与功放播放链路已整条删除，回复只显示在屏幕上；因此屏显排版（翻页、行数、截断）取代朗读成为本项目的核心交互。
- 角色形象：8 种动画姿态，可由 Octop 生成新角色替换（[docs/CHARACTERS_CN.md](CHARACTERS_CN.md)）。

一句话原理：

> 设备把 16 kHz 单声道 PCM 录进 PSRAM 的 30 秒裁剪区，松开按键后 POST 给 Octop 换回一行文字，再把这行文字作为 `user_turn` 帧发进同一条 WebSocket；Octop 以 `token` 帧流式返回，设备边收边上屏，`done` 帧收尾。所有出网都走设备自己的 Wi-Fi STA，没有 BLE，没有 Noise，没有隧道网卡。

## 2. 模块地图

```
仓库根
├─ mosaico.py                     产品 CLI 启动器（真身在子模块里）
├─ .mosaico.json                  工作区清单：设备表 esp32s31、依赖路径、build.runner=builtin
├─ DEPENDENCIES.json              上游 commit 锁定（idf / gsp 1.4.0 / 子模块 SHA）
├─ tools/bootstrap.py             子模块引导
├─ firmware/                      改造前的旧整包，已 .gitignore、不随公开仓库分发，见 §14
├─ docs/                          BUILDING / SETUP_CN / CHARACTERS_CN / 本说明书 / OCTOP_MIGRATION_CN
└─ projects/muse_companion/       ← 应用本体（工程名保留 muse_companion）
   ├─ build_octop.ps1              唯一受支持的构建入口，编码了本机 4 条环境约束
   ├─ main/                        应用层（自研，Apache-2.0），约 1.6k 行
   │   main.c (72)                  启动编排：Recovery 守护 → 凭据 → 门户 → UI → 运行时
   │   companion_model.c (121)      纯函数状态机，无硬件依赖，可在 PC 上单测
   │   companion_ui.c (451)         GSP 场景渲染、翻页、动画绑定
   │   companion_runtime.c (313)    麦克风、按键、IMU、NVS 偏好、翻页节拍、任务编排
   │   companion_setup.c (203)      192.168.4.1 配置门户 + httpd（4 条路由）
   │   companion_generation.c (152) 角色生成的设备侧 RPC 服务（走 Octop 文本通道）
   │   board_display.c (75)         CO5300 / 触摸板级适配
   │   setup.html                   门户单页（中英双语，无外链资源）
   ├─ components/                  约 1.7k 行
   │   ├─ companion_credentials/    凭据 v2 结构、校验与 NVS 存储（127 行）
   │   └─ octop_link/               唯一的后端组件（1478 行）
   │       octop_wifi.c (302)        AP+STA 无线电、扫描、试连、退避重连
   │       octop_http.c (363)        URL 解析、JSON 请求、`/api/auth/login`
   │       octop_stt.c (89)          multipart 上传 → `{"text":…}`
   │       octop_ws.c (189)          聊天 socket、分帧重组、超大帧整帧丢弃
   │       octop_link.c (535)        会话状态机：登录 → agent → thread → WS → 轮次
   ├─ ui/                          GSP 场景 JSON + 角色 PNG + 字体（生成物，勿手改）
   ├─ pc/                          raylib 模拟器适配层 + companion_model_test.c
   ├─ tools/                       prepare_ui.py / generate_character.py / preview_character.py / tests
   ├─ vendor/                      ⚠ 已退出构建：旧 muse_link_sdk、noise_core、product_info
   ├─ partitions.csv               16 MB Flash 分区（本次改造未改动一个字节）
   └─ sdkconfig.defaults           芯片/PSRAM/显示；BLE、HomeHub、Noise 配置已删
```

改造前 `vendor/` 那三个组件约 2.8 万行，现在一行都不进镜像；应用与链路合计约 3.3k 行。

## 3. 启动流程（`main/main.c:24-72`）

1. `esp_iris_boot_probe()`（容忍失败）与 `nvs_flash_init()`。
2. `iris_ota_support_start()`：**外部 UI 镜像缺失或损坏时 Recovery 仍然可达**，这是刷机安全网，必须排在 UI 之前。
3. `companion_credentials_init()`：读 `companion_keys/tokens` blob，版本或长度不符即视为未配置。
4. `companion_setup_start()`：未配置时开热点 + httpd；已配置时只保留"手机配置"入口。
5. `ui_bundle_open()` 打开 GSP 资源包；失败就打日志并 `return`，**不阻塞 Recovery RPC**。
6. `board_display_init` / `board_touch_init` → `esp_gsp_esp_lcd_start`。这里有两个刻意的配置：`task_stack_psram=true`（渲染回调只做偏好写入入队，所以可以占 PSRAM）与 `task_stack_size_freetype=64 KiB`（中文长回复 + FreeType 16 KiB 灰度池会撑爆默认 24 KiB 渲染栈）。
7. `companion_setup_active(NULL,NULL)` 为假才 `companion_runtime_start()`：门户模式下不开麦克风、不起链路。
8. `companion_ui_create()` 绑定场景。

## 4. 运行时架构：3 个自有任务 + 3 条队列

| 任务 | 栈 | 优先级 | 职责 | 绝不 |
|---|---|---|---|---|
| `companion_mic` | 4096（PSRAM） | 5 | 从 ES8311 读 16 kHz / 256 帧 PCM，投 `audio` 队列 | 不碰网络、不碰 NVS |
| `companion_voice` | 8192 | 4 | 唯一调用 `octop_turn_*` 的任务，拥有全部阻塞决策与超时 | 不做 UI 渲染 |
| `octop_link` | 8192 | 5 | Wi-Fi 上线、登录、agent/thread 探测、WS 收发、轮次推进 | 不读麦克风 |

队列深度：`audio` 48、`control` 12、`messages` 2（`companion_runtime.c:24-30`）。`messages` 每个槽都携带一整条回答，深度直接是 PSRAM 成本；渲染任务每 tick 抽干全部槽位，流式回答每秒最多发布 5 次，所以 2 个槽已足够覆盖一次突发。UI 与后端之间的接缝只有 `main/companion_platform.h` 的 11 个函数；`companion_model.c` 是纯函数状态机，所以 PC 单测能覆盖它。

世代号 `generation` 是这套并发唯一的正确性来源：每次新轮次自增，迟到的回复或错误因世代不匹配被丢弃，`octop_turn_event()` 另外丢弃已取消轮次的事件。

## 5. Octop 链路：状态与端点契约

链路状态（`octop_link.h`）：`OCTOP_NOT_SET → OCTOP_OFFLINE / OCTOP_UNTESTED / OCTOP_TESTING → OCTOP_REACHABLE`，失败侧 `OCTOP_UNREACHABLE`。设置页显示的就是这个枚举 + 一句 48 字内的中文详情。

设备侧只用到 5 个端点（对用户的自托管实例实测，逐条证据在工作计划 §7；**真实地址与账号不写进仓库任何文件**）：

| 用途 | 请求 | 关键点 |
|---|---|---|
| 登录 | `POST /api/auth/login` | `{username,password}` → `{access_token}`；实测 `exp-iat=86400`，24 h 有效，20 h 主动重登 |
| 找 agent | `GET /api/agents` | 读 `agent_id`（不是 `name`），`state=="running"` 才用 |
| 取线程 | `GET/POST /api/agents/{a}/threads` | 列表里挑 `is_active`，没有就新建；`thread_id` 存 NVS，是唯一对话锚点 |
| 转写 | `POST /api/voice/stt` | multipart 字段 `audio`（16 kHz / 16 bit / mono WAV）→ `{"text":…}`，`language` 可省 |
| 对话 | `WS {base}/api/agents/{a}/chat/ws?token=<JWT>` | **token 只能放 query**；`Authorization` 头与 `Cookie` 头实测 403 |

WS 下行帧处置（`octop_ws.c` 收帧、`octop_link.c` 判型）：

| 帧 | 处置 |
|---|---|
| `token` | 追加进回复缓冲 → 上屏（`streaming=true`） |
| `reasoning` | 丢弃（与 `token` 同形，但不是回复） |
| `state_update` / `state_snapshot` / `usage` | 丢弃但**保持连接**；`state_snapshot` 实测可达 21 KiB，超过帧缓冲就整帧跳过，而不是重组完再丢 |
| `done` | 收尾，最后一帧文字标为 `streaming=false` |
| `error` | 本轮失败，`text` 原样上屏 |

看门狗（`octop_link.c:31-32` 与 `:385-400`）：45 s 内连一帧都没有 → 发取消帧并提示"等了 45 秒没有回答，请重试"；已开始后 60 s 无新帧 → 取消并提示"回答中断"。**这不是过度设计**：实测长文请求首帧可拖到 13 s，而半开 socket 会吞掉 `user_turn` 且不报错。

## 6. 一次对话的端到端时序

1. 按住 AI 键，`companion_voice` 先问 `octop_turn_ready()`（凭据齐 + Wi-Fi up）。不满足就直接给"网络未连接，请稍后重试"或"Octop 还没就绪，请检查配置或网络"，**不出网**。
2. `octop_turn_begin()` 打开 PSRAM 960 KB 裁剪区（`CLIP_MAX = 30 * 16000 * 2`，`octop_link.c:25-28`），`companion_mic` 的帧经 `octop_turn_audio()` 写入；超过 30 s 的帧丢弃，短于 `CLIP_MIN = 3200` 帧按"太短"处理。
3. 松开：`octop_turn_end()` → `octop_link` 任务补 WAV 头、`POST /api/voice/stt` 得到 `OCTOP_EV_HEARD`（听到的文字先上屏），再把这行文字作为 `user_turn` 发进 WS。
4. `token` 帧以 `streaming=true` 刷进 `companion_message_t`；`done` 帧给最终文字（`streaming=false`）。
5. `companion_model.c` 走 `COMP_LISTENING → COMP_THINKING → COMP_RECEIVING → COMP_IDLE`。`COMP_RECEIVING` 刻意继承旧"说话"姿态的序号（`companion_model.h:7-10`），所以 GSP 场景不用重排资源。
6. 对话期间再按住 = 打断：`octop_turn_cancel()` 丢裁剪区并向服务端发取消帧，之后到达的事件按世代丢弃。

文字提问（Iris RPC）与语音共用同一条链路：`octop_text_begin()` + `octop_text_observer()`；角色生成就走这条路（§11）。

## 7. 屏显与翻页（本项目新的核心工作量）

| 常量 | 值 | 含义 | 出处 |
|---|---|---|---|
| `COMP_REPLY_CAP` | 65536 | 屏上保留的回复字节上限，与链路侧 `REPLY_CAP` 对齐；约 95 页 | `companion_model.h:24` |
| `COMP_LINE_PX` | 440 | 一行留给文字的像素宽度 | `companion_pager.h:19` |
| `COMP_PAGE_LINES` | 10 | 每页行数（正文框 444×300，20 px 字号行高 30 px） | 同上 |
| `COMP_PAGE_BYTES` | 1024 | 单页输出缓冲，最坏情况（整行空格）889 B + 换行仍放得下 | 同上 |

排版按**像素**而不是按字数核定：Noto Sans SC 20 px 下汉字字宽正好 20 px（一行 22 字、一页 220 字），ASCII 平均 10.8 px（本实现按 12 px、空格 5 px 估算），所以拉丁文会排得更密而不会被裁掉。切页只在 UTF-8 码点边界发生，页首落在 `0x80..0xBF` 的情况被排除，因此不会出现半个汉字变豆腐块；`comp_page_text()` 在同样的 440 px 边界上插入硬换行，页与行的模型共用一套宽度估算，**一页永远不会需要第 11 行**。

流式期间**不自动翻页**（否则用户正在读的一页会跳走）；`done` 之后按"翻页间隔"节拍自动翻，翻到最后一页停止。阅读页的三种操作：轻触正文翻下一页、左右横滑（≥64 px 且横向占优）翻页、长按正文或点「回首页」返回；横滑由 `esp_gsp_set_pointer_observer()` 提供坐标，识别到滑动后用 `swipe_held` 吞掉紧随其后的原生 click，避免一次手势翻两页。页码 `第 x / y 页`、`‹ 上一页` / `下一页 ›` 按边界显隐。

熄屏：从 `sleep_seconds*1000 - DIM_TRANSITION_MS(2000)` 起线性调暗，最低 `DIM_BRIGHTNESS(8)`；到点且无回复时进 `COMP_SLEEPY`（`companion_model.c:6,92`）。触摸或按键唤醒；有回复时不会变困倦表情，熄屏期间不自动翻页。

## 8. 凭据、配网与首次配置

**结构**（`companion_credentials.c`，`CREDENTIALS_VERSION = 2`）：`octop_url[256]`、`username[64]`、`password[128]`、`stt_url[256]`、`stt_key[256]`，存在 NVS 命名空间 `companion_keys` 的 blob `tokens`。版本或总长不符 → 全零，等同未配置；**旧 v1 记录会被自动清掉**，不会被误当凭据。

**校验规则**（`companion_credentials_valid`）：URL 必须 `http://` 或 `https://` 开头，且无空格 / `@` / `#`；用户名无空白；密码允许空格（只裁首尾）；`stt_url` / `stt_key` 可空——**空串与 NULL 都算有效**，因为落盘快照把"未填写"写成 `""`，若把 `""` 判非法，一次留空保存就会让设备永久回到门户。

**门户**（`companion_setup.c`）：热点 `Mochi-Setup-%02X%02X`（MAC 后两字节，开放网络，`:178`），AP + STA 并存（`WIFI_MODE_APSTA`），固定 1 信道。4 条路由（`:185-188`）：

| 路由 | 作用 |
|---|---|
| `GET /` | `setup.html` 单页，中英双语 |
| `GET /api/session` | 一次性 nonce，保存请求必须带 `X-Setup-Nonce` |
| `GET /api/networks` | Wi-Fi 扫描列表（SSID + RSSI） |
| `POST /api/config` | 凭据 + 目标网络，**先真实试连再落盘** |

`POST /api/config` 的顺序是刻意的：`octop_wifi_portal_join()` 最多阻塞 15 s 等 IP，连不上就返回 `{"error":"wifi"}` 且不写任何凭据；连上了才 `companion_credentials_save()` + `octop_wifi_save()` + 重启。`radio_init` 用 `WIFI_STORAGE_RAM`，门户的试连不会污染运行时配置。

**运行时重连**：`WIFI_EVENT_STA_DISCONNECTED` 只在非门户模式下用 `esp_timer` 一次性退避（1 s 起 ×2，60 s 封顶，`octop_wifi.c:122-135`）。**绝不能在事件回调里 `vTaskDelay`**——那会卡死整个默认事件循环，配置页跟着一起死。

**身份保留**：分区表、`sysmeta`/`nvs` 布局与设备 MAC 都没动，`app-update` 之后配网与凭据仍在，只有整片擦除才会丢。

## 9. UI 与角色动画

`ui/*.json` 与生成的场景 C 源码是 `tools/prepare_ui.py` 的产物，**不要手改**；要改样式就改生成器再重跑。硬件与模拟器共用 `ui/companion_rgb565.yaml` 的 8 播放器配置；动画 JPEG 帧压缩，40 帧预览在设备资源里降为 20 帧 / 4 s 以保住资源分区。

设置页当前项：**拾音**（原"音量"位，0–100 % 线性映射 `esp_codec_dev_set_in_gain()` 的 0–30 dB，默认 55 %）、亮度、语言、自动熄屏、自动翻页、手机配置。喇叭静音与语速已随朗读链路删除。

## 10. 构建、烧录、主机测试

见 [docs/BUILDING.md](BUILDING.md)。要点：`projects/muse_companion/build_octop.ps1` 是唯一受支持入口；资源或布局变更后用 `iris system-update`，只有代码变更且分区表完全一致才可用 `app-update`；两个主机测试（`setup_tests`、`companion_model_test`）都必须打印 `PASS`。

当前构建事实（2026-10-14，`IDF LOW-NOISE BUILD: OK`，238.8 s）：

| 项 | 值 |
|---|---|
| `muse_companion.bin` | 9,982,720 B（`0x985300`） |
| `ota_0` 容量 | `0xaf0000` = 11,468,800 B → 余量 1,486,080 B |
| `.rodata` | 7,864,964 B（内嵌 NotoSansSC 字体 + GSP 资源包占大头） |
| 构建告警 | 1 条：`factory` 分区（`0x1c0000`）容不下应用——**设计如此**，应用装进 `ota_0`，不要为了消除告警去改 Recovery 分区 |

`system-update-bundle` 目标在本构建图里存在（`build/build.ninja:271`），`ui_apps.bin` 由 `bundle.gspb` 打包生成，所以 `mosaico.py iris system-update` 可以直接出包。

## 11. 角色生成工作流

`main/companion_generation.c` 是一个 `esp_iris` RPC 服务（`GENERATION_SERVICE = 0x7e10`）：PC 端 `tools/generate_character.py` 分块上传提示词（`PROMPT_CAP` 32 KiB），设备交给 Octop 文本通道，流式收回源码（`REPLY_CAP` 64 KiB，超限判 overflow），PC 再分块下载。设备**从不执行**收到的 C 代码；检查、编译、预览都在电脑上完成（[docs/CHARACTERS_CN.md](CHARACTERS_CN.md)）。

## 12. 参数速查

| 参数 | 值 | 出处 |
|---|---|---|
| 采样率 / 帧长 | 16000 Hz / 256 帧 | `companion_runtime.c:24` |
| 录音上限 / 下限 | 30 s（960 KB PSRAM）/ 0.2 s | `octop_link.c:25-28` |
| 屏上回复上限 | 65536 B | `companion_model.h:24` |
| 链路回复上限 | 65536 B | `octop_link.c:29` |
| 首帧 / 空闲看门狗 | 45 s / 60 s | `octop_link.c:31-32` |
| 外层 UI 兜底 | 120 s | `companion_runtime.c:28` |
| 阅读页排版 | 440 px × 10 行 / 页，单页缓冲 1024 B | `companion_pager.h:19` |
| 熄屏渐暗 | 提前 2 s，最低亮度 8 | `companion_model.c:6` |
| Wi-Fi 重连退避 | 1 s ×2，≤60 s | `octop_wifi.c:129` |
| 门户试连超时 | 15 s | `octop_wifi.h` |
| JWT 有效期 | 24 h（20 h 主动重登） | 实测，`octop_http.c:283` |
| 拾音增益 | 0–100 % → 0–30 dB，默认 55 % | `companion_runtime.c` 的 `mic_gain_max_db` |

## 13. 故障排查

| 现象 | 先查 |
|---|---|
| "网络未连接，请稍后重试" | Wi-Fi 是否上线；曾连上过但路由器重启 → 看 `station down, retry in …ms` 退避日志 |
| "Octop 还没就绪，请检查配置或网络" | `octop_link_ready()`：地址/账号是否有效、`GET /api/agents` 里有没有 `running` 的 agent、登录是否 401 |
| 说话后一直卡在思考 | STT 是否返回 `{"text":…}`；自定义 `stt_url` 必须兼容 multipart 上传 |
| "等了 45 秒没有回答" | 服务端吞掉了 `user_turn`（半开 socket）；设备已发取消帧，重试即可 |
| WS 握手 403 | 只接受 `?token=`；检查反向代理是否吃掉了 query |
| 回复缺尾 | 屏上与链路同限 64 KiB（约 95 页），只有整轮回答超过 64 KiB 才截断；正常长度不会缺尾，若缺尾先查 `octop_link` 是否提前收到 `done` 或触发看门狗 |
| 门户总提示连不上 | 目标网络确实不可达（5 GHz 取决于 `SOC_WIFI_SUPPORT_5G` 下的 AUTO band 模式）或密码错；**不会写坏旧凭据** |
| 中文长回复后崩溃 | `task_stack_size_freetype` 是否还是 64 KiB |
| 构建报 `/D:/` 路径错 | `.ninja_deps` 没删（`build_octop.ps1` 已负责，别绕过它手搓 cmake） |
| 保存后设备不回原网络 | 门户保存的是"试连成功"的网络；切回常用 Wi-Fi 后重启一次即恢复 |

## 14. 已知限制与验收状态（诚实版）

- **核心链路已在真机验收**。截至本文更新时：两个主机测试全绿、GSP 场景重生成通过、模拟器可交互、固件构建通过；录音 → STT → WS 流式回答上屏的端到端已在真实 Octop 上跑通并由使用者确认。不同自建服务与网络环境仍需在各自设备上复现一遍。
- 没有视频播放，没有音频输出，环境光自动亮度未接入。
- 屏上回复上限与链路对齐为 64 KiB（约 95 页），超长回复靠翻页读完，不做滚动。
- **字号没有设置项**：GSP 1.4.0 没有运行期改字号的属性（`GSP_PROPERTY_KEY_*` 里没有 font size），所以 20/18/16 三档只能编译期做（三套 label + 三套页几何），本次按"不做"处理，阅读密度固定 20 px / 每页 220 汉字。
- 模拟器跑阅读页要带 `--dynamic-font ui/fonts/NotoSansSC-Regular.ttf`（或环境变量 `GSP_SIM_DYNAMIC_FONT`）：GSPC 打进 bundle 的是按 `main/*.c` 静态文案裁剪的子集字体，模拟器用它渲染动态回复会出现豆腐块；真机走 `app_config.ttf` 的整份 FreeType 字体，不受影响（`main/main.c:43-44,60`）。
- 本仓库不分发预编译固件：改造前的 Muse 形态整包（旧 `firmware/*.irisfw` 与 `cloud-validation.json`，千问 TTS 首音频证据）已从公开仓库移除，任何历史固件包都不能刷本项目设备，只能从源码构建后 `system-update`。
- ASR 由 Octop 服务端配置（该部署用 MiMo ASR），设备端不需要也不保存任何模型 Key；`stt_url` / `stt_key` 只是可选覆盖。
- 仓库与固件不含作者的服务地址、账号或 Wi-Fi 数据。

## 15. 安全（按要求不作为设计目标，只列实际暴露面）

评审时值得知道：配置热点是开放网络 + 一次性 nonce，门户开着时同网段物理邻近者可提交配置；Octop 走明文 `http://` 时 JWT 与对话内容在局域网内可被读取；凭据 blob 在 NVS 中未加密（未启用 Flash 加密）。这些都已被接受，因为本项目范围是"能用、不发声、自建服务"，且没有引入新的暴露面——旧版本同样是开放热点配网。刻意保留的两条底线是 nonce，以及"先真实试连成功再落盘"。
