# configuration 目录索引

配置读取、校验和本地设置加载

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
configuration/  # 配置读取、校验和本地设置加载
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── configuration.cppm  # 服务配置类型、默认配置和加载入口
├── graph_configuration.cppm  # Neo4j 服务配置和校验
├── listener_settings.cpp  # 服务监听、来源和鉴权参数读取
├── service_settings.cpp  # 语音、直播和观众服务配置读取
└── settings.cppm  # 配置同目录的 Agent、LLM 与直播设置读取及旧版本迁移
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: fe3abbcb3582e17ee0bd28d703e89d9debba7fc6ba89ea797f7583078caad352 -->
