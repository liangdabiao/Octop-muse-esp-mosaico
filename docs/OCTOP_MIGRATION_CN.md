# Mosaico 后端替换方案 v2：Muse → Octop（纯显示，不做朗读）

> 目标：ESP-Mosaico（esp32s31 / 480×480 方屏触摸 / PSRAM）脱离 Muse，直连自托管 **Octop**；**回复只上屏显示，不朗读**。
> 依据：`octop-passport`（ESP32-C3，Octop 客户端已真机验证）+ 本机 `esp-mosaico-vibe-main/.agents/skills`（实测施工约束）+ Mosaico 现有源码。
> v1 → v2 的变化：**朗读整条链路从范围里移除**，因此凭据、UI、状态机、设置项都要改版；工作量重心从"协议对接"转移到"大段文字的屏显与阅读交互"。
> 本报告只做设计，未改任何代码。

---

## 一、结论

**可行，约 9 人日；且改造后系统比现在简单得多。**

| 维度 | 结论 |
|---|---|
| 应用层与 Muse 的耦合 | 只有 **12 个 C 符号 + 4 个就绪门**（§三）。这是本次改造的结构性红利。 |
| Octop 能力覆盖 | **覆盖**。STT（`POST /api/voice/stt`）+ Agent 流式对话（WS `token`/`done`/`error`）。 |
| 朗读 | **Octop 无 TTS 契约，本项目也不需要** → 直接删除 `companion_tts.c` / `companion_qwen.c` / `companion_pcm.c`（约 480 行）与全部 TTS 凭据字段。 |
| 大段文字显示 | **这是新的核心工作量，且现有 UI 完全不够用**：当前每页只有 **36 字**（18 字 × 2 行）。详见 §五。 |
| 安全 | 按你的要求**不作为设计目标**。只保留"免费的"防线（门户一次性 nonce 已有），其余不加。（§七） |
| 施工环境 | **本机已就绪**：`D:\esp-mo\esp-idf`（同钉死提交 `7b9cc1ac`）+ `D:\esp-mo\mingw\w64devkit` 宿主 gcc。但子模块为空，开工第一步必须 `python tools/bootstrap.py`。 |

---

## 二、改造后的系统形态

```
红色AI键按住 ──▶ 16k/16bit/mono PCM ──chunked multipart──▶ Octop POST /api/voice/stt
   松开        （边录边传，不整段缓存）                     └─▶ {"text":"转录"}
                                                                  │
                                          {"type":"user_turn","text","thread_id"}
                                                                  ▼
                       Octop 长连接 WS /api/agents/{agent}/chat/ws
                          token* → done          （error / cancel 可选）
                                                                  ▼
                                            reply 缓冲 → 480×480 阅读页（分页/滚动）
                                                                  ✕ 无喇叭链路
```

与现状的三个根本差别：
1. **一次云端调用变两次**（STT → Agent），中间有可感知的等待，屏上要区分"识别中 / 思考中"。
2. **BLE 配对 + Noise 加密 + IP 隧道整条删除**（约 9k 行出构建），设备只需 Wi-Fi + 一个 Octop 账号。
3. **没有喇叭输出**：录音前"等上一段 TTS 播完 3.5 s"的防回声逻辑（`companion_runtime.c:132-141`）整体作废，交互延迟直接改善。

---

## 三、唯一需要替换的接口面（这是好消息）

`companion_runtime.c` / `companion_model.c` / `companion_ui.c` 对后端的依赖只有下面这些符号，新后端提供同名实现即可：

