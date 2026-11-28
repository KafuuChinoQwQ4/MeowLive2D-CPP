# live 目录索引

直播连接和 OBS 控制界面

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
live/  # 直播连接和 OBS 控制界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── live_panel.cppm  # 直播连接状态、事件统计与连接控制
├── live_settings_form.cppm  # 直播应用设置与脱敏凭据编辑
└── obs_panel.cppm  # 本机 OBS 场景、录制和推流控制
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: bd6ef7b248f00daeb7f5ae06f9ccb84a6ddbe9f1625e726fe457279686d6fd17 -->
