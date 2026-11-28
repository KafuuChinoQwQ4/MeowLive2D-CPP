# decision 目录索引

决策规则、互动与人物提示词

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
decision/  # 决策规则、互动与人物提示词
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── interaction.cppm  # 观众公平、朗读策略与欢迎冷却
├── persona.cppm  # Agent 人物卡档案类型
├── runtime.cppm  # 可取消模型调用、重试与工具续轮
└── unicode_alphanumeric.inc  # 用于追问匹配的 Unicode 字符属性范围
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 152b4d5643b15d59a3c905ef09f9f04019cb47da4eacaaef4e61b363803dcd40 -->
