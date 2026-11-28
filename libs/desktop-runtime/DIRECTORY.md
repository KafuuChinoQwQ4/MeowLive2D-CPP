# desktop-runtime 目录索引

Windows 的设备与直播软件执行能力

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
desktop-runtime/  # Windows 的设备与直播软件执行能力
├── audio/  # WASAPI 播放、设备时钟和口型同步
├── avatar/  # 角色控制与 VTube Studio 适配
├── obs/  # OBS 的 WebSocket 控制
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── connection.cppm  # Qt 执行端双通道连接、消息校验及重连
└── xmake.lua  # Windows 执行端命名模块静态库目标
```

## 子目录索引

- [audio/](audio/DIRECTORY.md)
- [avatar/](avatar/DIRECTORY.md)
- [obs/](obs/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: f4edad03990a73cd57a76f54089c3e58ebcf81c1a0d666e761bc690537fcabe7 -->
