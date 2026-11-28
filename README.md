# MeowLive2D C++ 重构

本项目将同级 Rust 工程迁移为 Windows x64 主服务与 Qt 6.12.1 Widgets 客户端。WSL2 用于开发和交叉构建。Qt 客户端通过协议 v3 配对控制与音频通道，支持试音、文字播报和立即停止。

## 构建入口

工程使用 C++26 和 Xmake 3.1.2，第三方 C++ 包通过 Xmake 的包管理功能接入。WSL 服务端使用 Clang、`clang-scan-deps` 和提供 `std` 模块的标准库构建，并使用系统的 libcurl 开发文件（7.85 或更新版本）。

```sh
xmake f -p linux -m release --toolchain=clang -o build/layout-linux
xmake build meowlive-server
xmake run meowlive-server
```

Windows 构建同时包含 `meowlive-server` 和 `meowlive-desktop`。从 WSL2 生成 Windows x64 程序需要 Linux 宿主 Qt 工具、同版本的 Windows MSVC ABI Qt 库、clang-cl、lld-link、Windows SDK 和 MSVC CRT。将 WSL 工具链中供 Xmake 使用的 `clang-cl` 指向项目包装器。使用 `$WINDOWS_SDK/bin/x64` 布局时，将该目录下的 `clang-cl` 链接到包装器，并把工具目录加入 `PATH`。包装器处理响应文件和依赖记录，调用 `/usr/bin/clang-cl` 或 `MEOWLIVE_REAL_CLANG_CL` 指定的编译器。[clang-cl 包装器](packaging/windows/wsl/clang-cl)

在仓库根目录通过 `xmake-windows` 使用独立的 Windows 配置，继续复用同一个 `build/layout-linux`。将 `WINDOWS_SDK`、`QT_TARGET` 和 `QT_HOST` 设为本机工具链及两套 Qt SDK 的路径。该入口使用 Python 3，只在项目本地加载 Xmake 3.1.2 的模块缓存兼容修正。[WSL 构建入口](packaging/windows/wsl/xmake-windows)

```sh
xmake_windows=./packaging/windows/wsl/xmake-windows

# 首次配置，或工具链和 SDK 路径变更时执行
"$xmake_windows" f -p windows -a x64 -m release -o build/layout-linux \
    '--toolchain=clang-cl[llvm]' --runtimes=MD --sdk="$WINDOWS_SDK" \
    --qt="$QT_TARGET" --qt_host="$QT_HOST"

"$xmake_windows" build meowlive-server meowlive-desktop
```

Linux 继续使用 `xmake`，WSL 下针对 Windows 的构建、测试与 clangd 导出使用 `xmake-windows`。按当前改动挑选目标。保留构建目录里的 `.gens`、`.objs` 和 `.deps`，便于下次增量构建。

Xmake 3.1.2 对命名模块中 `#include` 的头文件可能漏掉增量更新。单独修改这类头文件后，用 `xmake build -r <目标>` 重建并验证对应目标，WSL 下的 Windows 目标用 `xmake-windows build -r <目标>`。

在 Windows 目标 Qt SDK 安装同版本的 Qt WebSockets 模块，并在运行桌面端时把 `Qt6WebSockets.dll` 放在 EXE 同级。

两端通过命名模块导入共享协议。跨平台代码导入 `meowlive.standard`。Linux 由该模块统一导出 `std`，Windows 交叉工具链在全局模块片段使用集中维护的 MSVC STL 头文件。[标准库模块](libs/platform/standard.cppm)

### VS Code 代码检查

在 WSL 的项目窗口使用 clangd 扩展，clangd 与构建所用的 Clang 保持同版本。现有 Windows x64 目标在 WSL 中可提供服务端、Qt 桌面端及测试的编译参数。Linux 服务端可使用以下构建和导出命令，Windows 目标使用 `xmake-windows` 执行对应命令。

```sh
xmake build meowlive-server
xmake clangd
```

`xmake clangd` 将当前配置的编译参数导出到项目根目录的 `compile_commands.json`。修改构建目标、切换平台或清理后，重新构建并导出，以便 clangd 找到最新的模块产物。项目窗口由 clangd 提供 C++ 诊断。

## 测试

从项目根目录运行有界工作队列单元测试、协议契约测试和进程联调。联调会启动本机 C++ 服务，验证配置加载、HTTP、鉴权及双 WebSocket。

```sh
xmake test 'meowlive-unit-tests/work_queue' 'meowlive-protocol-contracts/protocol_v3' 'meowlive-transport-integration/stage_2_transport' 'meowlive-playback-integration/stage_3_playback'
```

可将同一联调用于旧版 Rust 主服务。

