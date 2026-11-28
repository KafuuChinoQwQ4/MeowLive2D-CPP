# viewers 目录索引

观众、记忆和关系管理接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
viewers/  # 观众、记忆和关系管理接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── graph_http.cpp  # 关系查询、维护请求解析和关系更正用例调用
├── graph_http.cppm  # 关系查询和图投影维护 HTTP 接口
├── memory_http.cpp  # 观众记忆请求解析和用例调用
├── memory_http.cppm  # 观众记忆和索引维护 HTTP 接口
├── storage_http.cppm  # 观众存储操作和依赖故障的 HTTP 响应
├── viewer_http.cpp  # 观众查询、积分和身份合并请求处理
└── viewer_http.cppm  # 管理员观众、事件、积分及身份合并入口
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 8f92c769dda6be3da3924e043b3e6a57da8a952e8f93b826ab107bc2d0ec0785 -->
