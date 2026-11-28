# migrations 目录索引

旧版 SQLx 兼容的 PostgreSQL 迁移脚本

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
migrations/  # 旧版 SQLx 兼容的 PostgreSQL 迁移脚本
├── .gitattributes  # 固定 SQL 脚本的换行与字节校验
├── 0001_viewer_identity_events.sql  # 观众身份、别名、会话及事件表
├── 0002_companionship.sql  # 观看、礼物、亲密度及回复结算表
├── 0003_memories.sql  # 记忆、证据、向量及处理任务表
├── 0004_relationships.sql  # 关系事实、审计和发件箱表
├── 0005_memory_cleanup.sql  # 记忆清理迁移
├── 0006_memory_operations.sql  # 记忆操作请求记录
├── 0007_viewer_merge.sql  # 观众合并与审计结构
├── 0008_memory_graph_invalidation.sql  # 记忆图失效通知
├── 0009_superchat_room_enter_events.sql  # SC 和进房事件类型约束
└── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 27a7988f0fbae5b78f90aa6596b12510a17a19a78d4f41df9aaba8254570e82c -->
