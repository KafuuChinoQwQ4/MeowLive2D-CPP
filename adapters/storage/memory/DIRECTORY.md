# memory 目录索引

记忆编辑、提取和向量存储

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
memory/  # 记忆编辑、提取和向量存储
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── memory_admin.cppm  # 支持版本校验和审计的观众记忆管理
├── memory_jobs.cppm  # PostgreSQL 记忆提取租约、失败重试和过期清理
├── memory_records.cppm  # 记忆提取结果、事件依据和数据版本事务
└── memory_vectors.cppm  # 向量作业租约和带版本校验的 pgvector 写入
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: f313302f3e34cc4090217d805b7e74a89bc6616241b4a9f9d50ecc20219a6b0b -->
