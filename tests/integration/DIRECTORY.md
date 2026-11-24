# integration 目录索引

服务、数据库及桌面端之间的联动验证

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
integration/  # 服务、数据库及桌面端之间的联动验证
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── server_process.cppm  # 测试服务的临时数据目录、配置和子进程管理
├── stage_2_transport.cpp  # C++ 与 Rust 主服务的原生 HTTP 和 WebSocket 进程联调
├── stage_2_windows.ps1  # 同机 Windows 上的 Qt 和 Rust 执行端配对与重连验证
├── transport_client.cppm  # 回环 HTTP 与 WebSocket 集成测试客户端
└── xmake.lua  # 本机服务传输层的原生集成测试目标
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 6db4e8d91ca5cd32e2febb89186e3eea19cbdac7b0448681b16e6a805e47d1fe -->
