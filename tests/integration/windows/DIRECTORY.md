# windows 目录索引

Windows 安装与受管服务验证

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
windows/  # Windows 安装与受管服务验证
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── stage_2_windows.ps1  # 同机 Windows 上的 Qt 和 Rust 执行端配对与重连验证
├── stage_9_windows.ps1  # Windows 受管启动、试音、故障恢复与外部服务复用实测
└── update_readiness.cpp  # Windows 原生更新检查和训练终态写入故障恢复联调
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 7d3954efeab790b107347d5ac1d9aee998be2704456470aeab449d134e10b9f7 -->
