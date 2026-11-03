# 第 1 阶段测试说明：复制粘贴文件

## 1. 编译

新增的源文件（Visual Studio 工程和 CMake 都已加入）：

| 文件 | 用途 |
|---|---|
| `common/FileClipProto.h/.cpp` | 协议编解码、路径安全检查（两端共用） |
| `common/FileClipboard.h/.cpp` | 剪贴板引擎：监听复制、虚拟文件、按需读取（两端共用） |

改动的文件：`winvnc/winvnc/vncExtChannel.*`、`winvnc/winvnc/vncclient.cpp`、`winvnc/winvnc/winvnc.cpp`、`vncviewer/ClientConnectionExt.*`、`vncviewer/ClientConnection.cpp`、`common/ExtChannel.*`。

新代码需要链接 `ole32`、`shell32`、`uuid`，源文件里已经用 `#pragma comment(lib, ...)` 写好，一般不用改工程设置。

### 单元测试（可选）

```
cl /std:c++17 /EHsc tests\fileclip_proto\fileclip_proto_test.cpp common\FileClipProto.cpp common\ExtChannel.cpp
fileclip_proto_test.exe

cl /std:c++17 /EHsc /DUNICODE /D_UNICODE tests\fileclip_e2e\fileclip_e2e.cpp common\FileClipboard.cpp common\FileClipProto.cpp common\ExtChannel.cpp
fileclip_e2e.exe
```

第二个测试会**使用本机剪贴板**（运行时不要复制别的东西），在 `%TEMP%\uvnc_fileclip_e2e` 下生成测试文件。两个测试都输出 `all ... tests passed` 才算通过。我在 Linux 上用 Wine 跑过这两个测试，都通过了。

## 2. 前提

- 服务端设置里**「允许文件传输」要打开**（文件复制粘贴跟随这个权限）。
- 查看器不能是「只看模式」，也不能勾选「禁用剪贴板」。
- 查看器日志（`-loglevel 10 -logfile ...`）里会出现 `channels 0x3`（0x1 是控制通道，0x2 是文件剪贴板）。

## 3. 测试用例

「本地」指查看器所在的机器，「远程」指服务端所在的机器。

### 基本功能

| # | 操作 | 预期 |
|---|---|---|
| 1 | 本地复制一个小文件 → 在远程桌面或资源管理器里 Ctrl+V | 文件出现在远程，内容一致 |
| 2 | 远程复制一个小文件 → 在本地资源管理器里 Ctrl+V | 文件出现在本地，内容一致 |
| 3 | 复制**文件夹**（包含子文件夹、空文件夹、0 字节文件） | 目录结构完整，空文件夹也建出来了 |
| 4 | 一次选中**多个文件和文件夹**复制 | 全部粘贴过来 |
| 5 | **中文文件名**、带空格的文件名 | 名字正确 |
| 6 | **大文件（> 4GB）** | 有进度条，完成后校验一致（例如 `certutil -hashfile 文件 SHA256`） |
| 7 | 大文件粘贴到一半，在进度对话框里点**取消** | 立即停止；之后再复制粘贴别的文件正常 |
| 8 | 同一批文件**粘贴两次**（粘贴到两个不同目录） | 两次都成功 |

### 和原有功能的配合

| # | 操作 | 预期 |
|---|---|---|
| 9 | 复制文件之后，再复制一段**文字** | 文字照常同步到对端；对端剪贴板里的虚拟文件被替换 |
| 10 | 远程复制文件，**本地粘贴之前**又在远程复制了别的东西（不是文件） | 本地粘贴时提示没有可粘贴的内容（或粘贴的是新内容），不会粘出旧文件 |
| 11 | 原有的「文件传输」窗口 | 照常可用 |

### 异常情况

| # | 操作 | 预期 |
|---|---|---|
| 12 | 粘贴大文件过程中**断开连接** | 资源管理器报读取错误，不会卡死；对端剪贴板里的虚拟文件被清掉 |
| 13 | 复制后，在源端把文件**删掉或改名**，再到对端粘贴 | 资源管理器报错，不会卡死 |
| 14 | 服务端设置里**关掉「允许文件传输」**再连 | 不能复制粘贴文件（文字剪贴板照常） |
| 15 | 查看器用**只看模式**连 | 不能复制粘贴文件 |
| 16 | 新查看器连**原版服务端**、原版查看器连**新服务端** | 连接和文字剪贴板正常，只是没有文件复制粘贴 |

### 服务端运行方式（重点）

| # | 操作 | 预期 |
|---|---|---|
| 17 | 服务端**以 Windows 服务方式**运行（安装成服务），做用例 1 和 2 | 都能成功。这是风险最高的一项：远程资源管理器要跨用户调用服务端（SYSTEM 身份）的 COM 对象。服务端日志里应有 `COM security set for file copy and paste` |
| 18 | 服务端以**普通程序**方式运行（非服务），做用例 1 和 2 | 都能成功 |
| 19 | 服务端以服务方式运行，远程**锁屏**状态下，远程复制过的文件在本地粘贴 | 拒绝（报错），锁屏状态下不读取远程文件 |

### 网络和性能

| # | 操作 | 预期 |
|---|---|---|
| 20 | 走公网（或用限速工具模拟），粘贴一个几百 MB 的文件，同时操作远程桌面 | 速度接近带宽上限；远程画面和鼠标键盘仍然能响应（会比平时慢一些） |

## 4. 排查：按日志定位是哪一步出了问题

日志里搜 `file clipboard`。以「远程复制 → 本地粘贴」为例，正常情况下依次出现：

| 步骤 | 哪一端的日志 | 应该看到 | 如果看到的是 |
|---|---|---|---|
| 1. 发现复制 | 服务端 | `clipboard has N file(s)/folder(s)` | 什么都没有：剪贴板变化没被发现（现在有每 0.5 秒的轮询兜底） |
| 2. 生成清单 | 服务端 | `offered N entries` | `no user token` / `impersonation failed` / `cannot access ...`：拿不到用户身份或访问不了文件 |
| 3. 收到清单 | 查看器 | `peer offers N entries`，然后 `... are on the clipboard, ready to paste` | `OleSetClipboard failed`：本地剪贴板写入失败 |
| 4. 粘贴读取 | 服务端 | （调试级别）`serving ...` | `cannot open ..., error N` / `read for offer ... but the current offer is ...` |
| 5. 出错 | 查看器 | — | `peer reports error N on stream ...`：对端读文件失败，看对端日志里的原因 |

本地复制 → 远程粘贴时，两端的角色正好反过来。

## 5. 出问题时请提供

- 两端的日志（查看器 `-loglevel 10 -logfile ...`；服务端打开调试日志），里面搜 `file clipboard`；
- 服务端的运行方式（服务还是普通程序）和两端的 Windows 版本；
- 失败的是哪一个方向（本地 → 远程，还是远程 → 本地）。
