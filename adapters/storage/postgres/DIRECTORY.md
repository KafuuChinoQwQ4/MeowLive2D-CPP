# postgres 目录索引

PostgreSQL 驱动与迁移执行

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
postgres/  # PostgreSQL 驱动与迁移执行
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── postgres.cppm  # 按需加载 libpq 与参数化 SQL 执行
└── postgres_migrations.cppm  # SQLx 历史、SHA-384 校验及事务迁移
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: dca50727c80a8d6f1fbe32fb9c4980c23157c63ba3cee138d09ceb221d23dba9 -->
