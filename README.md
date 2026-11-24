# MeowLive2D C++ 重构

本项目将同级 Rust 工程迁移为 Windows x64 主服务与 Qt 6.12.1 Widgets 客户端。WSL2 用于开发和交叉构建。服务端提供基础 HTTP 接口，Qt 客户端通过协议 v3 配对控制和音频通道。

## 构建入口

工程使用 C++26 和 Xmake 3.1.2，第三方 C++ 包通过 Xmake 的包管理功能接入。WSL 服务端使用 Clang、`clang-scan-deps` 和提供 `std` 模块的标准库构建。

```sh
xmake f -p linux --toolchain=clang
xmake build meowlive-server
xmake run meowlive-server
```

Windows 构建同时包含 `meowlive-server` 和 `meowlive-desktop`。从 WSL2 生成 Windows x64 程序需要 Linux 宿主 Qt 工具、同版本的 Windows MSVC ABI Qt 库、clang-cl、lld-link、Windows SDK 和 MSVC CRT。

在 Windows 目标 Qt SDK 安装同版本的 Qt WebSockets 模块，并在运行桌面端时把 `Qt6WebSockets.dll` 放在 EXE 同级。

两端通过命名模块导入共享协议。Windows 目标通过标准库头文件使用 MSVC STL，Linux 服务端的相关模块使用 `import std;`。

## 测试

从项目根目录运行 C++ 协议契约测试和进程联调。联调会启动本机 C++ 服务，验证 HTTP、鉴权及双 WebSocket。

```sh
xmake test 'meowlive-protocol-contracts/protocol_v3' 'meowlive-transport-integration/stage_2_transport'
```

可将同一联调用于旧版 Rust 主服务。

```sh
xmake build meowlive-transport-integration
xmake run meowlive-transport-integration --rust ../MeowLive2D/target/debug/meowlive-server
```

## 本机连接

服务默认监听 `127.0.0.1:19601`，从 Windows 的 `%LOCALAPPDATA%\MeowLive2D\server.toml` 读取配置。可用 `--data-dir`、`MEOWLIVE_DATA_DIR` 指定数据目录，用 `--config` 指定 TOML。日志和上传暂存目录位于数据目录内。桌面端可通过 `MEOWLIVE_SERVER_URL` 指定服务地址。开启鉴权后，桌面端使用设备凭据建立双通道，管理员在窗口中单独登录。[服务配置样例](config/server.example.toml)

## C++ 格式化

构建机需要在 `PATH` 中提供 `clang-format`。`xmake build` 成功后会按 `.clang-format` 就地格式化 `apps`、`libs`、`adapters` 和 `tests` 中的 C++ 源码、头文件和命名模块。Allman 风格使用四空格缩进，代码块的左大括号单独起行。也可以直接运行 `xmake format`。

## 模块边界

- `apps/server` 装配 Agent、应用用例和外部适配器，Drogon 1.9.14 的路由放在 `transport`。
- `libs/application` 依赖 `libs/domain` 并定义外部接口，`adapters` 实现这些接口。`libs/agent` 放主服务调用的决策和工具轮次逻辑。
- `libs/protocol` 定义两端共享的控制和音频协议。`apps/desktop` 通过 `services` 访问主服务，WASAPI、VTube Studio 和 OBS 集成放在 `libs/desktop-runtime`。
- `packaging/windows` 和 `packaging/wsl` 分别承载桌面安装包与 WSL 环境材料。

开发工作区的 `docs/` 保存重构计划、Rust 迁移对照、Windows 构建与阶段验证记录以及任务交接。

每个目录的 `DIRECTORY.md` 列出直属文件、子目录及用途，可通过子目录链接逐层查看。[项目目录索引](DIRECTORY.md)

用途说明维护在 `scripts/directory-descriptions.json`。变更后运行 `npm run tree:update` 和 `npm run tree:check`。索引只使用 Node.js 标准库。
