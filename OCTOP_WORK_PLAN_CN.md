# Octop 改造工作计划（施工用）

> 这份文档只讲一件事：**我接下来按什么顺序做、每步动哪些文件、怎么算做完。**
> 设计理由和可行性论证不在这里，在 `docs/OCTOP_MIGRATION_CN.md`。
> 状态由我更新，你随时看这张表就知道走到哪了。

---

## 0. 使用约定

- 任务编号 `P<阶段>.<序号>`。每张任务卡固定四段：**做什么 / 动哪些文件 / 完成的判据 / 卡住怎么办**。
- 状态标记：`[ ]` 未开始 · `[~]` 进行中 · `[x]` 已验收 · `[!]` 阻塞（后面必须写清卡在什么上）。
- **"已验收"只认机器判据**（构建通过、单测通过、模拟器截图/日志、真机串口日志）。我不写"应该没问题"这种话。
- 每个阶段结束我交付一次：改了什么 + 实测证据 + 还差什么。不攒到最后一次性交。

---

## 1. 目标与总验收

**目标**：这台 Mosaico（esp32s31 / 480×480 方屏触摸）不再连 Muse，直连你自托管的 **Octop**；**按住 AI 键说话 → 屏上出转录 → 屏上流式出回复全文**。**全程不朗读、不接喇叭。**

**总验收（做完的标准）**：

1. 刷机后开机 → 手机连设备热点 → 门户里选自家 Wi-Fi、填 Octop 地址和账号 → 保存重启。
2. 重启后首页显示"已连接"，按 AI 键说一句中文，松手 → 3 秒内屏上出现我的转录 → 回复逐字流式上屏。
3. 500 字左右的回复，**翻页不超过 3 页**，可左右滑/点击翻页，也可设自动滚动。
4. 连续 30 轮对话，成功 ≥ 28 轮；无"按了没反应"；无每轮必断。
5. `python mosaico.py project sim` 里能离线跑完整交互（假 STT/假 Agent），不用真机也能回归。

**总工作量：10 人日**（P0–P3 = 7.5 天是可用的最小集；P4/P5 可后置）。

---

## 2. 需要你给我什么（阻塞项先说清）

| # | 项 | 阻塞哪个阶段 | 没有的话我怎么办 |
|---|---|---|---|
| 1 | ~~Octop 地址 + 用户名/密码~~ | ~~P0/P2/P3 真机~~ | **已给，P0 已实测完成（证据见 §7）。** |
| 2 | Octop 部署版本 / commit | 不阻塞 | 实测已锁定该部署的行为特征（§7），够我写设备端。仍建议给我版本号，便于以后核对升级漂移 |
| 3 | ~~后台是否已开 STT、用的哪家~~ | ~~P2~~ | **已确认：MiMo ASR 已配置且可用**，转录准确，2.6–4.2 s（5.9 s 语音）。语音通路不用砍 |
| 4 | 一期是否砍"云端生成角色形象" | P3 | **我默认砍**，先用官方玩偶形象交付。你没反对我就按砍做 |
| 5 | 阅读页默认字号 | P3 | **我默认 20 px**（每页约 220 字）。不够密我再降到 16 |

现在**没有硬阻塞**了。剩下要你拍的只有第 4、5 两项，且都有默认值。

---

## 3. 计划总览

| 阶段 | 内容 | 人日 | 依赖 | 交付物 |
|---|---|---|---|---|
| **P0** | 契约实测：在你服务器上把 4 个端点打一遍 | 0.5 | 需要 §2 第 1 项 | 端点×实测响应对照表，定稿 §四 契约 |
| **P1** | 凭据 v2 + 门户配网（新增 Wi-Fi 扫描/连接）+ Octop 登录会话 | 2.0 | P0 | 真机保存配置后拿到 200 + `access_token`；宿主单测全绿 |
| **P2** | 语音→文字→Agent：STT 上行、WS 会话、12 个符号的新实现 | 2.5 | P1 | 真机按住说话 → 屏上流式出字 |
| **P3** | 阅读页重做（每页 ≥220 字、翻页/自动滚动、字号档、状态机加"识别中"） | 2.5 | P0 + 设计确认 | 模拟器：500 字 ≤3 页、翻页全程可操作、无缺字 |
| **P4** | 拆栈：摘 Muse 连接栈与 TTS 三文件、重写 hooks | 1.0 | P2 | 前后体积对比表；BLE 不再广播；无 noise 日志 |
| **P5** | 模拟器 mock + 文档同步 + 30 轮回归 | 1.5 | P2/P3 | 30 轮 ≥28 成功的日志证据 |
| **P6** | （可选，二期）云端生成角色形象 | 2.0+ | 单独立项 | 本期不做 |

顺序上 P3 的**设计确认（mockup）可以和 P0 并行**——它不需要你的服务器。

> ⚠️ P1 比方案里写的 1.5 天上调到 2.0 天，原因见 §6 第 2 条：门户现在**完全没有** STA 配网能力。

---

## 4. 任务卡

### P0 契约实测（0.5 天）`[x]` 已完成 · 证据在 §7

**P0.1 端点可达性**
- 做什么：用 curl 依次打 `POST /api/auth/login` → `GET /api/agents` → `POST /api/agents/{a}/threads` → `POST /api/voice/stt`（拿一段 16k/16bit/mono WAV 试）。
- 完成的判据：4 个端点的真实请求/响应各存一份；状态码、字段名、`thread_id` 是否必填都有记录。
- 卡住怎么办：若 `/api/voice/stt` 在你的版本里不存在 → 改走自定义 OpenAI 兼容端点（`stt_url` 字段已为此预留）；若都没有 → 通知你，P2 改成"门户打字输入"先行。

