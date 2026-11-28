# stream 目录索引

直播 WebSocket 与消息解码

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
stream/  # 直播 WebSocket 与消息解码
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── notice.cppm  # 平台弹幕、礼物、SC 和进房事件转换
├── packet.cppm  # 长连接包头、鉴权消息和有界解压
└── websocket.cppm  # 长连接建立、消息分片重组与收发期限
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 5c33f325400a4024505d20a73e659329c9b8a44a1bd22fd10d77432bb5c34b0e -->
