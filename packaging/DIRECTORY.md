# packaging 目录索引

发布所需的运行环境与安装包材料

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
packaging/  # 发布所需的运行环境与安装包材料
├── windows/  # Windows 客户端、Qt 运行库和 NSIS 安装材料
├── wsl/  # WSL 发行版、后端运行环境与模型组件安装材料
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── drogon.lua  # Drogon 及其固定 Trantor 源码的 Xmake 依赖规则
└── tomlplusplus.lua  # 固定版本的 toml++ 头文件依赖规则
```

## 子目录索引

- [windows/](windows/DIRECTORY.md)
- [wsl/](wsl/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 42cd5c81e99031a3ed8680b46419e0945b4b671313ad44e2f478278e6dee6889 -->
