# audio 目录索引

WASAPI 播放、设备时钟和口型同步

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
audio/  # WASAPI 播放、设备时钟和口型同步
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── playback.cppm  # 分片排序、代次校验和播放任务模块入口
└── wasapi.cppm  # WASAPI 输出和设备播放时钟模块入口
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: a93959f21d8c79dccb3d9927ddf3604268e628e077ec06158a2fd8be463b67c6 -->
