# speech 目录索引

播报文本与角色资源规则

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
speech/  # 播报文本与角色资源规则
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── character.cppm  # 角色标识、角色名称与口型参数校验
└── speech.cppm  # 播报生命周期、文本和音色标识校验
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 6750a4fc8f5ea9af3f08fbe9c2e55cd9fda588ef919026b1b7c9035bf0b001f2 -->
