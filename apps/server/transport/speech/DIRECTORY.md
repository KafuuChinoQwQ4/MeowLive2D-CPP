# speech 目录索引

播报、音色和角色接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
speech/  # 播报、音色和角色接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── character_http.cppm  # 角色档案和执行端模型资源路由
├── speech_http.cppm  # 播报请求校验与任务快照序列化
└── voice_http.cppm  # 音色查询、WAV 上传、选择与删除入口
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 76be643970c7fc67c26caddc7bcf359da86da6fc82d72a40380b779d14a86e25 -->
