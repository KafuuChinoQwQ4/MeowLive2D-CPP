# update 目录索引

Windows 更新清单验证与安装包准备

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
update/  # Windows 更新清单验证与安装包准备
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── download.cppm  # 按摘要缓存分块、组装并验证安装包
├── main.cpp  # 更新下载、进程退出等待与安装交接入口
└── manifest.cppm  # Ed25519 签名、发行版本和分块清单校验
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 1370da81a36058f5affa7c32c0cc4b29d2c6c0632a371c54ab2d72349105f8b8 -->