| 符号 | 现实现 | Octop 侧新实现 |
|---|---|---|
| `muse_hatch_start` | `muse_chat_link.c:760` | 建事件队列 + WS 会话任务 |
| `muse_hatch_ready` | `:812`（linked && wifi && noise） | `wifi_mgr_is_connected() && jwt_ok && ws_up` |
| `muse_hatch_turn_begin` | `:821`（开 `/chat/stream` + 发 JSON head） | 开 `POST /api/voice/stt`，发 boundary + WAV 头 |
| `muse_hatch_turn_audio(pcm,256)` | `:872`（1536 B → base64 2048 字符） | **1536 B 裸 PCM 直接写 chunked**（省 base64，CPU 与带宽双降） |
| `muse_hatch_turn_end` | `:895` | 补 `language` 字段 + 结束边界 → 读 `{"text"}` → 发 `user_turn` |
| `muse_hatch_turn_cancel` | — | abort STT + 发 `{"type":"cancel","thread_id"}` |
| `muse_hatch_turn_event` | `:940` | 由 WS `token/done/error` 驱动，**枚举保持不变** |
| `muse_companion_language / _reply_copy` | `:819 / :958` | 供 STT `language` 取值 / 拷全文给阅读页 |
| `muse_companion_text_begin` | `:845` | 文本直发 `user_turn`（角色生成用，建议一期砍，§六） |
| 4 个就绪门 | `mosaico_hooks.c:28,40,41,43` | 重写 `mosaico_hooks.c`（本就是本仓库自有的适配层，正是为这个用途设计的） |
| `app_run()` / `app_confirm_pairing_press()` | `app.c`（2822 行总控） | `octop_app_run()`：NVS → Wi-Fi STA → SNTP → 会话任务 |

**不移植 Passport 的网络代码。** 它的裸 socket + 手写 WS（`op_net.c`+`op_ws.c`+`op_ws_proto.c` 共 832 行）是因为 C3 没 PSRAM；Mosaico 已链 `esp_http_client`、`esp_websocket_client`、`esp_tls`。**要移植的是协议语义和它踩过的坑，不是代码。**

---

## 四、Octop 端点契约（照抄 Passport 真机实现）

> **【2026-10-14 更新】** 本节已用你的服务器实测完成。实测与 Passport 源码一致的
> 地方我保留；不一致的（`reasoning` 帧、`language` 可省、WS 只认 `?token=`、同一条
> 连接可复用多轮、`state_snapshot` 单轮实测 21 KB、STT 存在 20 s 无响应后 RST）
> 以及由此产生的 6 处计划改动，都在 **`OCTOP_WORK_PLAN_CN.md` §7–§8**，那份为准。


```
POST {base}/api/auth/login        {"username","password"} → {"access_token":"<JWT>"}
      24h 有效，签发时间入 NVS，20h 主动重登                       op_octop.c:24,144-175
GET  {base}/api/agents            Bearer → 扫 "agent_id":"..."；优先复用记住的那个  :201-234
POST {base}/api/agents/{a}/threads {} → 201 {"thread_id":"..."}                    :246-262
WS   ws://{host}/api/agents/{a}/chat/ws?token=<JWT>                                :311
     ↑ 发 {"type":"user_turn","text":"<esc>","thread_id":"..."}                    :329
     ↑ 发 {"type":"cancel","thread_id":"..."}                                      :346
     ↓ 收 {"type":"token","content":"<≤2KB>"} / {"type":"done"} / {"type":"error","message"}  :269-296
     ↓ 其余（state_snapshot / state_update / usage）一律忽略                        :295
POST {base}/api/voice/stt         multipart chunked；字段 audio(16k/16bit/mono WAV)+language → {"text"}
      WAV RIFF size 写占位值，识别端按流长解析                    op_voice.c:119-160,218-235
      备选：stt_url 非空 → 自定义 OpenAI 兼容端点，字段名改 file    op_voice.c:11,85-93
```

