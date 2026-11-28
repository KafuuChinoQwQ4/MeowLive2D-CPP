# live 目录索引

直播会话接入和连接设置

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
live/  # 直播会话接入和连接设置
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── live.cppm  # 直播连接生命周期、事件准入与 Agent 唤醒模块入口
└── live_settings.cppm  # 直播凭据配置、脱敏快照和原子保存
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: e9da8bbffe2c3137781187a454312d963440d0e2ed8a7926dea4a594566e2193 -->
