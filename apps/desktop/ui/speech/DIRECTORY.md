# speech 目录索引

播报及音色管理界面

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
speech/  # 播报及音色管理界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── speech_panel.cppm  # 试音、文字入队、停止与播报任务状态
└── voice_panel.cppm  # 音色导入、选择、试听和删除
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 6de265a19cdba0067dc252ec028471b88c16eb7d643047c74de7cb89408b0d0a -->
