# providers 目录索引

模型请求、响应和供应商协议

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
providers/  # 模型请求、响应和供应商协议
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── language_model.cppm  # 模型列表、连接测试与四种原生模型接口
├── output.cppm  # 模型决策、工具请求和用量解析
├── provider_config.cppm  # 服务商 API 格式与模型连接配置
├── reasoning_options.cpp  # 模型供应商推理参数适配
├── request.cppm  # 按模型协议组装决策请求与上下文
├── stream.cppm  # 模型 SSE 结束状态与供应商分片还原
└── tool_messages.cpp  # 模型工具定义及工具轮次消息构造
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 8c5ab6bb16f6d05f521965563d774a683201b93503747a39722e8cdbb6406ebf -->
