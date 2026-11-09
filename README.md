# MaToy

把 AI Passport 变成随身电台、Mac 遥控器和音视频终端。

MaToy 由 ESP32-C3 固件和 macOS 菜单栏应用组成：开发板负责屏幕、按键和声音交互，Mac 负责桌面与摄像头采集、输入控制和音频传输。开机显示自定义插画，随后进入「电台」「远程桌面」「远程监控」三个入口。

## 功能

| 入口 | 可以做什么 | 布局 |
| --- | --- | --- |
| 电台 | 直接连接网络广播，切台、暂停、选择地区、调整音量 | 竖屏 |
| 远程桌面 | 查看并操作 Mac 当前会话，支持鼠标、屏幕键盘、光标跟随放大和全桌面切换 | 横屏 |
| 远程监控 | 查看 Mac 摄像头，分别开启或关闭镜头、环境音监听和按键对讲 | 竖屏 |

- 桌面提供全桌面、3 倍跟随和 6 倍文字模式，缩放仅作用于发送给开发板的画面。
- 屏幕键盘包含字母、数字及 Enter、Tab、退格等特殊键。
- 桌面语音支持 Mac 系统声音或麦克风；监控监听使用 Mac 麦克风。
- 对讲采用按住讲话、松开收听的方式，两端轮流说话。
- 电台目录包含 345 个频道、31 个地区；公开广播源的在线情况可能变化。

电台只需开发板联网。远程桌面和监控需要 Mac 应用运行，并与开发板处于可互通的局域网。监控画面来自 Mac 摄像头。

## 硬件与环境

| 项目 | 配置 |
| --- | --- |
| 设备 | FoloToy AI Passport，ESP32-C3 |
| Flash / 屏幕 | 8 MB Flash、无 PSRAM、240×320 屏幕 |
| 交互 | 三个实体按键、麦克风、扬声器 |
| 网络 | 开发板使用 2.4 GHz Wi-Fi；Mac 可使用同一局域网的 Wi-Fi 或有线网络 |
| 固件工具链 | ESP-IDF v5.5.3 |
| Mac 发送端 | Apple 芯片，随附应用最低 macOS 26.0 |
| USB 接入工具 | Python 3.10 及以上，依赖见 `tools/requirements.txt` |

USB 用于刷机、写入网络配置和配对。日常画面、操作和音频通过局域网传输，使用 TCP 45902～45905。

## 快速使用

获取项目：

```zsh
git clone https://github.com/huangama666/MaToy.git
cd MaToy
```

### 已配对的设备

在项目根目录打开当前随附的 Mac 应用：

```zsh
open "outputs/AI Passport 实时控制.app"
```

开发板连接已保存的 Wi-Fi 后，上下键选择入口，短按确定进入。

当前随附的应用和固件来自此前发布，保留原文件名及内部名称；项目与后续源码构建使用 **MaToy**。因此，随附应用在 macOS 权限列表中仍可能显示「AI Passport 实时控制」。

### 首次接入、换 Mac 或换网络

用 USB 数据线连接设备，在项目根目录准备工具：

```zsh
python3 -m venv .venv
PY="$PWD/.venv/bin/python"
"$PY" -m pip install -r tools/requirements.txt
"$PY" tools/接入.py port
```

1. Wi-Fi 尚未配置时，刷临时配网固件，再运行 `"$PY" tools/接入.py wifi`。
2. 刷入正式应用镜像，保留网络配置。
3. 退出旧的 Mac 发送端，运行 `"$PY" tools/接入.py pair`，按提示选择 Mac 局域网地址。
4. 打开 Mac 应用，在系统提示中允许所需权限，并按提示重新打开。

分区检查、完整命令和恢复方法见 [使用与接入说明](docs/使用与接入说明.md)。

### Mac 权限

| 权限 | 用途 |
| --- | --- |
| 屏幕与系统音频录制 | 桌面与系统声音采集 |
| 辅助功能 | 鼠标和键盘控制 |
| 摄像头 | 监控画面 |
| 麦克风 | 环境音监听及语音对讲 |
| 本地网络（系统询问时） | 与开发板通信 |

重新构建并签名后，macOS 可能要求重新授权。配对文件保存在 `work/mac-screen-pair.json`，其中包含控制密钥，**分享或发布项目时必须排除**。

## 常用按键

