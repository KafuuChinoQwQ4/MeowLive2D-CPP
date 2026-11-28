# integration 目录索引

服务、数据库及桌面端之间的联动验证

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
integration/  # 服务、数据库及桌面端之间的联动验证
├── agent/  # Agent 和档案联调
├── avatar/  # VTube Studio 连接联调
├── live/  # 直播会话联调
├── playback/  # 桌面音频和播报联调
├── speech/  # 音色和真实语音组件联调
├── storage/  # 数据库迁移与投影联调
├── support/  # 进程及网络测试辅助
├── transport/  # 服务启动和传输联调
├── windows/  # Windows 安装与受管服务验证
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── xmake.lua  # 本机服务传输层的原生集成测试目标
```

## 子目录索引

- [agent/](agent/DIRECTORY.md)
- [avatar/](avatar/DIRECTORY.md)
- [live/](live/DIRECTORY.md)
- [playback/](playback/DIRECTORY.md)
- [speech/](speech/DIRECTORY.md)
- [storage/](storage/DIRECTORY.md)
- [support/](support/DIRECTORY.md)
- [transport/](transport/DIRECTORY.md)
- [windows/](windows/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: bc91773bb355e2980702256bea8971ce76eee90b6ab026634dd6436bd43787c1 -->