**P0.2 三个未知项定稿**
- 做什么：浏览器开 `{base}/api/docs`，核对 ① STT 的 `language` 合法取值域（Passport 硬编码 `zh-CN`）② chat WS 能否用 `Authorization` 头代替 `?token=`（JWT 进 URL 会落 Nginx access log）③ WS 下行帧的实际 `type` 集合。
- 完成的判据：三条各有一句结论 + 文档/实测出处；`docs/OCTOP_MIGRATION_CN.md` §四 同步更新。
- 卡住怎么办：`/api/docs` 不存在或需登录才给 → 我用一次真实 WS 会话抓帧定型（需要 P0.1 的账号可用）。

**P0.3 WS 抓一次真实流**
- 做什么：用现成的 WS 客户端（不给设备写代码）连 `ws://.../api/agents/{a}/chat/ws?token=`，发一帧 `user_turn`，记录 token/done/error 的实际时序与超长 `state_snapshot` 帧的真实大小。
- 完成的判据：一份帧序列日志 + `state_snapshot` 实测最大字节数（决定我们的接收缓冲）。
- 卡住怎么办：连不上就换 header 鉴权再试；仍不行 → 记录现象找你确认服务端日志，不在设备上盲调。

---

### P1 凭据与配网（2.0 天）

**P1.1 凭据结构 v2**
- 做什么：`components/companion_credentials/` 改成 4 字段（`octop_url` / `username` / `password` / 可选 `stt_url`+`stt_key`），删光 `tts_*`；**`CREDENTIALS_VERSION` 从 1 bump 到 2**。
- 动哪些文件：`companion_credentials.c`、`include/companion_credentials.h`、删 `include/companion_tts_preset.h`、`companion_setup.c`（读写字段）、`tools/tests/setup_tests.c`。
- 完成的判据：`tools/tests` 全绿；且**旧 v1 blob 升级后不闪退回默认**（我会在单测里造一条 v1 blob 断言"要么迁移要么干净地要求重新配置，绝不静默半配置"）。
- 三个必须一起改的坑（都是我这轮核对出来的，不改就是隐性 bug）：
  1. `companion_credentials.c:80` 尺寸/校验不符就**整体清零** → 不 bump 版本等于升级即丢配置。
  2. `:43` `token_valid` 不接受空串 → 可选字段 `stt_url` 留空会让整条校验失败。我用独立的"空即合法"检查，不复用 `token_valid`。
  3. `:54-63` 只放行 `https://`/`wss://` → Octop 默认 `http://IP:8088` 会被拒。同时 **`tools/tests/setup_tests.c:41` 现在断言的正是"http:// 非法"**，这条断言必须反过来改，否则测试挂。

**P1.2 门户加配网页（净新增，这是 P1 上调 0.5 天的原因）**
- 做什么：门户从"只填 token"变成"选 Wi-Fi + 填密码 + 填 Octop 地址账号"。设备侧新增两个 HTTP 路由：`GET /api/networks`（扫描）和 `POST /api/network`（连接并记住）。
- 动哪些文件：`main/companion_setup.c`（AP→**AP+STA 共存**、新路由）、`main/setup.html`（表单与双语词条）、新增 `main/setup_wifi.c`（调 `wifi_mgr_scan/connect` + `wifi_known_remember`）。
- **好消息**：底层不用重写。`wifi_mgr.h` 已有同步扫描（去重、按 RSSI 排序、支持定向扫隐藏 SSID）与阻塞式连接；`wifi_known.h` 已有 NVS 记忆列表（最多 8 个）。这些文件保留在构建里，不随 Muse 栈删除。
- 完成的判据：真机热点 `Mochi-Setup-XXXX` → 手机门户里能看到你家 SSID → 填错密码返回明确失败并可重试 → 填对后 `esp_wifi_connected` 且 IP 到手 → 重启后不需要重配。
- 卡住怎么办：AP+STA 共存不稳 → 退化成"配网期间纯 STA，配完重启进 AP 只做一次"，即先连手机热点再切回；这条我会在真机日志里判定，不猜。

**P1.3 `companion_setup_start()` 的"ready"语义**
- 做什么：现在 `companion_setup.c:150` 用"凭据齐了"决定是否跳过配网页，但凭据齐 ≠ Wi-Fi 通。**判据改成"凭据齐 且 有已记忆的 Wi-Fi"**，否则每次开机都会掉进配网页（`main.c:69` 会因此永远不起 runtime）。
- 完成的判据：三种状态（全新 / 只有凭据 / 齐全）分别从串口日志能区分，且只有第三种进首页。

**P1.4 `octop_auth.c`（新增）**
- 做什么：login → 存 JWT + 签发时刻；20 h 主动重登（Passport 用同一阈值）；无 scheme 的 URL 规范化（补 `http://`、去尾斜杠）；agent 选定后记住，thread 惰性创建。
- 动哪些文件：新增 `components/octop_link/octop_auth.c` + `octop_wire.c`（纯 C11：URL 规范化、JSON 转义、过期判定）。
- 完成的判据：`octop_wire` 的宿主单测覆盖 —— 12 种 URL 写法归一到同一结果；含引号/反斜杠/中文的文本转义后能被解析回来；JWT 过期边界（19 h59 m / 20 h01 m）判定正确。**这一层不需要真服务器就能测。**

