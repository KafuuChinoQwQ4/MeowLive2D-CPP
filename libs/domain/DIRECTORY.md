# domain 目录索引

直播事件、人物卡和发言等领域类型

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
domain/  # 直播事件、人物卡和发言等领域类型
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── live_event.cppm  # 平台无关的直播事件、观众身份和礼物元数据类型
├── speech.cppm  # 播报任务生命周期状态类型
└── xmake.lua  # 领域命名模块静态库目标
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 22598a7c88168f812b042f4e5257604f16e4585e1d4f451c63ee3586f662e168 -->
