# domain 目录索引

直播事件、人物卡和发言等领域类型

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
domain/  # 直播事件、人物卡和发言等领域类型
├── live/  # 直播事件及响应时效
├── speech/  # 播报文本与角色资源规则
├── viewers/  # 观众亲密度与记忆规则
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── xmake.lua  # 领域命名模块静态库目标
```

## 子目录索引

- [live/](live/DIRECTORY.md)
- [speech/](speech/DIRECTORY.md)
- [viewers/](viewers/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 25fc9e0defa28d29b15bdcc5c003868b6c2d32178cb14b752479533f793406b7 -->
