# integration 目录索引

服务、数据库及桌面端之间的联动验证

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
integration/  # 服务、数据库及桌面端之间的联动验证
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── stage_2_transport.py  # C++ 与 Rust 主服务的 HTTP 和 WebSocket 进程联调
└── stage_2_windows.ps1  # 同机 Windows 上的 Qt 和 Rust 执行端配对与重连验证
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 53a63ade54feb608236a66116fd10b20d30fd847a7f7326d2aed5d2755b03feb -->
