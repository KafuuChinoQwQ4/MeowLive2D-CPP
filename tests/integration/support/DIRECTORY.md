# support 目录索引

进程及网络测试辅助

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
support/  # 进程及网络测试辅助
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── loopback_socket.cppm  # 集成测试的本机回环套接字
├── server_process.cppm  # 测试服务的临时数据目录、配置和子进程管理
└── transport_client.cppm  # 回环 HTTP 与 WebSocket 集成测试客户端
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 27cdc7f634ba115daa267ba42bda1fa1298c21ce9465401b7a6055da9e1501a1 -->
