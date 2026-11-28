# audio 目录索引

WASAPI 播放、设备时钟和口型同步

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
audio/  # WASAPI 播放、设备时钟和口型同步
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── playback.cppm  # 音频提前到达缓冲、代次校验和执行回执
└── wasapi.cppm  # 共享模式 WASAPI 输出和设备播放时钟
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: c16c4b7a5acd23be5ac0054e7a272c81a3d63d3b5593de80680e7ae8b1cc320b -->
