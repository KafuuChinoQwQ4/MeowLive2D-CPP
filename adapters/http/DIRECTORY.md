# http 目录索引

外部 HTTP 适配器共用的 libcurl 边界

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
http/  # 外部 HTTP 适配器共用的 libcurl 边界
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── curl_resources.cppm  # libcurl 资源不足错误转换
└── response_body.cppm  # 响应体大小限制与 C 回调异常传递
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: f2c388d2e822ab0008627cee42e737bf05f980dc9ffb76d1142105671603dcdf -->
