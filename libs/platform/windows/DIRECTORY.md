# windows 目录索引

Windows 内核资源所有权

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
windows/  # Windows 内核资源所有权
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── handle.cppm  # 管理 Win32 内核句柄的独占所有权和关闭
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: d0bf00b1649447708b656d0ff8501a301996adef858e65c593397f9f9f7c4626 -->
