# websocket 目录索引

控制和音频通道配对、传输和连接管理

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
websocket/  # 控制和音频通道配对、传输和连接管理
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── audio_transfer.cppm  # WebSocket 端点与音频分帧传输
├── bridge_session.cppm  # 控制与音频通道配对、会话超时、回执和资源查询
└── websocket.cppm  # 注册双通道 WebSocket 控制器，校验握手并分派客户端消息
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: bec9f113fc27368ed9ab56f298e5d99144a24476a1a616930ef49eb732ceea15 -->
