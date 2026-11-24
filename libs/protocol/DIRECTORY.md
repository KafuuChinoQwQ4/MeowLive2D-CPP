# protocol 目录索引

跨端 HTTP、控制消息、PCM 音频帧和执行回执的契约

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
protocol/  # 跨端 HTTP、控制消息、PCM 音频帧和执行回执的契约
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── audio.cppm  # MLPC 帧边界和 PCM 分片编解码
├── control.cppm  # v3 握手、服务器命令和执行端消息编解码
├── execution.cppm  # 播放任务执行状态与回执类型
├── json.cppm  # 控制载荷的严格 JSON 读写和字段检查
├── resources.cppm  # 桌面角色与音色资源请求和结果
├── utf8.cppm  # 协议字符串和音频标识的 UTF-8 校验
├── version.cppm  # 跨端共享的协议版本
└── xmake.lua  # 两端共享的协议命名模块静态库目标
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: f0b2fc136849265e7edf419abf7c24662a1b0b20f5e87483174164f9b64fb4e5 -->
