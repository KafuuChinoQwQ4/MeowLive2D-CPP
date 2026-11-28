# playback 目录索引

桌面音频和播报联调

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
playback/  # 桌面音频和播报联调
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── fixture.cppm  # 播报进程与模拟执行端测试环境
├── stage_3_desktop.cpp  # Windows Qt 执行端的分片、代次与重连联调
└── stage_3_playback.cpp  # 播报、停止、回执日志与 Windows 音频回环联调
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 1e2197f882abe71a1aaf076854900ffa9137a53b4285ef79c76cc7f51948e706 -->
