# 第 2a 阶段测试说明：显卡视频模式

## 1. 编译

### 依赖（vcpkg，已写进 `vcpkg.json`）

| 包 | 用途 | 用在 |
|---|---|---|
| `ffnvcodec` | NVENC API 头文件（NVIDIA Video Codec SDK，MIT 许可，只有头文件） | 服务端 |
| `libyuv` | NV12 → BGRA 颜色转换 | 查看器 |

Visual Studio 工程是 vcpkg 清单模式，第一次编译时会自动下载编译这两个包。系统库（`d3d11`、`dxgi`、`winmm`、`mfplat`、`mfuuid`）在源文件里用 `#pragma comment(lib, ...)` 指定了，CMake 里也加了。

**NVENC API 版本**：头文件版本决定了对显卡驱动的最低要求。Ubuntu 上的版本是 12.1，需要 **NVIDIA 驱动 531 或更新**。vcpkg 里 `ffnvcodec` 的版本以你那边装到的为准；驱动太旧时，服务端日志会写 `the NVIDIA driver is too old for NVENC API x.y`。

新增文件：

| 文件 | 用途 |
|---|---|
| `common/VideoProto.*` | 视频通道协议（两端共用） |
| `winvnc/winvnc/vncVideoCapture.*` | DXGI 桌面复制抓屏 |
| `winvnc/winvnc/vncNvencEncoder.*` | NVENC 编码 |
| `winvnc/winvnc/vncVideoStreamer.*` | 抓屏 → 编码 → 发送的线程 |
| `vncviewer/VideoDecoder.*` | Media Foundation 解码 |
| `vncviewer/ClientConnectionVideo.cpp` | 查看器的视频模式逻辑 |

### 单元测试（可选）

```
cl /std:c++17 /EHsc tests\video_proto\video_proto_test.cpp common\VideoProto.cpp common\ExtChannel.cpp
```

`tests/video_decoder/` 里有一个解码器测试：用 ffmpeg 生成一段纯色 H.264，解码后检查尺寸和颜色。我在 Linux 上用 Wine 跑过，通过了。

## 2. 前提

- **服务端**：NVIDIA 显卡，驱动够新；要视频化的那个**显示器接在 NVIDIA 显卡上**。
- **查看器**：任何显卡都行（用 Windows 自带的解码器）；颜色选「Full colors」（32 位）；服务端缩放为 1/1。
- 两端都是新版本。

服务端日志里会写视频模式是否可用：可用时不会出现 `video mode unavailable: ...`；不可用时这一行会给出原因。

## 3. 使用

查看器窗口标题栏右键（系统菜单）→ **Video Mode (GPU)**，前面打勾表示开启；再点一次关闭。

## 4. 测试用例

| # | 操作 | 预期 |
|---|---|---|
| 1 | 局域网，1080p 显示器，开启视频模式，播放一段视频 | 画面流畅，接近 60 帧；查看器日志有 `video: stream N: 1920x1080 at 0,0`，服务端日志有 `video: streaming 1920x1080 at 60 fps, 20000 kbps, H.264` |
| 2 | 视频模式下操作鼠标键盘、打开菜单、拖动窗口 | 正常响应。**已知限制**：远程鼠标指针形状（比如文本框里的 I 形）不会变化 |
| 3 | 关闭视频模式 | 立即回到原来的办公模式，画面完整、清晰 |
| 4 | 视频模式下画面静止不动 | 几乎没有网络流量（静止时不发帧） |
| 5 | 远程弹出 **UAC 提示**或**锁屏**再解锁 | 短暂卡顿后恢复（服务端以服务方式运行时）；如果 5 秒内恢复不了，视频模式自动退出并弹出原因 |
| 6 | 远程**改分辨率** | 视频流自动按新分辨率重新开始 |
| 7 | **多显示器**：查看器显示全部显示器时开启视频模式 | 只有一个显示器（重叠最多的那个）是视频，其他显示器停在开启那一刻的画面；关闭视频模式后恢复 |
| 8 | 服务端**没有 NVIDIA 显卡**（或驱动太旧）时点菜单 | 弹出提示「The server does not offer video mode」，不影响正常使用 |
| 9 | 查看器颜色设成 256 色或 64K 色时点菜单 | 提示需要 Full colors |
| 10 | 视频模式下**断网再恢复**（自动重连） | 重连后回到办公模式（菜单不打勾），可以重新开启 |
| 11 | 用**限速**（比如 5 Mbps）的网络开启视频模式 | 帧率下降但画面不花、不卡死；服务端调试日志里每 10 秒有一行 `sent N frames ..., skipped M` |
| 12 | 新查看器连**原版服务端** | 点菜单提示服务端不支持 |
| 13 | 视频模式和文件复制粘贴同时用 | 都正常（文件传输会占带宽，视频帧率可能下降） |

## 5. 性能参考（请帮忙记录）

| 项 | 数值 |
|---|---|
| 服务端显卡 / 驱动版本 | |
| 分辨率 / 实际帧率 | |
| 查看器 CPU 占用（视频模式下） | |
| 主观延迟（在远程打字到屏幕出现字） | |

查看器 CPU 占用决定了 2b 要不要优先做硬件解码。

## 6. 出问题时请提供

- 两端日志里搜 `video`；
- 服务端显卡型号和驱动版本、显示器分辨率和数量；
- 查看器的颜色设置和缩放设置。
