# config 目录索引

可发布的服务端和桌面端配置模板

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
config/  # 可发布的服务端和桌面端配置模板
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── server.example.toml  # 服务监听、来源与凭据配置样例
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 7093678c3666fdaaa80a843eda9c9fea8cfb5da7e1a0968e41ecf58dea03ee05 -->
