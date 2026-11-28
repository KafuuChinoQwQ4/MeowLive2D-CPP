# viewers 目录索引

观众管理、记忆与关系投影后台任务

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
viewers/  # 观众管理、记忆与关系投影后台任务
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── graph_worker.cppm  # 关系投影后台任务和状态
├── memory_worker.cppm  # 记忆提取和向量索引后台任务
└── viewer_admin.cppm  # 观众管理查询与未确认事件统计
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 7288b4a5daea9b704aad34f921a76759b5d14b065b878ad40d1d8577f2be4c89 -->
