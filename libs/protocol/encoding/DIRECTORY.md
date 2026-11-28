# encoding 目录索引

JSON 和 UTF-8 编解码

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
encoding/  # JSON 和 UTF-8 编解码
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── json.cppm  # 控制载荷的严格 JSON 读写和字段检查
└── utf8.cppm  # 协议字符串和音频标识的 UTF-8 校验
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 5c3432ecd0e8ecfd1ac99e850340691e55fc91e8732bfcbe12d11b5d8a1bf80a -->
