# system 目录索引

运行日志和版本更新界面

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
system/  # 运行日志和版本更新界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── log_panel.cppm  # 本机主服务日志的有界末尾浏览
└── update_panel.cppm  # 手动更新下载、空闲检查与安装交接
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 8712cef1cd04698a917ca60a98df30378255bab1662eb6b2dd862aff13dc5e89 -->
