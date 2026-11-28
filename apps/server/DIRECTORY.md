# server 目录索引

主服务入口与依赖装配

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
server/  # 主服务入口与依赖装配
├── agent/  # Agent 运行协调、模型调用和观测
├── configuration/  # 配置读取、校验和本地设置加载
├── live/  # 直播会话接入和连接设置
├── profiles/  # 模型和人物档案编辑与持久化
├── speech/  # 音色、角色管理及播放回执
├── startup/  # 服务启动装配、宿主生命周期与关闭
├── training/  # 语音训练任务与转写服务
├── transport/  # Drogon 的 HTTP 路由、鉴权和 WebSocket 会话
├── viewers/  # 观众管理、记忆与关系投影后台任务
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── main.cpp  # 服务端进程入口，调用装配模块
└── xmake.lua  # 链接 Drogon 与 TOML 解析器的 Linux 和 Windows 服务端目标
```

## 子目录索引

- [agent/](agent/DIRECTORY.md)
- [configuration/](configuration/DIRECTORY.md)
- [live/](live/DIRECTORY.md)
- [profiles/](profiles/DIRECTORY.md)
- [speech/](speech/DIRECTORY.md)
- [startup/](startup/DIRECTORY.md)
- [training/](training/DIRECTORY.md)
- [transport/](transport/DIRECTORY.md)
- [viewers/](viewers/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 45a462711f2d33d0a32a75eaf4bc8d5783ebd66a1943145f3deba4cc564b27fd -->
