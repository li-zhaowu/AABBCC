# 第 4 阶段测试说明：远程声音

## 1. 先跑探测程序（决定远程静音的做法）

远程静音现在的做法是：推流期间把**服务端的扬声器静音**，然后用 loopback 采集。问题是：有的声卡驱动静音后 loopback 也采不到声音。这个只能在真机上测，所以请先跑探测程序。

在**服务端那台机器**上（普通用户登录，接着扬声器或耳机）：

```
cd tests\audio_probe
cl /std:c++17 /EHsc audio_probe.cpp ole32.lib
audio_probe.exe
```

它会播放一个很小声的 440Hz 音，大约十几秒，分别测四种情况（不静音、扬声器静音、音量调到 0、只静音播放程序），最后打印一张表，并恢复原来的音量和静音状态。**请把整张表（包括最后一行 Windows 版本号）发给我。** 如果有好几台机器（Win10 / Win11、不同声卡），每台都跑一下更好。

看表的方法：静音那几行里，哪一列显示能采到声音，哪种做法就可行。

## 2. 编译

新增文件，已经加入 Visual Studio 工程和 CMake：

| 位置 | 文件 |
|---|---|
| 公共 | `common/AudioProto.*`（协议 + 抖动缓冲）、`common/AacCodec.*`（Windows 自带的 AAC 编解码） |
| 服务端 | `winvnc/winvnc/vncAudioStreamer.*` |
| 查看器 | `vncviewer/AudioPlayer.*`、`vncviewer/ClientConnectionAudio.cpp` |

新的链接库：服务端和查看器都要 `mfplat.lib`、`mfuuid.lib`（MSVC 里已经用 `#pragma comment` 自动链接，CMake 里也加了）。没有新的 vcpkg 依赖。

单元测试（Linux 下用 MinGW + Wine 跑过，都通过了）：
- `tests/audio_proto`：协议编解码、抖动缓冲。
- `tests/aac_codec`：AAC 解码（Wine 没有 AAC 编码器，编码部分只能在 Windows 上测）。在 Windows 上可以这样跑，会自动多测一项"编码再解码"：
  ```
  ffmpeg -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=1" -ac 2 -c:a aac -b:a 128k -f adts tone.aac
  cl /std:c++17 /EHsc /I. tests\aac_codec\aac_test.cpp common\AacCodec.cpp common\AudioProto.cpp common\ExtChannel.cpp
  aac_test.exe tone.aac
  ```
- `tests/audio_player`：查看器播放器的打开 / 播放 / 关闭（Wine 里没有声卡，只验证没有崩溃、流号过滤正确）。

## 3. 使用

查看器窗口的系统菜单（点左上角图标）里新加了 **Remote Audio**，勾上就开始听远程声音，远程机器的扬声器同时被静音；再点一次取消，远程扬声器恢复。断开连接也会恢复。断线重连后需要重新勾选。

## 4. 看日志

服务端：
```
audio: streaming 48000 Hz, 2 channel(s), 128 kbps AAC, speakers muted
```
查看器：
```
audio: stream 1, 48000 Hz, 2 channel(s), 128 kbps
audio: playing 48000 Hz, 2 channel(s)
audio: stream 1 stopped, N underrun(s), M frame(s) dropped     ← 关闭时的统计
```
`underrun` 多说明网络抖动大、声音会断断续续；`dropped` 多说明积压过（比如网络卡顿后追进度）。

## 5. 测试用例

| # | 操作 | 预期 |
|---|---|---|
| 1 | 远程播放音乐或视频，勾选 Remote Audio | 本地约 0.2 秒内听到声音，连续不断；远程扬声器没声音 |
| 2 | 取消勾选 | 本地停止，远程扬声器恢复有声 |
| 3 | 勾选状态下直接断开连接 / 关闭查看器 | 远程扬声器恢复有声 |
| 4 | 远程**原本就是静音**的，勾选再取消 | 远程仍然是静音（恢复的是原来的状态） |
| 5 | 远程什么都不播放，勾选一会儿后再开始播放 | 立即能听到，没有爆音、没有越来越大的延迟 |
| 6 | 两个查看器同时连同一台服务端，都勾选；关掉其中一个 | 另一个仍然有声；两个都关掉后远程扬声器才恢复 |
| 7 | 同时开着视频模式（或单窗口模式）看视频 | 声音和画面都流畅，声音略早或略晚一点点可以接受 |
| 8 | 同时复制粘贴一个大文件 | 声音不断（声音优先级最高） |
| 9 | 远程切换默认播放设备（比如从扬声器换成耳机） | 2 秒内自动跟上，声音继续 |
| 10 | 本地切换默认播放设备 | 2 秒内从新设备出声 |
| 11 | 远程**没有任何播放设备**（禁用所有声卡）时勾选 | 几秒后弹出提示说明原因，菜单取消勾选 |
| 12 | 连原版 UltraVNC 服务端，点 Remote Audio | 提示服务端不支持，其他功能正常 |
| 13 | 用限速工具模拟 100ms 延迟、1% 丢包 | 声音基本连续（TCP 重传会偶尔造成短暂停顿，之后自动恢复） |

如果第 1 条远程扬声器静音了、但本地**也听不到声音**（日志显示在推流），那就是第 1 节说的驱动问题，请把探测程序的结果发给我，我改用别的静音方式。
