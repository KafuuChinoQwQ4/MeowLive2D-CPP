# tooling 目录索引

项目开发工具的自动化验证

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
tooling/  # 项目开发工具的自动化验证
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── module-dependencies.test.mjs  # 校验模块依赖检查对合法导入、越层和依赖环的处理
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 42c47d7af7703fd67118196884ffd466aa70e483a29f0eb19b722bbcb8c892f1 -->
