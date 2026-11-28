# memory 目录索引

观众记忆提取与查询

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
memory/  # 观众记忆提取与查询
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── memory_extraction.cppm  # 结构化观众记忆提取与证据来源校验
└── memory_lookup.cppm  # Agent 读取当前观众记忆的应用层接口
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: b4853b33b19b6064f6f460c93002bd41e8e594d65c2ed2f64cff1b247aee2ddf -->
