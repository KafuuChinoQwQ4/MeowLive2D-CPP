# transport 目录索引

Drogon 的 HTTP 路由、鉴权和 WebSocket 会话

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
transport/  # Drogon 的 HTTP 路由、鉴权和 WebSocket 会话
├── agent/  # Agent、人物和模型接口
├── http/  # HTTP 注册、请求校验及鉴权
├── live/  # 直播会话接口
├── speech/  # 播报、音色和角色接口
├── training/  # 转写和训练任务接口
├── viewers/  # 观众、记忆和关系管理接口
├── websocket/  # 控制和音频通道配对、传输和连接管理
└── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
```

## 子目录索引

- [agent/](agent/DIRECTORY.md)
- [http/](http/DIRECTORY.md)
- [live/](live/DIRECTORY.md)
- [speech/](speech/DIRECTORY.md)
- [training/](training/DIRECTORY.md)
- [viewers/](viewers/DIRECTORY.md)
- [websocket/](websocket/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 58d0a78ec15fcd87f4278682b0f5f5f1d2e383068b729e446fe573e75766988f -->
