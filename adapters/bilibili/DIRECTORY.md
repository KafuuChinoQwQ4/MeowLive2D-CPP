# bilibili 目录索引

B 站直播开放平台认证、连接和直播事件转换

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
bilibili/  # B 站直播开放平台认证、连接和直播事件转换
├── open_platform/  # 开放平台项目管理和请求签名
├── stream/  # 直播 WebSocket 与消息解码
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── live_source.cppm  # 开放平台会话、心跳和直播事件转换模块入口
└── transport.cppm  # 可取消的 libcurl 传输和地址校验
```

## 子目录索引

- [open_platform/](open_platform/DIRECTORY.md)
- [stream/](stream/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 45b122437fb8261f3c551391d554ba8f071fc13b1476e3a6f981550a301cc950 -->
