# scripts 目录索引

开发与目录索引工具

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
scripts/  # 开发与目录索引工具
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── directory-descriptions.json  # 工程目录与文件中文用途登记
├── directory-tree.mjs  # 按用途登记为各目录生成和核验直属条目索引
├── directory-tree.test.mjs  # 验证逐层索引、内容指纹和 Xmake 产物排除规则
└── format_cpp.lua  # 供 Xmake 构建后钩子和手动命令共用的 C++ 格式化入口
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: f2ff2bb749a50c1211d5c12bad7e57ca970c7eb12cb7656ef15c32093fd2e041 -->
