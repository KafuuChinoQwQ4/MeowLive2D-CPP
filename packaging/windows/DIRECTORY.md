# windows 目录索引

Windows 客户端、Qt 运行库和 NSIS 安装材料

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
windows/  # Windows 客户端、Qt 运行库和 NSIS 安装材料
├── voice/  # Windows 原生语音模型、组件安装和运行脚本
├── wsl/  # WSL 上的 Windows 交叉构建辅助工具
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── build-installer.sh  # 从 Windows x64 制品和运行库生成 NSIS 安装包
├── import-rust-data.py  # 将 Rust 快照迁入新的 Windows 用户数据目录
├── installer.nsi  # Windows 每用户 NSIS 安装与卸载脚本
└── update-manifest.mjs  # 生成 Ed25519 签名的 Windows 更新清单与分块
```

## 子目录索引

- [voice/](voice/DIRECTORY.md)
- [wsl/](wsl/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 17837c7324b7d24715753d0f110147f83f19ed1459698f5734b1da98615c6a64 -->
