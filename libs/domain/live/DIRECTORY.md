# live 目录索引

直播事件及响应时效

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
live/  # 直播事件及响应时效
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── live_event.cppm  # 平台无关的直播事件、观众身份和礼物元数据类型
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: d6aa7100c64f95f560f3b8d68af284c661293559023effb3383619f9d7e80e98 -->
