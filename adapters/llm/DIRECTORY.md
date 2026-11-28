# llm 目录索引

模型厂商的请求、流式回复、工具调用和用量适配

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
llm/  # 模型厂商的请求、流式回复、工具调用和用量适配
├── providers/  # 模型请求、响应和供应商协议
├── tools/  # 向量嵌入和搜索服务
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── http.cppm  # 可取消且限制响应体大小的模型 HTTP 传输
```

## 子目录索引

- [providers/](providers/DIRECTORY.md)
- [tools/](tools/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 08a1c57a02e26409755259b3d59a7c4f9392686f232b5e89def36a8075fbb81d -->
