# wsl 目录索引

WSL 上的 Windows 交叉构建辅助工具

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
wsl/  # WSL 上的 Windows 交叉构建辅助工具
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── clang-cl  # 规范化 WSL 交叉构建的 clang-cl 响应文件和依赖记录
└── xmake-windows  # 保留 WSL 的 Windows 构建配置并修正 Xmake 模块缓存检查
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 7d1f3974418446920180e3dad7252843522e265045221a834e9519b7fd361446 -->
