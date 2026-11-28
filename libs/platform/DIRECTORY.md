# platform 目录索引

跨平台标准库接入与异常诊断

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
platform/  # 跨平台标准库接入与异常诊断
├── include/  # 平台兼容头文件
├── windows/  # Windows 内核资源所有权
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── exception_diagnostics.cppm  # 异常分类、上下文日志和进程终止诊断
├── standard.cppm  # 标准库命名模块入口
└── xmake.lua  # 标准库模块构建目标
```

## 子目录索引

- [include/](include/DIRECTORY.md)
- [windows/](windows/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 1e5cf38d0abb416f64792e0c2124aefb82fc7d89507a3137257addd6a97fd62a -->