---

### P2 语音 → 文字 → Agent（2.5 天）

**P2.1 `octop_stt.c`**
- 做什么：按住期间把 16k/16bit/mono PCM **边录边以 chunked multipart 上推**，不整段缓存；WAV 的 RIFF size 先写占位值，靠流长度定界；松手补 `language` 和结束边界，读回 `{"text"}`。
- 约束照抄 Passport（`op_voice.c`）：10 ms/640 B 一帧、20 帧一块发、单轮上限 30 s、麦克风连续失败要快速中止（它的阈值是 200 次），别对着坏麦克风录满 30 s。
- 完成的判据：真机录一段中文 → 屏上出现正确转录；拔掉/堵住麦克风 → 2 秒内报错而不是转圈。

**P2.2 `octop_ws.c`（长连接会话任务）**
- 做什么：常驻 WS 会话任务，收帧分类：`token` 追加进回复缓冲、`done` 收尾、`error` 报错，**其余 type（`state_snapshot`/`state_update`/`usage`）一律丢弃且保持连接**。
- **必须遵守的第 1 条真机教训**：`state_snapshot` 随会话历史无界增长，固定缓冲一定装不下，**超限判死 = 每轮必断**（Passport 原话，`op_ws.h:26-30`）。所以：先解析出 `type` 再决定是否装配 payload；超长的只丢这一帧。绝不能学 `companion_qwen.c:52` 那种"缓冲超限就置 error"。
- 完成的判据：连续 20 轮无一次非预期断连；日志里能看到"丢弃了 N 帧非对话帧"且轮次不受影响。

**P2.3 `octop_link.c`：提供那 12 个同名符号**
- 做什么：实现 `muse_hatch_start/ready/turn_begin/turn_audio/turn_end/turn_cancel/turn_event/status` + `muse_companion_language/reply_copy/text_begin`。**`muse_hatch_ev_t` 的 6 个枚举值（NONE/HEARD/REPLY/DONE/ERROR/SENT）语义保持不变**，这样 `companion_runtime.c`/`companion_model.c` 基本不动。
- `turn_audio(pcm,256)` 改成裸 PCM 直写（现在走 base64，省掉 CPU 和 1/3 带宽）。
- 完成的判据：`grep -c muse_hatch_ main/companion_runtime.c` 的调用点一个没少也没加；`companion_ui.c` 一行不改就能把 `REPLY` 增量刷上屏。

**P2.4 三条"永远不要抛给 UI"的内部兜底**
- 做什么，在 `octop_link` 内部实现，不让用户看见：
  1. 按键/请求**永远接收、排队补发**，只在 READY 才收会静默吞掉输入（这是 Passport 真机"按了没反应"的根因）。
  2. 首帧 45 s 未到 → **换新 thread 重发原句**；发送后立刻断链且一帧未收 → 自动重发（半开连接会让 `user_turn` 进黑洞）。
  3. `thread_id` 必须显式带（服务端会忽略 `session_key`）。
- 完成的判据：模拟器里人为注入"首帧超时/半开连接"，用户侧表现为"多等了一下然后出结果"，而不是报错页。

---

### P3 阅读 UI 重做（2.5 天）—— 本期真正的设计活

**先做 P3.0，它不需要你给服务器**

**P3.0 480×480 阅读页 mockup（设计确认）**
- 做什么：出一张 1:1 首屏图，含：小头像位、正文区、页码脚注、翻页手势提示。给你确认后才写实现。
- 为什么必须先做：按 `mosaico-ui` 的规矩，整体布局未确认前不写 UI 代码；静态图不等于可运行行为，但能一次定死版面预算。
- 完成的判据：你说"就这样"或提出具体改法。

**容量核算（这是必须重做的硬证据）**：现在 `prepare_ui.py:124-128` 的回复卡是 **444×70**，两个 26 px 高的 label；`companion_ui.c:10-12` 写死 `LINE_CHARACTERS=18`、`PAGE_CHARACTERS=36`。**一页 36 个汉字**，500 字要翻 14 页，默认 7 秒自动翻页 → 读完约 98 秒。朗读取消后屏显是唯一出口，不可接受。

**P3.1 新增"阅读页"scene（推荐方案：每页 220 字）** `[x] 已完成，见 §9`
- 实测修正：正文框不是 444×270 而是 **444×300**（20 px 字号真实行高 30 px），坐标 (18,138)；分页器不是"复用旧常量"而是重写为 `main/companion_pager.h`，按**像素**预算行宽（440 px/行 × 10 行），因为字体是比例字体，拉丁文比汉字密。
- 做什么：GSP 场景数组再加一个页面；正文区 (18,170)→(462,440)＝444×270，字号 20、行高 26 → **10 行 × 22 字 = 220 字/页**（500 字约 3 页）。顶部留 140×140 小头像，不放大形象。
- 复用而非重写：现有 UTF-8 码点安全分页器 `companion_ui.c:50-76` 原样留用，只把常量 36/96/18 → 220/660/22；页码提示文案骨架 `companion_ui.c:208-212` 搬过去。切页沿用已有的 `esp_gsp_goto_scene()`（`companion_ui.c:125` 已经在用它切设置页）。
- 缓冲：回复上限 4096 B → **16 KB**（有 PSRAM，分页器本来就吃任意长度）。
- 完成的判据：模拟器灌一段 500 字中文回复 → 页数 ≤3、不切半字符、不溢出容器；`prepare_ui.py` 重跑 + `tools/tests` 全绿。

