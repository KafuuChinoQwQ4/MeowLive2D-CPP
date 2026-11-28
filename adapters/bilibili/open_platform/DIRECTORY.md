# open_platform 目录索引

开放平台项目管理和请求签名

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
open_platform/  # 开放平台项目管理和请求签名
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── project_api.cppm  # 已签名的开放平台项目启动、心跳与结束请求
└── signing.cppm  # 平台请求体摘要、HMAC 签名和随机数
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 4a731612f4eeefd49af2cd4a7b2794ef243694dcc8605aeb60e382778f2e8cff -->
