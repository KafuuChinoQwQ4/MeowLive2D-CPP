# viewers 目录索引

观众亲密度与记忆规则

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
viewers/  # 观众亲密度与记忆规则
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── affinity.cppm  # 互动积分计算与弹幕去重规则
└── memory.cppm  # 观众记忆可信陈述、升格与过期规则
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 0ac2dc99bc89972275a61da51c176c8daf7d6cde7db4fcaab2498b038cda558b -->