**P3.2 翻页手势（有一个待验证点）** `[x] 已完成，待验证点已出结论`
- 结论：`swipe` 是**场景间**导航开关，翻不了回答页；`esp_gsp_event_t` 也没有坐标。所以横滑走 `esp_gsp_set_pointer_observer()`（唯一坐标源；注册失败不致命，轻触/长按/页脚按钮仍然可用），滑动 ≥64 px 且横向占优才翻页，并用 `swipe_held` 吞掉紧随其后的原生 click。模拟器实帧已证明一次手势只翻一页。
- 做什么：左右滑翻页 + 点屏幕任意处下一页 + 长按回首页；保留自动翻页间隔（3–15 s）当"自动滚动阅读"。
- **开工后第一件事**：`bootstrap.py` 跑完后读 `esp-gsp` 的 scene schema，确认 `prepare_ui.py:171` 里那个从没开过的 **`swipe=False`** 能不能直接用。支持就用原生 swipe；不支持就用 `press/release` 坐标自己判方向（`prepare_ui.py:136-139` 的 talk 按钮就是这个路子）。
- 判据：**在模拟器里 5 分钟出结论**，不用上真机。这条只影响实现方式，不影响可行性。
- 版本漂移要盯：本工程钉 `esp-gsp ==1.4.0`（`main/idf_component.yml`），而 vibe 工作区 skill 写 1.5.1。我按本工程的 1.4.0 读 schema，不看 1.5.1 的文档。

**P3.3 状态机与设置项改版** `[x] 已完成（字号档取消，见下）`
- 已做：插入 `COMP_STT`（松手 → 识别中 → 收到转录才进思考中，两段等待在屏上分开）；`COMP_SPEAKING` 改作 `COMP_RECEIVING` 且姿态序号用 `mode_pose[]` 表显式映射，插态不再错位；音量滑块改作拾音增益。
- **取消**：字号 20/18/16 三档。GSP 1.4.0 没有运行期字号属性（`GSP_PROPERTY_KEY_*` 里没有 font size），实现它要在场景里堆三套 label + 三套页几何，收益（更密）不如代价，故不做。
- 做什么：
  - `companion_model.h` 加 **`COMP_STT`（识别中）** 态——现在一次对话要打两次云端（STT→Agent），中间那段等待必须在屏上区分开，否则像卡死。
  - `COMP_SPEAKING` 改作"回复上屏中"（名字我会改成 `COMP_READING`，同时把 `companion_model_reply(..., bool speaking)` 的 `speaking` 参数去掉）。
  - 删：音量、静音、语速、"重试朗读"按钮及 `COMP_MUTE/COMP_VOLUME/COMP_RETRY/COMP_SPEECH_RATE/COMP_EFFECT_RETRY`。
  - 加：**字号档（20/18/16）**、翻页方式（自动/手动）。
  - 6 个角色姿势与 `mode==(companion_mode_t)i` 的映射会因插入新态而错位，`companion_ui.c` 里那段 pose 循环要一起改。
- 完成的判据：`pc/companion_model_test.c` 新增状态转移用例并全绿（按住→识别中→思考中→上屏→首页，及每一态的取消/错误路径）。

**P3.4 文案与字库**
- 做什么：门户与屏上中文词条全部重写；`prepare_ui.py:168` 的字符集是从 `main/*.c` 抽字面量生成的，新文案会自动进子集。
- 一条已核实的好消息：屏上用 **FreeType + TTF 动态渲染**（`main.c:45-46,62`，渲染栈已提到 64 KiB），**不受字分子集限制**，Octop 回复里的生僻字不会缺字。要守的是字库必须完整随 `ui_apps` 打包，别裁太狠。
- 完成的判据：故意让 mock 回复含"熵、囍、emoji"，屏上无豆腐块。

---

### P4 拆栈（1.0 天，可后置）

**P4.1 从构建里摘 Muse 连接栈**
- 做什么：`components/muse_link_sdk/CMakeLists.txt` 的 `sdk_sources`（现 20 项）只留 `wifi_mgr.c wifi_known.c identity.c muse_chat_text.c`（分页纯函数）+ 新实现，其余摘除；REQUIRES 里 `noise_core`、`product_info`、`bt` 同步摘。
- **注意（我这轮核对的结论）**：磁盘上有一堆文件其实**根本没在构建列表里**（`voice*.c`、`epaper_status.c`、`image_fetch.c`、`sensecap_sensors.c`、`tunnel_netif.c` 等）。所以这一步是"从 `sdk_sources` 摘 15–17 个条目"，不是"删 15k 行文件"。**文件一律留在盘上**，回退只需还原 CMakeLists。
- 完成的判据：`BUILD sha` 前后镜像字节数对比表；`muse_companion/main/CMakeLists.txt` 的 SRCS 里 `companion_tts.c companion_qwen.c companion_pcm.c` 消失；串口日志无 `noise_ctrl`；BLE 不再广播 `MuseGadget`。

