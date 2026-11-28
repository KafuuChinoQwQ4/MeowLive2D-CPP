# profiles 目录索引

模型与人物档案编辑界面

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
profiles/  # 模型与人物档案编辑界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── model_panel.cppm  # 模型档案、列表、连通性测试和推理参数
├── persona_panel.cppm  # 人物卡档案与结构化正文编辑
└── profile_selector.cppm  # Qt 档案选择控件
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 18253c0fb043cb0bfc633cfb44f7418c7cd3ad75bd2cabf0f94b76285a51b50a -->
