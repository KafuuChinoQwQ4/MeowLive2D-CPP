# viewers 目录索引

观众档案、积分、记忆和关系界面

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
viewers/  # 观众档案、积分、记忆和关系界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── memory_panel.cppm  # 观众记忆、事件依据和任务维护
├── relationship_panel.cppm  # 观众关系查询、关系图状态与重建
├── viewer_affinity.cpp  # 桌面端观众积分管理
├── viewer_detail.cpp  # 桌面端观众分页及详情
├── viewer_merge.cpp  # 桌面端观众档案合并
└── viewer_panel.cppm  # 观众档案、事件、积分和合并确认
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 3ef73698c12b1b2179a9d8b1ef3b5d5055bcde8e0c9508ef5a53237330f4342e -->
