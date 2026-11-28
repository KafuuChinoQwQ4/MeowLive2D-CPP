# ui 目录索引

控制台窗口、页面和 Qt Designer 表单

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
ui/  # 控制台窗口、页面和 Qt Designer 表单
├── agent/  # Agent 运行控制和观测界面
├── live/  # 直播连接和 OBS 控制界面
├── presentation/  # 虚拟角色及形象控制界面
├── profiles/  # 模型与人物档案编辑界面
├── speech/  # 播报及音色管理界面
├── system/  # 运行日志和版本更新界面
├── training/  # 语音训练、转写和任务界面
├── viewers/  # 观众档案、积分、记忆和关系界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── main_window.cppm  # Qt 控制台健康、执行端与管理员会话界面
```

## 子目录索引

- [agent/](agent/DIRECTORY.md)
- [live/](live/DIRECTORY.md)
- [presentation/](presentation/DIRECTORY.md)
- [profiles/](profiles/DIRECTORY.md)
- [speech/](speech/DIRECTORY.md)
- [system/](system/DIRECTORY.md)
- [training/](training/DIRECTORY.md)
- [viewers/](viewers/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 88295e0cbafb38e4594bf7f60bc9d94c77a802a8c59ed84a32e518514ea452a9 -->
