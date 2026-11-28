# speech 目录索引

音色、角色管理及播放回执

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
speech/  # 音色、角色管理及播放回执
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── character_profiles.cppm  # 角色档案的编辑、模型确认与动作验证
├── receipt_journal.cppm  # 已确认执行终态的异步本地回执日志
└── speech_voices.cppm  # 音色管理和播报音色更新
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 133a68038b219300609016769a81c9ad4b8bfd0d0ea29f8cb2a57ca32df18f7d -->
