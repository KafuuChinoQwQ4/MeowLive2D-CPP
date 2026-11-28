# storage 目录索引

数据库迁移与投影联调

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
storage/  # 数据库迁移与投影联调
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── postgres_migrations.cpp  # PostgreSQL 空库和旧版 SQLx 历史验证
└── relationship_projection.cpp  # PostgreSQL 与 Neo4j 的关系派生、恢复及管理回归
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 4b3d4a95758cbbda6a3578aa5b07487efd0ee04398b8e6d1598b577691d9ef99 -->
