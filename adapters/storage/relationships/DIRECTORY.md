# relationships 目录索引

关系数据、查询和图数据库投影

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
relationships/  # 关系数据、查询和图数据库投影
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── neo4j_projection.cppm  # Neo4j 关系投影和查询服务
├── relationship_admin.cppm  # 关系创建、确认、撤销和删除的版本与审计事务
├── relationship_derivation.cppm  # 从已证实的观众记忆派生关系事实
├── relationship_outbox.cppm  # PostgreSQL 关系投递、失败恢复和重建
└── relationship_queries.cppm  # 观众关系事实与图查询版本验证
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 08856690dfc268f0627888c1d1fc50af6ca3466639b393f5626954704155b9c4 -->
