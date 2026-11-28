# storage 目录索引

本机资源、PostgreSQL、pgvector 和图数据库的存储适配

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
storage/  # 本机资源、PostgreSQL、pgvector 和图数据库的存储适配
├── files/  # 本地文件写入及音色目录持久化
├── memory/  # 记忆编辑、提取和向量存储
├── migrations/  # 旧版 SQLx 兼容的 PostgreSQL 迁移脚本
├── postgres/  # PostgreSQL 驱动与迁移执行
├── relationships/  # 关系数据、查询和图数据库投影
├── replies/  # 回复结算与可重试写入
├── viewers/  # 观众事件、查询、积分与档案合并
└── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
```

## 子目录索引

- [files/](files/DIRECTORY.md)
- [memory/](memory/DIRECTORY.md)
- [migrations/](migrations/DIRECTORY.md)
- [postgres/](postgres/DIRECTORY.md)
- [relationships/](relationships/DIRECTORY.md)
- [replies/](replies/DIRECTORY.md)
- [viewers/](viewers/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 6b69c1f98ae054703aba2a67c77282ab0bfba99115e11014e9dd0f45d81eccf8 -->
