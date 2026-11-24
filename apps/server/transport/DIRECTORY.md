# transport 目录索引

Drogon 的 HTTP 路由、鉴权和 WebSocket 会话

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
transport/  # Drogon 的 HTTP 路由、鉴权和 WebSocket 会话
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── access.cppm  # 管理员会话、设备鉴权和来源校验
├── http.cppm  # 健康、服务状态和管理员会话 HTTP 路由
└── websocket.cppm  # 控制与音频 WebSocket 的连接占用、配对、心跳及断线清理
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 258e4eb536e8109ebc5de4f41cf7d668da1931533403f0caac4ec1a8ebcbb218 -->
