# tests 目录索引

跨模块的协议和集成验证

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
tests/  # 跨模块的协议和集成验证
├── benchmarks/  # Windows 播报性能测量及测试替身
├── contracts/  # 旧版与 C++ 版本的协议兼容测试及样本
├── integration/  # 服务、数据库及桌面端之间的联动验证
└── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
```

## 子目录索引

- [benchmarks/](benchmarks/DIRECTORY.md)
- [contracts/](contracts/DIRECTORY.md)
- [integration/](integration/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 749ebe94878af0614890be7656d8c250bf749f1c38bcaaa6ec3cc40b34ff2975 -->
