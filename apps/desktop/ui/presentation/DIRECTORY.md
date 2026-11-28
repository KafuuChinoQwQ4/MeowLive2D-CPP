# presentation 目录索引

虚拟角色及形象控制界面

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
presentation/  # 虚拟角色及形象控制界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── avatar_panel.cppm  # VTube Studio 授权、模型切换和快捷键预览
└── character_panel.cppm  # 角色档案、资源选择与动作预览
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 6b87a3069b6a576c67f08c7fb41d6ce1cf3dd32d48acde767de7fa37e32eda3d -->
