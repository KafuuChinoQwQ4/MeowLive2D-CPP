# services 目录索引

界面访问主服务及管理本机服务进程的接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
services/  # 界面访问主服务及管理本机服务进程的接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── managed_server.cppm  # 本机服务进程管理和健康检查模块入口
└── server_client.cppm  # 控制台 HTTP 请求和管理会话模块入口
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 2a34a25f5d79b0f9b41e15ba4f55629a4a1fa42e4776bba4d057312758efab00 -->
