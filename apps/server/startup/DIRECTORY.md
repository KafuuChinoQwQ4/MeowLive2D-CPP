# startup 目录索引

服务启动装配、宿主生命周期与关闭

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
startup/  # 服务启动装配、宿主生命周期与关闭
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── bootstrap.cpp  # Windows 与 WSL 主服务配置、适配器装配和关停实现
├── bootstrap.cppm  # 导出主服务进程入口
└── managed_lifetime.cppm  # 跟踪 Windows 桌面持有的进程输入管道
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 32b1c585ea043511a230830a3cd0b7e64872808efaec76d40954ee160456df04 -->
