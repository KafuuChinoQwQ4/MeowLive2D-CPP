# protocol 目录索引

跨端 HTTP、控制消息、PCM 音频帧和执行回执的契约

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
protocol/  # 跨端 HTTP、控制消息、PCM 音频帧和执行回执的契约
├── encoding/  # JSON 和 UTF-8 编解码
├── wire/  # 音频分帧、控制消息和执行回执
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── version.cppm  # 跨端共享的协议版本
└── xmake.lua  # 两端共享的协议命名模块静态库目标
```

## 子目录索引

- [encoding/](encoding/DIRECTORY.md)
- [wire/](wire/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 5aa689209128cd223311550f6d2971e1839edc939f9c90c1ff57c1eba433e202 -->