**P4.2 重写 `mosaico_hooks.c`**
- 做什么：这个 63 行的文件本来就是本仓库自有的适配层，正是为换后端准备的。把 4 个就绪门（`:28` `muse_port_initialized`、`:40` `muse_wifi_connected`、`:41` `muse_link_hatch_linked`、`:43` `muse_link_req_ready`）从"BLE 已配网 + Noise 已连"改成"有 Wi-Fi + JWT 有效 + WS 在线"；`:44-49` 的 `noise_ctrl_req_*` 转发改成 Octop 请求。
- 完成的判据：`app_run()` 被 `octop_app_run()` 取代（NVS → Wi-Fi STA → SNTP → 会话任务）；`main.c` 启动顺序不变。

**P4.3 分区表**：**不动**。Recovery 在 `factory`（0x20000, 1.75 MB）比应用小是设计如此，应用装 `ota_0`。删代码只会让镜像更小。

---

### P5 交付（1.5 天）

**P5.1 模拟器离线回归**
- 做什么：`pc/platform_pc.c`（已存在，`gsp_add_backend` 已经把 `companion_ui.c + companion_model.c + platform_pc.c` 编成 PC 后端）里加假 STT/假 Agent：延迟回转录、按 token 流式回长回复、可注入错误与首帧超时。它已有 `MUSE_COMPANION_SIM_SETUP=1` 开关模拟配网页，我照这个路子加 `OCTOP_SIM_*` 变量。
- 完成的判据：`python mosaico.py project sim --project projects/muse_companion --interactive` 里，点 AI 区 → 转录 → 长回复流式 → 翻页，全程不需要网络和真机。

**P5.2 真机 30 轮回归**：连续 30 轮，≥28 成功；每轮记录耗时；真机图片用 `python mosaico.py iris screenshot <path> --json` 取并我直接看。

**P5.3 文档同步**：`docs/OCTOP_MIGRATION_CN.md`（契约以 P0 实测为准）、`docs/SETUP_CN.md`（新门户流程）、`docs/PROJECT_MANUAL_CN.md`（删 Muse/TTS 章节）、README 的凭据说明。**不留下任何还写着"Muse token / 百炼 key"的入口。**

---

## 5. 施工铁律（本机环境，违反就白等）

来自 `D:\esp-mo\esp-mosaico-vibe-main\.agents\skills` 的实测条目，我每条都会执行：

1. **子模块是空的，现在什么都跑不了**。开工第一条：`python tools/bootstrap.py`，再 `pip install -r requirements.txt`（只有 Pillow）。
2. **IDF 配对铁律**：`D:\esp-mo\esp-idf` ↔ 默认 `%USERPROFILE%\.espressif`（`idf6.2_py3.13`），或 `D:\esp-tools\esp-idf` ↔ `D:\esp-tools\espressif`（`idf6.2_py3.12`）。**禁止交叉**；设 `IDF_TARGET=esp32s31` 也绕不过去。选一套用到最后——来回切会触发 2000+ 项 `build/` 批量删除被安全守卫拦截，需要人工确认。
3. **每次构建前删 `build\.ninja_deps`**（GSP 生成文件写了 `/D:/` 路径，ninja 增量必挂）。单文件删除不受阈值限制，我会固化进脚本。
4. **只能在 PowerShell 构建，不能从 Git Bash**（`idf_tools.py` 见到 `MSYSTEM` 直接 fatal，`env -u MSYSTEM` 无效）。
5. **ccache 要"移除"环境变量而不是设 0**（`IDF_CCACHE_ENABLE=0` 反而是启用）；`IDF_PY_BUILD_JOBS=4`；磁盘/页面文件紧张时加 `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`。
6. **宿主测试用 `D:\esp-mo\mingw\w64devkit\bin\gcc.exe`**（本机无系统 gcc）。`octop_wire.c` 这类纯逻辑放独立目录，设备端与宿主共用同一份源码。
7. **IDF 6.2 没有内建 cJSON**——cJSON 来自 managed component `espressif/cjson ~1.7`。新代码显式 include，**不要加 `REQUIRES json`**。
8. 中文文案 `snprintf` 缓冲给到 **64**（`-Werror=format-truncation` 对 UTF-8 字面量估算很保守）。

---

## 6. 我这轮核对代码后对方案的两处修正

写进计划是因为它们会改变工作量，不能只当注释：

1. **`sdk_sources` 与磁盘文件不一致**（§P4.1）：P4 是"摘构建条目"而非"删 15k 行文件"，风险更低、回退更快。
2. **门户完全不配 Wi-Fi**（§P1.2）：`companion_setup.c:148-184` 是**纯 AP、无 STA、无扫描**，现在 Wi-Fi 是靠手机 Muse App 经 BLE 写进 `wifi_known` 的 NVS。Octop 没有配套 App，所以配网能力必须我们在门户里自建 —— **P1 从 1.5 天上调到 2.0 天，总量 9.5 → 10 人日**。好消息是底层 `wifi_mgr_scan/connect`、`wifi_known_remember` 已存在且保留在构建里，净新增只是两个 HTTP 路由 + 一个页面。

---

## 7. P0 实测证据（2026-10-14，直接打你的服务器）

目标：`http://<你的 Octop 地址>:8088`（`server: uvicorn`），账号见你自己的部署（**不要把地址和账号写进仓库文件**）。测试素材：Windows TTS 合成的 16k/16bit/mono 中文语音 5.9 s（说的是"屏显正常，这是一段用于语音识别测试的中文语音。"）。

### 7.1 端点 × 实测响应