```sh
xmake build meowlive-transport-integration
xmake run meowlive-transport-integration --rust ../MeowLive2D/target/debug/meowlive-server
```

阶段 1.5 播报基线使用 Windows 原生测量程序。先用 Windows 目标配置构建，再在有默认播放设备的 Windows 环境运行，将旧版 Rust 服务和执行端的 EXE 路径作为参数传入。程序默认测量五轮，将延迟、工作集和 CPU 时间输出为 JSON。

```sh
./packaging/windows/wsl/xmake-windows build meowlive-playback-baseline
```

```powershell
.\meowlive-playback-baseline.exe .\meowlive-server.exe .\meowlive-client.exe
```

阶段 3 的进程联调使用固定 WAV 验证播报、停止、回执和重连。Windows 上可同时启动实际 Qt 执行端，用 WASAPI 回环采集验证试音与停止静音。

```powershell
.\meowlive-playback-integration.exe .\meowlive-server.exe .\meowlive-desktop.exe
```

## 本机连接

服务默认监听 `127.0.0.1:19601`，从 Windows 的 `%LOCALAPPDATA%\MeowLive2D\server.toml` 读取配置。可用 `--data-dir`、`MEOWLIVE_DATA_DIR` 指定数据目录，用 `--config` 指定 TOML。日志和上传暂存目录位于数据目录内。桌面端可通过 `MEOWLIVE_SERVER_URL` 指定服务地址。开启鉴权后，桌面端使用设备凭据建立双通道，管理员在窗口中单独登录。[服务配置样例](config/server.example.toml)

服务在 TOML 所在目录的 `local/` 下按配置文件名加载 Agent、LLM 和直播设置。启用鉴权时，服务启动前要求管理员和设备凭据均有效且相互独立。每种凭据优先读取其环境变量，变量缺失时读取 TOML 指定的文件。

服务异常记录在数据目录的 `logs/meowlive-server.exceptions.log`，包含操作、异常类型及捕获位置。由 Windows 桌面启动时，子进程的启动错误也会写入 `logs/meowlive-server.startup.log`，桌面异常写入 `logs/meowlive-desktop.exceptions.log`。HTTP 的 `out_of_memory` 和 `internal_error` 对应两类内部故障，确认的依赖暂时不可用或工作队列繁忙会使用各自的状态和错误码。

普通管理操作、模型探测和训练版本试听各自使用有界队列。`server.queue_capacity` 分别限制各队列中运行和等待的请求总数。播放完成后，待结算事件会重试落盘。主结算目录无法写入时，正常关机会尝试写入同级的 `reply-recovery`，下次启动继续结算。

## 手动播报

