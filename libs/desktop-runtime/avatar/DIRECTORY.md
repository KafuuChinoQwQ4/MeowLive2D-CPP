# avatar 目录索引

角色控制与 VTube Studio 适配

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
avatar/  # 角色控制与 VTube Studio 适配
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── authentication.cpp  # VTube Studio 授权和参数注册
├── messages.cpp  # VTube Studio 消息处理与口型发送
├── presentation.cppm  # 角色展示、VTube Studio 与播放口型模块入口
├── protocol.cppm  # VTube Studio 协议编解码、地址和口型参数
├── resources.cppm  # VTube Studio 资源请求与响应转换
└── token.cppm  # VTube Studio 授权令牌的本地校验与保存
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 7073d72e8f428a19f0ca8022aff1cfb202cda1ea62ff846ac4272c6e1de59363 -->
