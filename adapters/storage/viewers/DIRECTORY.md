# viewers 目录索引

观众事件、查询、积分与档案合并

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
viewers/  # 观众事件、查询、积分与档案合并
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── repositories.cppm  # 观众事件、记忆、关系和回执的存储模块入口
├── viewer_affinity.cppm  # 观众亲密度详情、人工调整和撤销事务
├── viewer_merge.cppm  # 身份合并预览、版本校验和事务审计
├── viewer_merge_data.cppm  # 合并观众事件、积分、记忆与关系的数据更新
└── viewer_queries.cppm  # 观众身份、别名和直播事件的分作用域查询
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 78d7dc81344fff33af6a2efbbe6a0eda1305b11a1b10662e7ea2c8009810bb47 -->