**公开文档查不到这些端点。** 我核对了 `docs.octop.cloud`：`/api/connectors` 只讲工具连接器；导航里的 `/api/overview` 与"通道"章节讲的是 IM 通道（微信/QQ/飞书/钉钉/Telegram/小艺/**MQTT**），**没有任何设备侧 chat/voice API**。因此：

- **契约的唯一权威来源是 Passport 源码**（真机验证过，可信度高，但绑定它的 Octop 版本）。
- 落地前**必须用你的服务器实测一轮**（`/api/docs` 是 Octop 的交互式 API 文档，浏览器打开即可核对字段名与 `language` 取值域）。
- 顺带一个发现：Octop 官方有 **MQTT 频道**（"将 IoT 设备消息路由到 Agent"）。**不推荐改用它**——Passport 的 HTTP+WS 有完整真机踩坑记录、流式 token 语义清晰，MQTT 侧我们没有任何验证证据。

---

## 五、新的核心工作量：大段文字的屏显

### 5.1 现状容量核算（这是必须重做的硬证据）

`prepare_ui.py:124-128` 定义回复区：`reply_card` 容器 (18,365) **444×70**，内含两个 label：`reply_text` (24,373,432×26)、`reply_text_2` (24,400,432×26)。默认字号 20（`prepare_ui.py:171`），行高 26 px。

而 `companion_ui.c:10-12` 写死：`LINE_CHARACTERS=18`、`PAGE_CHARACTERS=36`、`PAGE_BYTES=96`。

**结论：当前一页只显示 36 个汉字。** 一段 500 字的 Octop 回复要翻 **14 页**，每页自动 7 秒 → 读完约 98 秒。这在"朗读已经取消、屏显是唯一出口"的新定位下**不可接受**，是本次改造的主要 redesign 项。

### 5.2 屏幕预算（480×480 方屏，非圆屏）

`platform-capabilities.md:5` 明确：**面板是正方形 480×480，BSP 里 "round panel" 注释不准确、勿用**。所以可用面积就是整块 480×480（现在被按圆屏思维浪费了不少）。

以 20 px 字号 / 26 px 行高、左右各留 24 px 为例：

| 方案 | 角色占位 | 文本区 | 行 × 列 | **每页字数** | 500 字需 |
|---|---|---|---|---|---|
| 现状 | 300×300 @(90,75) | 444×70 @(18,365) | 2 × 18 | **36** | 14 页 |
| **A 主页加高** | 300×300 不变 | 扩到 y=250..470（220 px） | 8 × 20 | **160** | 4 页 |
| **B 独立阅读页（推荐）** | 顶部 140×140 小头像 | (18,170)..(462,440)＝444×270 | 10 × 22 | **220** | 3 页 |
| C 字号 16（行高 21） | 同 B | 同 B | 12 × 26 | **312** | 2 页 |

推荐 **B**：`esp_gsp_goto_scene()` 已经在用（`companion_ui.c:125` 切设置页），**GSP 场景数组再加一个"阅读页"** 即可，不改框架、不动渲染路径、复用现有 UTF-8 码点安全分页器（`companion_ui.c:50-76`，只需把 `PAGE_CHARACTERS`/`PAGE_BYTES`/`LINE_CHARACTERS` 从 36/96/18 提到 220/660/22）。设置页再加"字号 20/18/16"三档（对应 C 作为可选加大密度）。

交互（利用触摸屏，cst9220 实驱）：
- **左右滑动翻页** + 页脚 `第 x/y 页 · 自动/手动`（`companion_ui.c:208-212` 已有该文案骨架，直接搬到阅读页）；
- 保留现有自动翻页间隔（3–15 s，`model.c:71-72`）→ 变成"自动滚动阅读"；
- **点屏幕任意处 = 下一页**，长按 = 回首页（沿用"触摸唤醒"已有的输入通路）。

### 5.3 一处必须先验证的技术风险

`prepare_ui.py:171` 的场景公共参数里有 **`swipe=False`** —— 说明 GSP 场景层有 swipe 概念，但本项目从未开启过，且 esp-gsp 版本存在漂移（本工程 `DEPENDENCIES.json` 钉 **1.4.0**，而 vibe 工作区 skill 写 **1.5.1** 且 CDN 对 1.5.1 返回 403、需本地 clone `override_path`，见 known-issues #2）。

**开工后第一件事**：`bootstrap.py` 完成后读 `esp-gsp` 的 scene schema，确认 `swipe` / 可滚动容器语义能否直接给我们用。
- 若支持 → 用原生 swipe；
- 若不支持 → 用**触摸坐标命中自己判方向**（GSP button 已支持 `press`/`release` 事件与坐标，`prepare_ui.py:136-139` 的 `talk` 按钮就是这么做的），实现"左滑下页/右滑上页"。

这条不影响可行性，只影响实现方式，且能**在模拟器里 5 分钟判定**（`mosaico.py project sim --interactive`），不必上真机。

### 5.4 顺带要改的容量常量

- `reply` 缓冲 4096 B（`muse_chat_link.c:53` / `COMP_REPLY_CAP`，`companion_model.h:15`）→ 建议提到 **16 KB**。**实际落地为 64 KiB**（与链路侧 `REPLY_CAP` 对齐，见 `companion_model.h:24`）：改造后曾一度设 16 KiB，导致长回答在翻页前被静默截断，最终把屏上上限抬到与链路一致的 64 KiB 才消除缺尾。有 PSRAM，无压力；分页器本来就吃任意长度。
- Octop 单帧 `content` ≤ 2 KB、Passport 单轮文本上限 1024 B（`op_octop.c:21-22`）——我们自己拼 token 帧，不受该限。
- **字体子集**：`prepare_ui.py:168` 的字符集是从 `main/*.c` 里抽字面量生成的。新增文案会自动进子集，但如果阅读页出现用户回复里的**生僻字**，会缺字。**这条不是新功能的问题，是现在就有**；Octop 回复文本更长，暴露概率更高。vibe 工作区有现成解法：LVGL 字库 gen_font 走 GB2312 全集（`lvgl-chinese-fonts.md`），Mosaico 走的是 FreeType + TTF 动态渲染（`main.c:45-46,62`，渲染栈已提到 64 KiB）——**动态渲染不受子集限制，这条我们天然免疫**，但字库必须随 `ui_apps` 打包，不能裁剪过狠。

---

## 六、删除清单（改造净收益：删多于写）

| 处理 | 内容 | 体量 |
|---|---|---|
| **删（朗读）** | `companion_tts.c` / `companion_qwen.c` / `companion_pcm.c`；门户 5 个 TTS 字段与三个 provider 预设（`setup.html:5-17`）；`companion_credentials` 的 tts_* 字段；`COMP_SPEAKING` 态、喇叭/静音按钮、"重试朗读"按钮、语速设置；`runtime.c:132-141` 的 TTS drain 等待 | ≈ 600 行 |
| **删（Muse 连接栈）** | `app.c` `ble_server.c` `link_pairing.c` `pairing_*.c` `noise_control.cpp` `noise_tunnel.cpp` `tunnel_netif.c` `net_discovery.c` `vm_api.c` `bug_report.c` `factory_test.c` `image_fetch.c` `epaper_status.c` `sensecap_sensors.c` + 整个 `components/noise_core/` | ≈ **15 k 行**，从 `CMakeLists` 的 `sdk_sources` 摘除，**文件留在盘上以便回退** |
| **保留复用** | `wifi_mgr.c`（重连退避 1→60 s、快扫）`wifi_known.c`（NVS 存 ssid/pass/hidden/channel，≤1536 B）`identity.c`（MAC 派生设备名）`muse_chat_text.c` 的 caption 分页部分（纯函数）全部 GSP 资产管线与角色动画 | — |
| **新增** | `octop_auth.c` `octop_stt.c` `octop_ws.c` `octop_link.c`（12 符号）`octop_wire.c`（纯 C11，可宿主单测）＋门户 Wi-Fi 扫描/连接页＋阅读页 scene | ≈ 1400 行 |
| **建议一期砍** | 角色生成（`companion_generation.c` + ESP-Iris RPC 0x7E10 + `generate_character.py`/`preview_character.py`）。它依赖"让云端 Agent 写一整份 C 渲染代码"，在自托管 Agent 上成功率完全未知，且预览校验闸门与 Muse 提示词耦合。**先用官方玩偶形象交付，二期单独立项。** |

改造后凭据字段收敛为 4 个：**Octop 地址、用户名、密码**（+ 可选自定义 `stt_url`/`stt_key`）。不再需要百炼 Key、不再需要 Muse token、不再需要手机 App 配对。

⚠️ **结构体必须 bump 版本**：`companion_credentials.c:80` 的逻辑是"blob 尺寸或校验不符 → 整体清零"。不 bump 的话，升级后用户表现为"凭据全丢"。同时把校验器 `:54-63` 里"只允许 https/wss"改为"允许 `http://`"（Octop 默认就是 `http://IP:8088`）——这是**放宽**，按你的要求不做安全权衡，但要知情。

---

## 七、安全（按要求不作为设计目标，只列"免费的"和"必须知道的"）

**免费的（已在代码里，保留即可）**：门户写操作要一次性 `X-Setup-Nonce`（`companion_setup.c:104-106`）；URL 禁 `@`/`#` 防凭据走私（`companion_credentials.c:56-57`）。

**必须知道的三条事实（不是建议，是现状描述）**：
1. Octop 走**明文 HTTP**，密码在 JSON body、**JWT 在 URL query**（`?token=`）→ 会落进 Nginx/网关 access log。Passport 自己承认账号密码明文存 NVS。
2. 改造后设备**不再有任何端到端加密**（现状有 Noise_XX + eFuse 签名配对）。
3. 配置热点仍无密码。

**结论**：这套东西适合放在你自己的可信局域网/内网里用。**不要**把它交给别人、不要在公网明文裸跑、不要把生产账号配进去。这是使用边界，不是要我加功能。

---

## 八、施工路线（本机实测约束，来自 `.agents/skills`）

**环境已就绪，但有两处必须先满足**：

```powershell
# 0) 子模块为空，现在什么都跑不了
cd D:\esp-mo\Muse-charm-mosaico-main
python tools/bootstrap.py                      # 注入 gitlink + submodule update --init --recursive
python -m pip install -r requirements.txt      # 仅 Pillow
```

1. **IDF 配对铁律**（known-issues #8，第二高频，违反就 `CONFIGURATION ERROR` 且不带任何细节）：
   `D:\esp-mo\esp-idf` ↔ 默认 `%USERPROFILE%\.espressif`（python env `idf6.2_py3.13`），或
   `D:\esp-tools\esp-idf` ↔ `D:\esp-tools\espressif`（`idf6.2_py3.12`）。**禁止交叉组合**；设 `IDF_TARGET=esp32s31` **不能**绕过。选定一套用到最后，来回切会触发 #9（`build/` 2000+ 项批量删除会被安全守卫拦截，必须人工确认）。
2. **每次构建前删 `build\.ninja_deps`**（#1，最高频：GSP 生成文件写了 `/D:/` 路径，ninja 增量必挂）；单文件删除不受安全阈值限制，可固化进脚本。
3. **必须在 PowerShell 里构建，不能从 Git Bash**（#8 第三根因：`idf_tools.py` 检测到 `MSYSTEM` 直接 fatal；`env -u MSYSTEM` 无效，要 `Remove-Item Env:MSYSTEM`）。
4. **关 ccache 要"移除"环境变量不是设 0**（#18：`IDF_CCACHE_ENABLE=0` 反而启用）+ `IDF_PY_BUILD_JOBS=4`；磁盘/页面文件紧张时加 `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`（#14）。
5. **宿主测试用 `D:\esp-mo\mingw\w64devkit\bin\gcc.exe`**（#3，本机无系统 gcc）。`octop_wire.c` 这类纯 C11 逻辑按 skills 建议放独立目录、设备端与宿主共用同一份源码（参考 `weiqi-solution/app/host_tests/` 范式）。
6. **IDF 6.2 没有内建 cJSON**（#19）——现有 `companion_qwen.c` 能用是因为 managed_components 带了；**新增代码别依赖 `json` 组件**，继续显式用 cJSON 头即可，别加 `REQUIRES json`。
7. 中文文案 `snprintf` 缓冲给到 **64**（#12，`-Werror=format-truncation` 对 UTF-8 字面量的估算很保守）。
8. 分区表**不要动**：Recovery 在 `factory`（0x20000, 1.75 MB）比应用小是设计如此，应用装 `ota_0`（`docs/BUILDING.md:18`）。删代码让镜像变小，不需要重排分区。
9. UI 类改动按 `mosaico-ui` skill 的**三段回路**走：先出 480×480 首屏 mockup 给你确认设计，再用 `mosaico.py project sim` 跑通交互与布局，最后才上真机。**静态截图不等于可运行行为**，真机图片用 `python mosaico.py iris screenshot <path> --json` 取。

---

## 九、分阶段实施与验收

| 阶段 | 内容 | 人日 | 验收证据（可机器判定） |
|---|---|---|---|
| **P0 契约实测** | 用你的 Octop 跑一遍 4 个端点（curl/浏览器 + `/api/docs`），定稿 `language` 取值域、WS 能否用 header 带 token、`/api/voice/stt` 是否在你版本存在 | 0.5 | 一张"端点 × 实测响应"对照表；不一致就回改 §四 |
| **P1 凭据与配网** | 门户改版：加 Wi-Fi 扫描/密码 + Octop 地址/账号/密码，删全部 TTS 字段；`companion_credentials` → v2 + 一次性迁移；`octop_auth` login/重登/agent/thread | 1.5 | 宿主单测：URL 规范化（无 scheme 补 `http://`、去尾斜杠，照抄 `op_octop.c:128-141`）、JSON 转义、JWT 过期判定；真机门户保存后拿到 200 + `access_token` |
| **P2 语音 → 文字** | `octop_stt`（chunked multipart）+ `octop_link` 12 符号 + `octop_ws`（长连接/帧分类/**丢弃超长帧**） | 2.5 | 真机按住说一句 → 屏上出现转录并流式出字；30 s 上限生效；麦克风连续失败快速中止（`op_voice.c:188-195` 的 200 次阈值）；取消键生效 |
| **P3 阅读 UI 重做** | 新增"阅读页"scene（每页 ≥ 220 字）+ 滑动/点击翻页 + 页码 + 字号档 + 模型新增 `COMP_STT` 态 + 设置项改版 | 2.5 | 模拟器：500 字回复 ≤ 3 页、翻页与自动滚动全程可操作、中英双语无缺字、UTF-8 不切半字符；`prepare_ui.py` 重跑 + `tools/tests` 全绿 |
| **P4 拆栈** | CMakeLists 摘 `sdk_sources`、重写 `mosaico_hooks.c`、`octop_app_run()` 取代 `app_run()`、TTS 三文件下线 | 1 | 体积/最大堆占用前后对比表；BLE **不再**广播 `MuseGadget`；串口无 `noise_ctrl` 日志 |
| **P5 交付** | 模拟器 mock（`pc/platform_pc.c` 加 STT/Agent 假回复）、README/SETUP/说明书同步、真机 30 轮回归 | 1.5 | 30 轮对话 ≥ 28 成功；无"按了没反应"；文档字段与门户一致 |

**合计 9.5 人日**（P0–P3 = 7 人日是可用最小集；P4 可后置——不拆栈也能跑，代价是 flash 里躺着 15k 行无用代码 + BLE 仍在广播）。

### 必须照抄的 4 条真机教训（能直接毁掉体验）

1. **超长 `state_snapshot` 帧要"丢弃整帧、保持连接"，不能判死。** Passport 原话："state_snapshot 会随会话历史无界增长，固定缓冲必然装不下，而**直接判死会每轮必断**"（`op_ws.h:26-30`）。若用 `esp_websocket_client`，**必须先 peek `type`，非 `token/done/error` 立即停止装配并丢弃**；绝不能照 `companion_qwen.c:52` 那样"超限就置 error"。违反的表现是"能连上、但每轮都超时"。
2. **按键/转录请求要"永远接收、排队补发"**，只在 READY 才收会静默吞掉输入（`op_octop.c:98-106` 注释写明这是真机"按了没反应"的根因）。
3. **首帧 45 s 未到 → 换新 thread 重发原句**（`op_octop.c:355-366`）；且**发送后立刻断链、一帧未收 → 自动重发**（`:378-381`，半开连接会让 `user_turn` 发进黑洞）。Mosaico 现在是"超时直接报错给用户"（`runtime.c:207-211`），语义要在 `octop_link` 内部消化，不要抛给 UI。
4. **`thread_id` 必须显式带**，服务端会忽略 `session_key`（`op_store.h:8-9` 引 P-M0 实测）。

---

## 十、我还需要你提供的（一次列全）

1. **Octop 服务器地址 + 用户名/密码**（联调用的专用账号即可）。有了它我就能直接打 `/api/docs` 把 §四 的三处不确定（`language` 取值域、WS 鉴权能否用 header、你部署版本是否有 `/api/voice/stt`）全部实测定稿——这是 P0，0.5 人日。
2. **Octop 部署版本号 / commit**（端点漂移的唯一判据）。
3. 后台是否已启用 STT、用的哪家（MiMo / 腾讯云 ASR / Whisper）。
4. 是否接受**一期砍掉"通过后端生成角色"**（我的建议：砍）。
5. 阅读页字号默认档偏好：**20 px（每页约 220 字）**还是 **16 px（每页约 312 字）**？我默认按 20 做，密度不够再往下调。

**下一步我建议先做两件事（都不动代码）**：P0 端点实测（需要你给第 1 项），以及出一张 480×480 阅读页 mockup 给你确认设计——按 `mosaico-ui` skill 的规矩，整体布局没确认前不写实现代码。