| 端点 | 结果 | 实测细节 |
|---|---|---|
| `POST /api/auth/login` | **200** | `{access_token, token_type:"Bearer", expires_in:86400, user:{id:2, username, role:"user", locale:"zh", permissions:[channels,connectors,skill_packages,knowledge_bases]}}`。JWT 解码 `exp-iat = 86400` → **24 h 有效，和 Passport 的 20 h 主动重登阈值一致，不用改** |
| `GET /api/agents` | **200** | 1 个 agent：`agent_id="4X4EJ8"`、`name="Passport"`、`state="running"`。**字段名确认为 `agent_id`**（Passport 扫的就是这个） |
| `POST /api/agents/{a}/threads` | **201** | `{thread_id:"thr_...", session_key:"4X4EJ8:dashboard:2:dm"}`。**我传的 `session_key` 和 `title` 都被服务端忽略**，返回它自己拼的那把 |
| `GET /api/agents/{a}/threads` | **200** | 列表带 `is_active / has_messages / pinned / artifacts`。**新建 thread 会把上一个 `is_active` 置 false**（一个 agent 只有一个活跃线程） |
| `WS /api/agents/{a}/chat/ws?token=<JWT>` | **101** | 握手成功。发 `{"type":"user_turn","text","thread_id"}` → 收流。**`Authorization` 头 403、`Cookie` 头 403、坏 token 403**（`Content-Length: 0` + `Connection: close`，没有 error 帧也没有 WS 关闭码） |
| `POST /api/voice/stt` | **200** | `{"text":"屏显正常，这是一段用于语音识别测试的中文语音。","confidence":null}` —— **转录一字不差**。multipart 字段 `audio`（带 filename + `Content-Type: audio/wav`）；`language` **可省** |
| `GET /api/health` | 200 | `{"ok":true,"db":true,"users_loaded":2,"agents_running":3}` |
| `GET /api/providers` | 200 | 只有两家 LLM（DeepSeek、智谱）。**ASR 不在这个列表里**（MiMo 走别的配置面），所以我一开始"没配 ASR"的判断是错的 |
| `/openapi.json` `/api/docs` `/api/openapi.json` | 404 / SPA 兜底 | **这个部署不暴露 OpenAPI**。契约只能靠实测，本次的实测就是权威版本 |

### 7.2 WS 帧契约（真实抓帧，一轮完整对话）

单轮收到 153 帧，实测分布：

| `type` | 帧数 | 最大字节 | 结构 | 设备侧处置 |
|---|---|---|---|---|
| `token` | 57 | **101 B** | `{type,content,node,thread_id}` | 追加进回复缓冲，上屏 |
| `reasoning` | 73 | 112 B | 同 `token` 形 | **丢弃**（Passport 的忽略清单里没有这一条，是它那个版本没出现；我们"未知 type 全丢"天然覆盖） |
| `state_update` | 15 | **14670 B** | `{type,node,data,thread_id}` | 丢弃 |
| `state_snapshot` | 7 | **21094 B** | `{type,data:{messages:[...]}}` | **丢弃但保持连接** |
| `usage` | 1 | 353 B | `{type,usage:{input_tokens:16280,cache_read_tokens:15872,output_tokens:131,...}}` | 丢弃 |
| `done` | 1 | 63 B | `{type,thread_id}` | 收尾 |

时序：连接后 **405 ms 出首帧**（`state_snapshot`），`reasoning` 从 1.4 s 开始，`token` 约 2 s 开始，整轮 **2.3 s** 完成。
另一轮长文请求（"写 800 字"）**首 token 到 13 s** 才出现 —— 说明 Passport 那个"首帧 45 s 未到就换 thread"的窗口不是过度设计，是必要的。

`state_snapshot` 在**同一轮内** 285 → 14833 → 19036 → 19677 → 21062 → 21094 B 单调增长，且随会话历史继续膨胀。**Passport 那句"固定缓冲必然装不下，直接判死会每轮必断"被实测完全证实。**

### 7.3 打断与连接复用（实测，关系到 AI 键语义）

- 发到第 10 个 token 时发 `{"type":"cancel","thread_id"}` → **165 ms 后收到 `done`**，且**没有任何残留 token 漏进下一轮**。打断语义干净可用，设备端不用自己伪造 `done`。
- **同一条 WS 连接可以直接发起第二轮**（第二轮 3.2 s 完成）。所以会话任务可以是"一条长连接、多轮复用"，不必每轮重连——比 Passport 的实现还简单。

### 7.4 STT 时延与异常行为（决定超时参数）

| 输入 | 时延 | 结果 |
|---|---|---|
| 5.9 s 中文语音 ×5 次 | 2.61 / 2.82 / 2.85 / 2.65 / 4.21 s | 全部 200，转录正确 |
| 0.3 s 纯 220 Hz 音（非语音） | 1.27 s | 200，返回**幻觉文本 `"嗯。"`** |
| 冷启动那一批（我的前 6 次尝试） | **19.2–20.5 s 无任何响应，然后连接被 RST** | 无 HTTP 状态码 |

最后一条是真事，不是我写错请求：同一份字节、同样的 `-F audio/@file` + `language=zh-CN`，前一轮 6 次全部 20 s 无响应被掐，后一轮起连续 10+ 次全部 2.6–4.2 s 返回。

### 7.5 一件你该知道的事（不是建议，我不动它）