服务与 Qt 执行端连通后可直接点击「试音」。文字播报需要启动 Windows GPT-SoVITS `api_v2.py`，在 `[speech]` 中指定 `base_url`、GPT-SoVITS 可访问的本机 `reference_audio` 与对应的 `prompt_text`。Qt 的「入队」使用当前音色。已配置的默认音色或旧资源库的已选音色可以使用。管理员可以通过 `POST /api/speech`、`POST /api/speech/preview` 和 `POST /api/stop` 执行相同操作。[服务配置样例](config/server.example.toml) · [GPT-SoVITS API v2](https://github.com/RVC-Boss/GPT-SoVITS/blob/main/api_v2.py)

## Agent

在 Qt 的「LLM 接入」保存模型档案并重启主服务，在「人物卡」选择档案，连接执行端并配置可用音色后从 Agent 页面恢复运行。模拟事件可通过 `POST /api/events` 提交。Agent 页面提供暂停和互动设置，「Agent 运行」与观察页面提供工具设置和调度、轨迹及用量查询。[服务配置样例](config/server.example.toml)

```sh
xmake test 'meowlive-agent-session-tests/agent_session' 'meowlive-llm-provider-contracts/llm_providers' 'meowlive-agent-integration/stage_4_agent' 'meowlive-profile-integration/stage_4_profiles'
```

## B 站直播、资源与 Windows 部署

在 Qt 的「直播连接」保存开放平台应用 ID 和三项凭据，手动连接房间，再从 Agent 页面恢复互动。服务端以 UTC 事件时间计算剩余有效期，并用单调时钟调度。直播掉线后 Agent 会暂停，等连接恢复并由操作者恢复互动。[直播配置](config/server.example.toml)

Qt 的音色和角色页管理资源。VTube Studio 页处理授权和实际口型，OBS 页提供场景、录制和推流控制。Windows 转写和训练以本机语音组件为前提。观众库默认关闭，启用后使用 PostgreSQL 和 pgvector。[服务配置样例](config/server.example.toml)

启用观众数据后，Qt 提供档案、事件、积分、记忆和关系管理。后台记忆提取在 `[memory]` 配置独立的 OpenAI 兼容接口和模型。可选的嵌入接口负责生成 pgvector 索引，`[viewers].agent_memory` 控制 Agent 工具是否读取当前事件观众的有效记忆。`[graph]` 配置可重建的 Neo4j 关系投影。[服务配置样例](config/server.example.toml)

Windows 安装版的「可选模型」把权重、运行环境和下载缓存分别存到安装目录下的 `voice/models/`、`voice/runtimes/` 和 `voice/.staging/`。选择模型后可查看待下载文件与所需磁盘空间，下载中可取消，失败后可重试。安装使用 Windows x64 CPython 3.11，支持系统安装及安装目录中 `voice/python311/` 下的便携版。[版本和文件清单](packaging/windows/voice/voice-models.json)

录音转写提供 faster-whisper small 和 Paraformer-zh，两者均使用 CPU。安装后在「参考录音」或「声音训练」选择已就绪的转写模型，识别文字可在提交前校对。

在「可选模型」将用途切换为「训练底模」，可按需安装 GPT-SoVITS v2ProPlus 或 v4。两者共用 Windows CUDA 运行环境和公共权重，训练需要兼容 CUDA 12.6 的 NVIDIA GPU 与驱动。安装就绪后，到「声音训练」选择底模，关联音色并添加校对过的训练片段。作业会保存所选底模和修订。已有 GPT-SoVITS v2 环境仍可用于训练。

已有 Whisper 环境仍可从「已有 Whisper 环境」选择。离线部署可在有网络的机器准备组件，再用 Windows x64 CPython 3.11 安装。以下命令在安装目录运行，组件默认落到安装目录的 `voice/`。用 `MEOWLIVE_WHISPER_PYTHON` 和 `MEOWLIVE_WHISPER_MODEL` 可指定现有环境的绝对路径。[Whisper 组件安装脚本](packaging/windows/voice/install-whisper.py)

```powershell
py -3.11 .\voice\install-whisper.py download D:\whisper-bundle
py -3.11 .\voice\install-whisper.py install D:\whisper-bundle
```

桌面端默认检查并启动同目录的 Windows 主服务。设置 `MEOWLIVE_SERVER_URL` 可显式连接外部服务。WSL2 的 NSIS 打包命令接收同版本的 Windows Release 构建、Qt 目标目录及 Visual C++ 应用本地运行库目录。

```sh
packaging/windows/build-installer.sh 0.1.4 WIN_BUILD_DIR QT_DIR VC_REDIST_DLL_DIR OUTPUT_DIR
```

从 Rust 版导入时，在 Windows Python 3.11 及以上版本或 WSL2 中运行 `tools/import-rust-data.py`，给出旧版的本地快照和独立的数据目录。用 `--help` 查询资源、训练、OBS 和数据库备份的参数。先传 `--data-dir` 启动 C++ 主服务检查副本，再用 Qt 访问对应回环服务。PostgreSQL 备份可恢复到独立库，启用观众存储后由 C++ 校验并继续迁移。[旧数据导入工具](packaging/windows/import-rust-data.py)

## C++ 格式化

使用 `xmake format` 按 `.clang-format` 格式化 C++ 源码，或在编辑器中格式化改动的文件。格式化命令需要在 `PATH` 中提供 `clang-format`。构建和测试可直接运行。

## 模块边界

- `apps/server` 装配 Agent、应用用例和外部适配器，Drogon 1.9.14 的路由放在 `transport`。
- `libs/application` 依赖 `libs/domain` 并定义外部接口，`adapters` 实现这些接口。`libs/agent` 放主服务调用的决策和工具轮次逻辑。
- `libs/protocol` 定义两端共享的控制和音频协议。`apps/desktop` 通过 `services` 访问主服务，WASAPI、VTube Studio 和 OBS 集成放在 `libs/desktop-runtime`。
- `packaging/windows` 和 `packaging/wsl` 分别承载桌面安装包与 WSL 环境材料。

运行 `npm run arch:check` 检查生产 C++ 命名模块的显式导入方向、依赖环与缺失声明。检查范围包含各平台的源码及模块实现单元。运行 `npm run test:tooling` 验证依赖检查和目录索引工具。

开发工作区的本地 `docs/README.md` 按类别与日期索引重构计划、Rust 迁移对照、Windows 构建与阶段验证记录，以及每日任务交接。

每个目录的 `DIRECTORY.md` 列出直属文件、子目录及用途，可通过子目录链接逐层查看。[项目目录索引](DIRECTORY.md)

用途说明维护在 `scripts/directory-descriptions.json`。变更后运行 `npm run tree:update` 和 `npm run tree:check`。索引只使用 Node.js 标准库。
