# transport 目录索引

服务启动和传输联调

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
transport/  # 服务启动和传输联调
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── stage_2_startup.cppm  # 版本化设置、启动校验与错误脱敏的进程测试
└── stage_2_transport.cpp  # C++ 与 Rust 主服务的原生 HTTP 和 WebSocket 进程联调
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 52f816646c3609bb5c4a99c48e9dc95dcb35c94c6bb147c34398877349cc93f8 -->
