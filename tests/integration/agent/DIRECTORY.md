# agent 目录索引

Agent 和档案联调

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
agent/  # Agent 和档案联调
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── stage_4_agent.cpp  # 模型续轮、Agent 播报、历史与取消进程联调
└── stage_4_profiles.cpp  # 档案迁移、并发编辑、故障与重启恢复测试
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 83c5ac9af7be0dbd68e163d68ad8e39ae1bf3822926fa2bd6fab76fb6de3fa48 -->