`GET /api/providers` 会把 **`api_key` 明文**返回给 `role:"user"` 的普通登录 token（我在实测里直接看到了 DeepSeek 与智谱的完整 key）。任何拿到一个普通账号的人都能读走你所有供应商的 key。按你的要求我不改设计、也不为此加功能，只是告知事实。

---

## 8. 实测之后，计划要改的 6 处

这些是 §7 直接落到任务上的改动，不是补充说明：

| # | 实测结论 | 影响哪个任务 | 具体改法 |
|---|---|---|---|
| 1 | WS 只认 `?token=`，header/cookie 都 403 | P2.2 | **删掉"改用 Authorization 头"这个待验证项**。顺带：403 是裸 HTTP 拒绝、没有 WS 关闭码 → 设备必须把"握手 403"单独识别为 **JWT 失效 → 重登**，而不是当网络错误无限重连 |
| 2 | 同一条连接可复用多轮 | P2.2 | 会话任务改成"一条长连接 + 多轮"，不用每轮重建；`done` 之后连接保持，下一轮直接发 `user_turn` |
| 3 | `cancel` 后 165 ms 收到 `done`，无残留 token | P2.4 / P3 | 打断实现按"发 cancel 并等 `done`"写。屏上"按住 AI 键打断"这个交互**有真实后端支撑**，不是装饰 |
| 4 | `state_snapshot` 一轮内涨到 21 KB 且无界增长；另有 `reasoning` 帧 | P2.2 | 接收缓冲 **4 KB** 足够（`token` 帧最大才 101 B）；策略定死为"**先解出 `type` 再决定装不装配**，非 `token/done/error` 一律丢整帧不判死"。这条从"照抄 Passport 的教训"升级为"本机实测确认的硬约束" |
| 5 | STT 会出现 20 s 无响应后 RST；非语音输入返回幻觉文本；`language` 可省 | P2.1 | ① STT 请求加 **25 s 超时 + 一次重试**，超时期间屏上是"识别中"（不是"思考中"）；② 转录结果**长度/置信度过滤**：极短或空就丢弃不发问（Passport 也判空，我们加一条"≤2 字且非明确词"就丢）；③ 少发一个 `language` 字段，chunked 尾部更简单 |
| 6 | `session_key`/`title` 被忽略，且新建 thread 会把旧的置 inactive | P1.4 / P2.4 | 设备身份锚点**只有 `thread_id`**：一期就"一个固定 thread_id 存 NVS"，不要每轮新建（否则活跃线程标记一直漂）。Passport 的"45 s 换 thread 重发"保留，但**换完要把新 id 写回 NVS** |

工作量不变（10 人日）。P2 因为不用每轮重连、不用发 `language`，略微变简单；P3 未受影响。

---

## 9. 进度记录（我更新）

