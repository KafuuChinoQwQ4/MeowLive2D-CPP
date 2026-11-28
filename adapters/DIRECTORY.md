# adapters 目录索引

实现应用接口的外部服务适配器

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
adapters/  # 实现应用接口的外部服务适配器
├── bilibili/  # B 站直播开放平台认证、连接和直播事件转换
├── http/  # 外部 HTTP 适配器共用的 libcurl 边界
├── llm/  # 模型厂商的请求、流式回复、工具调用和用量适配
├── speech/  # GPT-SoVITS 推理、训练与素材转写的进程或 API 适配
├── storage/  # 本机资源、PostgreSQL、pgvector 和图数据库的存储适配
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── xmake.lua  # 外部服务适配器静态库目标
```

## 子目录索引

- [bilibili/](bilibili/DIRECTORY.md)
- [http/](http/DIRECTORY.md)
- [llm/](llm/DIRECTORY.md)
- [speech/](speech/DIRECTORY.md)
- [storage/](storage/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 7c16a1fe7fb91339d752d6067d8eebb323a96d74a25803e6bbed7b599a395ce5 -->
