# live 目录索引

直播用例及会话生命周期

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
live/  # 直播用例及会话生命周期
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── live.cppm  # 直播源接口、私有连接参数和失败类型
└── live_session.cppm  # 直播会话生命周期、事件时效和有界准入队列
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 3398e2b5fde454cb70a57c1ea8f15a7417709662f50d655fa17038d2a93c1463 -->
