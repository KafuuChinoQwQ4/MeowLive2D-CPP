# fixtures 目录索引

协议契约与集成测试共用的固定样本

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
fixtures/  # 协议契约与集成测试共用的固定样本
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── protocol-v3.json  # Rust 捕获的 HTTP、双 WebSocket 和 MLPC 协议 v3 样本
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: ca4b8a153af71977903369883cb03d7e4cf85c4008287ca4b07da640f9a7f37c -->
