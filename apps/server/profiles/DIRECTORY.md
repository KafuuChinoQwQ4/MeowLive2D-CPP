# profiles 目录索引

模型和人物档案编辑与持久化

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
profiles/  # 模型和人物档案编辑与持久化
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── edit_validation.cpp  # 档案字段校验和共享编辑操作
├── model_edits.cpp  # 模型档案编辑与凭据处理
├── persona_edits.cpp  # 人物档案编辑规则
├── profile_defaults.cppm  # Agent 与模型的 TOML 初始配置
├── profile_edits.cppm  # 人物卡与模型档案编辑接口及共享字段规则
├── profile_file.cppm  # 本机档案和观测文件的原子替换
└── profile_settings.cppm  # 串行持久化 Agent 和模型档案
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 8344cf1421c15d76df7c7ee271c1700731a6f4e2fe625210d5762ae470731aa9 -->
