# services 目录索引

界面访问主服务及管理本机服务进程的接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
services/  # 界面访问主服务及管理本机服务进程的接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── managed_server.cppm  # 本机 Windows 主服务受管启动与退出
├── server_client.cppm  # 控制台 HTTP、管理会话和播报操作
└── update_readiness.cppm  # 桌面更新前核对主服务、直播、训练与播报状态
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: b9c078dc6a0146b9a6e325f9c106c1896dafed2873a70d57397ca300b8e5f6ab -->
