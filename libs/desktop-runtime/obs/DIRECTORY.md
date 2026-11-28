# obs 目录索引

OBS 的 WebSocket 控制

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
obs/  # OBS 的 WebSocket 控制
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── client.cppm  # OBS WebSocket v5 鉴权、场景和输出操作
└── settings.cppm  # 本机 OBS 连接设置与私有凭据保存
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 53f4f6c5b98a116758073ce335cfa5d0c0dd20ad64ff0eae08d2cf66b6ee4343 -->
