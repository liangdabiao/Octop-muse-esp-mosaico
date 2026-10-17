# 角色与动画生成

本项目通过设备自己的 Octop 连接生成角色，电脑不读取 Octop 账号。默认描述延续现有奶白色连帽玩偶，也可用任意文字描述创作新角色。生成过程需要设备联网并通过 USB 连接电脑；日常显示动画不需要电脑。

## 生成与预览

```powershell
$env:PYTHONUTF8='1'
python projects/muse_companion/tools/generate_character.py
```

自由创作使用独立目录，避免覆盖原作品：

```powershell
python projects/muse_companion/tools/generate_character.py --description "A tiny round mint-green dragon with peach cheeks, gentle plush cartoon style, pure black background" --output projects/muse_companion/characters/mint-dragon
```

该工具保存 Octop 的原始回答、提示词、绘图源码和来源记录，不会执行生成的代码，也不会安装。先检查源码仅包含绘图计算，再运行主机预览：

```powershell
python projects/muse_companion/tools/preview_character.py --source projects/muse_companion/characters/muse-generated/muse_character.c
```

预览包含待机、聆听、思考、接收、困倦、错误、开心、开机八种动画，每种四秒。检查程序验证输出边界、每个像素已初始化、黑底和动画帧确实变化；人工检查角色外观及动作。生成失败、检查失败或 Octop 回答不完整时不启用角色。修改可把原源码与具体要求放进本地提示文件，再用 `--prompt-file` 请求 Octop 修订，仍需重新检查。

可添加 `--asan` 进行 AddressSanitizer 检查。普通编译、320 帧边界哨兵和像素初始化检查已验证；ASan 需在使用者的编译环境另行验证。工具不会忽略 ASan 错误。源码需先人工审查，静态规则和哨兵不等于运行任意代码的沙箱。

## 启用与恢复

确认预览后，将通过检查的动画选为界面资源：

```powershell
python projects/muse_companion/tools/prepare_ui.py --character projects/muse_companion/characters/muse-generated
python mosaico.py project sim --project projects/muse_companion --interactive
```

模拟器中检查状态变化、角色动画、设置和字幕，随后按设备操作指南进行完整资源更新：

```powershell
python mosaico.py iris system-update --project projects/muse_companion
```

角色资源变化必须使用 `system-update`。不要调用 `vendor/` 里旧设备 SDK 自带的串口刷写脚本。该项目保持 Recovery 为唯一固件写入者；必须验证同一设备重新进入正常应用、显示新动画并能继续对话。

原玩偶素材保留在 `ui/assets/`。恢复时：
恢复后仍有八种原生位置动态：待机呼吸、聆听轻摆、思考起伏与圆点、说话点头、困倦慢呼吸、错误摇晃、开心跳跃与爱心、开机招呼。开心复用原说话笑脸，开机复用原待机招手图；保留原像素素材，不运行生成绘图程序。姿态图本身不做逐帧面部变形。

```powershell
python projects/muse_companion/tools/prepare_ui.py --character original
python mosaico.py iris system-update --project projects/muse_companion
```

## 接入方式与边界

[旧版官方角色流程](https://github.com/facebookincubator/muse-gadget-sdk/blob/main/esp32/tools/muse/AVATAR_RECIPE.md)通过设备向 Muse 请求绘图源码，再在电脑检查、编译和更新；该链接只作为历史参考，本项目已不再连接 Muse。生成思路保持不变，改为 128×128 原创柔和卡通绘图，请求走设备自己的 Octop 会话。电脑将源码渲染为动画文件，GSP 播放检查后的资源；设备不运行 Octop 生成的任意 C 代码，也没有直接在手机里点一下就更换形象的入口。

生成与语音、文字提问共用一条 Octop 对话链路，由链路任务串行执行，因此生成期间不会出现两路回复互相覆盖。生成期间按 AI 键可取消并回到提问。回答上限 64 KiB；45 秒内连一帧都没有、或已经开始后 60 秒没有新帧，设备会取消本轮并提示重试（`components/octop_link/octop_link.c` 的 `REPLY_CAP`、`FIRST_TOKEN_WAIT_US`、`TOKEN_IDLE_US`）。电脑侧 `generate_character.py` 另有 `--timeout`，默认 360 秒，超时同时请求设备取消。默认角色的风格描述固定，角色内容可自由描述，不限制为某一种人物。

`generation.json` 记录 Octop 来源与源码哈希，`preview-validation.json` 记录实际检查结果。动画按固定循环播放；本项目没有任何音频输出，"接收"姿态只是动画，不涉及口型同步。实际生成及安装结果以验收记录为准。

Windows 使用应用自己的编译适配器，硬件与模拟器共用 `ui/companion_rgb565.yaml` 的八播放器配置。动画以 JPEG 帧压缩，当前 40 帧预览在设备资源中降为 20 帧/四秒，以便保留原 1 MiB 资源分区；检查压缩后的实际屏幕效果。更复杂角色可能超出分区，构建应阻止安装，需另行优化或规划资源布局。


## 画质与资源预算

当前布局保留状态栏、按钮和对话区，角色区域为 300×300。选择逐帧资源时必须替换现有角色并核对 `ui_apps` 的 3MiB 容量；提高 JPEG 品质可增加清晰度，也会增加镜像大小。先在模拟器验收，再通过 `system-update` 更新资源。初始角色使用姿态图片的位置动画。
