# 第 5 阶段测试说明：AV1

## 1. 编译

没有新文件、没有新依赖，只改了：`common/VideoProto.*`、`winvnc/winvnc/vncNvencEncoder.cpp`、`winvnc/winvnc/vncVideoStreamer.cpp`、`vncviewer/VideoDecoder.*`、`vncviewer/ClientConnectionVideo.cpp`、`vncviewer/ClientConnectionWindow.cpp`。

AV1 编码要 NVENC SDK 12 及以后的头文件。vcpkg 的 `ffnvcodec` 已经是 12.x 或更新，不用改。

## 2. 需要的硬件 / 软件

| 端 | 条件 |
|---|---|
| 服务端编码 AV1 | NVIDIA **RTX 40 系**及以后，驱动较新 |
| 查看器解码 AV1 | 显卡能硬解 AV1：NVIDIA RTX 30 系及以后；AMD RX 6000 系及以后（6400/6500 除外）；Intel 11 代及以后的核显、Arc。另外要装应用商店里的「**AV1 视频扩展**」（Win11 一般自带） |

两边都满足才会用 AV1，否则自动退回 HEVC，再退回 H.264，不需要手动设置。

## 3. 看日志

查看器：
```
video: requested, codecs AV1, HEVC, H.264      ← 本机能硬解的格式，按优先顺序
video: stream N: 1920x1080 at 0,0, AV1, GPU decoding
```
服务端：
```
video: streaming 1920x1080 at 60 fps, 10000 kbps, AV1
```
服务端显卡不支持 AV1 时：
```
video: AV1 not available (this GPU cannot encode AV1), trying HEVC
```

## 4. 解码测试程序（可选，先确认查看器那台机器能解 AV1）

在**查看器那台机器**上：
```
ffmpeg -f lavfi -i "color=c=0x3080C0:s=640x360:r=30" -frames:v 30 -c:v libsvtav1 -svtav1-params pred-struct=1 -pix_fmt yuv420p -f obu solid.obu
cl /std:c++17 /EHsc /I. tests\video_decoder\video_decoder_test.cpp vncviewer\VideoDecoder.cpp common\VideoProto.cpp common\ExtChannel.cpp
video_decoder_test.exe solid.obu 640 360 112 168 88
```
最后几行会列出本机能不能用显卡解 H.264 / HEVC / AV1。（我这边 Wine 没有 AV1 解码器，只验证了 AV1 文件能正确切分成 30 帧。）

## 5. 测试用例

| # | 环境 | 操作 | 预期 |
|---|---|---|---|
| 1 | 服务端 RTX 40，查看器能硬解 AV1 | 打开视频模式 | 日志显示 AV1、GPU decoding；画面正常，流畅 |
| 2 | 同上 | 和 HEVC 对比：同样的码率上限下看文字和运动画面 | AV1 画质应该更好，或者同样画质码率更低 |
| 3 | 同上 | 单窗口 + 视频模式；调整窗口大小 | 用 AV1，大小变化后画面正确 |
| 4 | 同上 | 限速（比如 5 Mbps、100 ms 延迟） | 码率自动下降，画面不卡死；比 HEVC 糊得少 |
| 5 | 服务端 RTX 40，查看器**不能**硬解 AV1 | 打开视频模式 | 查看器日志的 codecs 里没有 AV1；用 HEVC 或 H.264 |
| 6 | 服务端 RTX 30 或更老，查看器能硬解 AV1 | 打开视频模式 | 服务端日志 `AV1 not available ..., trying HEVC`；画面正常 |
| 7 | 查看器没装「AV1 视频扩展」 | 打开视频模式 | 不请求 AV1（codecs 里没有），正常工作 |
| 8 | 新服务端 + 上一版查看器（没有编码列表） | 打开视频模式 | 和以前一样，用 HEVC 或 H.264 |
