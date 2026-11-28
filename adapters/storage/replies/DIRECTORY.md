# replies 目录索引

回复结算与可重试写入

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
replies/  # 回复结算与可重试写入
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── reply_ledger.cppm  # 确认播毕后的 PostgreSQL 事务与幂等积分结算
└── reply_outbox.cppm  # 播毕结算的本地持久缓冲及重试
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 10b91d1cd3629b19ea372b0712fab97419d1a5332d7f11051cedcf7883c65df0 -->
