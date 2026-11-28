# http 目录索引

HTTP 注册、请求校验及鉴权

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
http/  # HTTP 注册、请求校验及鉴权
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── access.cppm  # 管理员会话、设备鉴权和来源校验
├── http.cpp  # HTTP 鉴权、方法白名单与请求分发
├── http.cppm  # 导出主服务 HTTP 路由注册入口
├── http_response.cppm  # HTTP 错误响应和异常状态转换
├── http_routes.cpp  # Drogon 路由注册、异常、预检和跨源响应
├── http_work.cppm  # 异步 HTTP 任务提交、异常诊断和回复
└── media_type.cppm  # 管理员会话请求的 JSON 媒体类型校验
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 0c2bb21d68e11838686afa39e2eef3c599d778e5d2050c940d676353d428bbc1 -->