| 日期 | 阶段 | 做了什么 | 证据 | 下一步 |
|---|---|---|---|---|
| 2026-10-14 | 计划 | 工作计划落盘；核对凭据/门户/hooks/场景预算的真实状态 | 本文 §6 两处修正 | 等 §2 第 1 项 → 开工 P0；期间先出 P3.0 mockup |
| 2026-10-14 | **P0 完成** | 拿到账号后实测 4 个端点 + WS 帧抓取 + STT 时延取样 + 打断/复用验证（153 帧、11 次 STT 调用） | 本文 §7 全部为实测数据；探针脚本 4 个（本机 Temp，未入库） | 下一步 P1（凭据 v2 + 门户配网 + `octop_auth`），P3.0 阅读页 mockup 可并行；开工前需要你点头 |
| 2026-10-14 | **P1 完成** | 凭据 v2（`octop_url/username/password/stt_url/stt_key`，版本 2，旧记录自动清）、门户自建 Wi-Fi（扫描 + 真实试连后才落盘）、`octop_http.c` 登录与 403→重登判定 | `setup_tests` 主机测试 `PASS`（URL 规则、可选 STT 留空、写失败保留旧快照、v1/截断/垃圾记录清零） | 已并入 P2 |
| 2026-10-14 | **P2 完成** | `octop_wifi`（AP+STA、退避重连用 esp_timer 不在事件回调里 sleep）、`octop_stt`（路径不重复拼反代前缀）、`octop_ws`（分帧重组 + 超 21 KB 整帧丢弃 + `esp_event_handler_t` 回调签名）、`octop_link`（登录→agent→thread→WS→轮次、45 s/60 s 看门狗、世代丢弃迟到事件、修掉每轮漏一次转录堆） | 固件构建 `IDF LOW-NOISE BUILD: OK`（233.9 s，`warnings: 1` 是已知的 `factory` 分区告警）；`liboctop_link.a` 23,846 B 进镜像 | 设备端真机往返 |
| 2026-10-14 | **P3 完成** | 状态机改成 `COMP_RECEIVING`（继承旧"说话"姿态序号，场景不用重排）、流式期间禁止自动翻页、字符边界切页、4096 B 屏显截断；音量滑块改作拾音增益 0–30 dB | `companion_model_test` 主机测试 `PASS`（链路门、翻页、流式 vs 最终、5023 B 截断、渐暗与 6 档熄屏、错误被回复覆盖）；两张场景 PNG 由模拟器渲染核对（`拾音 55%`…`网络 已连接 · Octop 就绪`，无喇叭/语速行） | 人工在浏览器预览里走一遍交互 |
| 2026-10-14 | **P4 完成** | `muse_link_sdk`/`noise_core`/`product_info` 移出构建（进 `vendor/`），TTS 三件套与配网旧通道删除，BLE/HomeHub Kconfig 摘除 | map 文件里 `muse_link_sdk`、`noise_core` **0 引用**；`libbt.a` 不出现在按归档统计中（贡献 0 B）；`muse_companion.bin` 9,982,720 B / `ota_0` 11,468,800 B → 余量 1,486,080 B，其中 `libmain.a` 的 7,429,033 B 是内嵌字体与 GSP 资源 | 无需再动 |
| 2026-10-14 | **P5 部分完成** | 文档全部同步到 Octop 形态（README×2、SETUP_CN、BUILDING、CHARACTERS、PROJECT_MANUAL 重写；`generate_character.py`/`preview_character.py` 的用户可见文案改口）；`setup.html` 去掉真实服务器地址默认值 | `docs/PROJECT_MANUAL_CN.md` 为当前代码说明书，旧 Muse 说明书移到本机 `.codex-runs/docs-backup/` | **剩下唯一一件：USB 连设备 `iris system-update` 刷进去，配一次 Octop，真机说一句话看屏显** |
| 2026-10-14 | **P3.1/P3.2/P3.3 完成** | 新增第三个 GSP 场景"阅读页"（`ui/read.json`，正文框 444×300）；分页器拆成 `main/companion_pager.h`（按 440 px 行宽 × 10 行切页，UTF-8 码点安全，页索引越界钳到最后一页）；屏显上限 4096→16384 B；状态机插入 `COMP_STT`（松手先"正在把你的话转成文字…"，收到转录才进"思考中"）；翻页三通道（轻触正文 / ≥64 px 横滑 / 长按回首页 + 页脚按钮），横滑用 `esp_gsp_set_pointer_observer()` 取坐标并用 `swipe_held` 吞掉紧随的原生 click；自动翻页限定在阅读页且只在收完文字后跑；`animation_max_players` 8→9 | `companion_model_test` 与 `setup_tests` 双 `PASS`（新增 STT 转移 + 500 字=3 页 + 拉丁 360 B/页 + emoji 220 字/页 + 不切半字符 + 截断尾不卡死）；`gspc diagnose` 三个场景均 `[]`；模拟器 4 张实帧：`read_auto2.png`（第 2/2 页，无豆腐块）、`read_swipe.png`（第 1/2 页 + 自动翻页提示，证明滑动只翻一页）、`read_last.png`（末页"读完轻触返回"）、`home_stt3.png`（首段流式即切阅读页）；固件 `IDF LOW-NOISE BUILD: OK`，`muse_companion.bin` 9,984,144 B（+1,424 B） | 真机刷写 + 端到端 |
| 2026-10-14 | P3 计划修正（实测推翻的 4 条） | ① `swipe` 是**场景间**导航开关，不能用来翻回答页，翻页必须应用自己做；② `esp_gsp_event_t` **没有坐标**（只有 type/action_id/arg/scene_id），"用 press/release 坐标判方向"的退路不成立，唯一坐标源是 pointer observer；③ 20 px 字号实测行高 **30 px**（ascent 24 + descent 6），不是 26 px，所以正文框从计划的 444×270 提到 444×300 才放得下 10 行；④ GSP 1.4.0 **没有运行期字号属性**，"字号 20/18/16 三档"从 P3.3 删除（要做只能编译期堆三套 label + 三套页几何） | 1.4.0 schema 与 `esp_gsp.h` 逐条核对；行高用 PIL/FreeType 实测（CJK 字宽恰好 20.0 px，ASCII 均值 10.83 px） | 已并入上一行 |
| 2026-10-14 | 模拟器字库事实 | GSPC 打进 bundle 的是按 `main/*.c` 静态文案裁剪的**子集字体**，模拟器不带 `--dynamic-font` 渲染动态回复会出豆腐块；真机走 `app_config.ttf` 整份 NotoSansSC，不受影响。BUILDING.md 与 PROJECT_MANUAL §14 已记录 | 不带该参数的 `home_stt.png` 显示 `?`，带参数后 `read_auto2.png` 全字正常 | 需要 emoji 时再验 NotoSansSC 是否含 emoji 字形（P3.4 唯一未验项） |
| 2026-10-14 | 环境事实修正 | 本机 `idf.py` 恒定以 `-DCCACHE_ENABLE=True` 配置（与 `IDF_CCACHE_ENABLE` 无关，实测删掉变量后仍为 True）。ccache 只影响编译耗时，链接与体积是真实的；`build_octop.ps1` 与 BUILDING.md 已按事实改写，不再声称关闭 ccache | 两次构建日志的 cmake 命令行 | 需要冷编译计时基线时设 `CCACHE_DISABLE=1` |

### 9.1 交付状态一句话

固件可刷：`projects/muse_companion/build/muse_companion.bin` 已由本轮 P3 改动后重新构建（`IDF LOW-NOISE BUILD: OK`，9,984,144 B，1 条已知 `factory` 分区告警），`system-update-bundle` 目标在本构建图内（`build/build.ninja:271`），`ui_apps.bin` 由 `bundle.gspb` 打包（现在是 **3 个场景**：main / settings / read），`python mosaico.py iris system-update --project projects/muse_companion` 可直接出包刷写。**尚未验证的只有设备端端到端那一环**（麦克风 → STT → WS → 屏显），因为它需要设备在手边且填入你的 Octop 凭据。

