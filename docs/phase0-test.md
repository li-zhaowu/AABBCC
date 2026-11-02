# 第 0 阶段测试说明：扩展消息通道

第 0 阶段只搭了底层通道，**界面上没有任何新功能**。测试的重点是：新旧版本互相连接都正常，新版之间能协商成功，而且长时间运行稳定。

## 1. 编译

改动涉及服务端（winvnc）和查看器（vncviewer）。两套构建方式都已经加了新文件：

- Visual Studio 工程：`winvnc/winvnc/winvnc.vcxproj`、`vncviewer/vncviewer.vcxproj`
- CMake：`cmake/winvnc/CMakeLists.txt`、`cmake/vncviewer/CMakeLists.txt`

新增的源文件：

| 文件 | 用途 |
|---|---|
| `common/ExtChannel.h/.cpp` | 协议核心（两端共用） |
| `winvnc/winvnc/vncExtChannel.h/.cpp` | 服务端接入 |
| `vncviewer/ClientConnectionExt.h/.cpp` | 查看器接入 |

新代码用到 C++17 的 `std::thread` / `std::mutex`，工程本来就是 `stdcpp17`，不需要额外设置。

可选：协议核心有单元测试，Windows 上可以用 MSVC 编译运行：

```
cl /std:c++17 /EHsc tests\ext_channel\ext_channel_test.cpp common\ExtChannel.cpp
ext_channel_test.exe
```

输出 `all ext_channel tests passed` 就是通过（中间会打印两行预期内的错误日志，不用管）。

## 2. 打开日志

- **查看器**：命令行加参数，例如
  `vncviewer.exe -loglevel 10 -logfile C:\temp\viewer.log 服务器地址`
- **服务端**：在服务端设置里打开调试日志（写入 `WinVNC.log`）。

## 3. 测试用例

| # | 组合 | 预期 |
|---|---|---|
| 1 | **新查看器 → 新服务端** | 正常连接和操作。查看器日志里有 `Extension channel negotiated with server, channels 0x1`；服务端日志里有 `Extension channel protocol extension enabled` 和 `extension channel negotiated`。 |
| 2 | **新查看器 → 原版 UltraVNC 服务端** | 正常连接和操作，查看器日志里**没有** `negotiated`。 |
| 3 | **原版 UltraVNC 查看器 → 新服务端** | 正常连接和操作，服务端日志里**没有** `Extension channel protocol extension enabled`。 |
| 4 | 新 ↔ 新，**开启加密**（SecureVNC 插件或 TLS） | 同用例 1。 |
| 5 | 新 ↔ 新，连接中**修改查看器的编码选项**（会重发 SetEncodings） | 连接不断，没有异常。 |
| 6 | 新 ↔ 新，**断网再恢复**，触发自动重连 | 重连后日志里再次出现 `negotiated`。 |
| 7 | 新 ↔ 新，**连接 30 分钟以上**，期间正常操作 | 不断线、不卡顿。后台每 2 秒有一次 Ping/Pong，流量可以忽略。 |
| 8 | 新 ↔ 新，**多个查看器同时连**一个服务端 | 都正常，关掉其中一个不影响其他。 |
| 9 | 新 ↔ 新，查看器**正常关闭** / **直接结束查看器进程** | 服务端能正常清理，接受下一个连接；服务端进程不卡住、不崩溃。 |

如果某个用例失败，把两端的日志发给我就行。
