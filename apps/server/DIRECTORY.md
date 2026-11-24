# server 目录索引

主服务入口与依赖装配

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
server/  # 主服务入口与依赖装配
├── transport/  # Drogon 的 HTTP 路由、鉴权和 WebSocket 会话
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── bootstrap.cppm  # 装配 Drogon 本机监听、日志与关停
├── configuration.cppm  # 服务端 TOML 配置、字段校验和 Windows 数据路径
├── live.cppm  # 直播连接生命周期、事件准入与 Agent 唤醒模块入口
├── main.cpp  # 服务端进程入口，调用装配模块
├── speech_worker.cppm  # 语音合成与跨端音频传输衔接模块入口
└── xmake.lua  # 链接 Drogon 与 TOML 解析器的 Linux 和 Windows 服务端目标
```

## 子目录索引

- [transport/](transport/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 29ef1f953591107145c07d49c631217e546e9ed39b7d3639ae2ba94d13164c72 -->
