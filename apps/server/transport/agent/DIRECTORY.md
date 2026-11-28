# agent 目录索引

Agent、人物和模型接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
agent/  # Agent、人物和模型接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── agent_http.cppm  # 事件、Agent 控制与运行观测接口
├── model_http.cppm  # 模型发现、连接测试和推理档位预览接口
└── profile_http.cppm  # 模型与人物卡档案 HTTP 路由
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 8e52d00ed9f2f0848dc9a21b5298e0f4bc141c7a55a8e479ee302e0fa39cef91 -->