| 场景 | 操作 | 功能 |
| --- | --- | --- |
| 首页 | 上下键 / 短按确定 | 选择 / 进入 |
| 电台 | 短按上下键 / 短按确定 | 切台 / 暂停或继续 |
| 电台 | 长按上键 / 长按下键 | 增加 / 减小音量 |
| 电台 | 长按确定 / 双按确定 | 打开选台菜单 / 返回首页 |
| 远程桌面 | 短按上下键 / 短按确定 | 沿当前方向移动鼠标 / 单击 |
| 远程桌面 | 长按确定 | 切换横向与纵向移动 |
| 远程桌面 | 双按确定 / 三连按确定 | 键盘开关 / 全桌面与原放大视图切换 |
| 远程桌面 | 鼠标模式长按上键 | 打开右上角分类菜单 |
| 监控 | 上下键 / 短按确定 | 选择并执行镜头、声音、对讲等操作 |
| 对讲 | 开启对讲后按住下键 | 向 Mac 讲话，松开恢复收听 |

监控每次进入默认开启摄像头、关闭声音。镜头与声音独立控制；离开监控会结束该会话的采集。完整键盘操作见使用说明。

## 开发与构建

先准备并激活 ESP-IDF v5.5.3，Mac 端安装带有相应 macOS SDK 的 Xcode Command Line Tools。详细环境步骤见 [开发与刷入说明](docs/开发与刷入说明.md)。

在项目根目录执行：

```zsh
# 正式固件：编译、合并并校验分区布局
bash scripts/build-firmware.sh

# Mac 发送端：Swift 编译及本机签名
bash scripts/build-mac.sh

# 需要重新构建临时配网固件时
bash scripts/build-wifi-setup.sh
```

| 构建产物 | 路径 |
| --- | --- |
| 正式应用镜像 | `build/firmware/MaToy.bin` |
| 正式完整镜像 | `build/firmware/MaToy-full.bin` |
| Mac 应用 | `outputs/MaToy.app` |
| 临时配网应用 / 完整镜像 | `build/wifi-setup/MaToy.bin` / `MaToy-full.bin` |

本项目不携带 ESP-IDF、编译器或下载组件缓存。首次构建需准备外部工具链并按依赖锁下载组件。构建脚本不会自动刷机或启动应用。

### 使用随附固件升级

确认设备采用本项目兼容分区表后，可在上述 USB 工具环境中只更新应用：

```zsh
PORT=$("$PY" tools/接入.py port) && test -n "$PORT" &&
  "$PY" -m esptool --chip esp32c3 --port "$PORT" --baud 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x10000 firmware/AI-Passport-Monitor-Talk-app.bin
```

刷自己构建的版本时，将最后一个路径换成 `build/firmware/MaToy.bin`。应用镜像写入 `0x10000`；完整镜像从 `0x0` 写入，用于初始化布局，可能覆盖已有配置。随附镜像的大小、写入地址和 SHA-256 见 [固件清单](firmware/manifest.json)。

## 项目结构

```text
source/
  firmware/      正式固件 C 源码、BSP、资源及依赖锁
  mac/           macOS Swift 发送端与 Info.plist
  wifi-setup/    临时 USB 配网固件源码
firmware/        已发布镜像及校验清单
scripts/         构建脚本
tools/           USB 配网、串口检测与配对
assets/          生成中文字库所需字体
outputs/         Mac 应用
work/            本机私有配对文件
docs/            使用、开发及刷入说明
第三方许可/      字体和依赖的许可文本
```

## 当前验证范围

随附版本已通过固件构建、布局与刷入校验、Mac 应用构建及签名校验；收到了真实桌面、摄像头图像和 Mac 麦克风音频，完成模拟板端上行音频及开关状态验证。实体双向对讲听感、长时间稳定性和实际网络延迟仍需继续验证。此处不承诺固定帧率、延迟或免提全双工通话。

MaToy 使用 macOS 当前会话及系统权限，不绕过锁屏登录。远程功能按局域网使用设计。

## 项目来源与许可

MaToy 基于 [FoloToy AI Passport](https://github.com/folotoy/ai-passport) 的开发基线和 BSP 构建。AI Passport 是硬件名称，MaToy 是本项目名称。

原项目许可见 [LICENSE](source/firmware/LICENSE)；字体及依赖许可见 [第三方许可](第三方许可)。
