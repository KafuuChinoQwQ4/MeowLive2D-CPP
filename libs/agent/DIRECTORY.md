# agent 目录索引

链接进主服务的 Agent 决策、工具调用与取消逻辑

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
agent/  # 链接进主服务的 Agent 决策、工具调用与取消逻辑
├── decision/  # 决策规则、互动与人物提示词
├── session/  # 会话状态、候选选择与决策提交
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── xmake.lua  # Agent 命名模块静态库目标
```

## 子目录索引

- [decision/](decision/DIRECTORY.md)
- [session/](session/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 4eb60cec3299598bc6e5dcfed1138746515f4be99704830bb400dac7116d11bf -->
