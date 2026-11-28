# desktop 目录索引

桌面执行端状态查询接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
desktop/  # 桌面执行端状态查询接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── obs_status.cppm  # Agent 读取桌面 OBS 状态的接口和结果
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 5a0abbc242815b4e07d1c248ec7e607f7781d47cd6fdd4c92c1e87fbfc25f7f3 -->
