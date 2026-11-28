# meowlive 目录索引

工程公共头文件

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
meowlive/  # 工程公共头文件
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── msvc_stl.hpp  # Windows 模块全局片段使用的 MSVC 标准库头文件
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 6ac949da2c29f6383930e375c4b4cee8e78125b5c532b7dfacbeeab0f0a9f358 -->
