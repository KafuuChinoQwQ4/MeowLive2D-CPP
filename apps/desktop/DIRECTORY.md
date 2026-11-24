# desktop 目录索引

Windows Qt Widgets 桌面程序

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
desktop/  # Windows Qt Widgets 桌面程序
├── services/  # 界面访问主服务及管理本机服务进程的接口
├── ui/  # 控制台窗口、页面和 Qt Designer 表单
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── main.cpp  # Qt 控制台入口，读取连接地址和设备凭据
└── xmake.lua  # Windows Qt Widgets、Qt Network 和 Qt WebSockets 可执行目标
```

## 子目录索引

- [services/](services/DIRECTORY.md)
- [ui/](ui/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 146db9d74e460f65767496bdf5554f4d6e9a80f3a715e28af246b28cc2f3ad56 -->
