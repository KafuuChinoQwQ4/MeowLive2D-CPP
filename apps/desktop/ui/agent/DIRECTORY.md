# agent 目录索引

Agent 运行控制和观测界面

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
agent/  # Agent 运行控制和观测界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── agent_observability_panel.cppm  # 活动、调度、轨迹与模型用量查询
├── agent_panel.cppm  # Agent 状态、暂停和互动设置
└── agent_runtime_panel.cppm  # 流式、工具、搜索及运行参数设置
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 3bcd7091a47c89578c649ce365d1861e5744d2d1722677a71c99351b5c5540cf -->
