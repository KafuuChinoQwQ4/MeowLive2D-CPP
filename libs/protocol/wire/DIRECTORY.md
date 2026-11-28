# wire 目录索引

音频分帧、控制消息和执行回执

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
wire/  # 音频分帧、控制消息和执行回执
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── audio.cppm  # MLPC 帧边界和 PCM 分片编解码
├── control.cppm  # v3 握手、服务器命令和执行端消息编解码
├── execution.cppm  # 播放任务执行状态与回执类型
├── managed_server.cppm  # Windows 受管服务的更新关停命令和应答
└── resources.cppm  # 桌面角色与音色资源请求和结果
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 9711b52321366acb199362c8f8ddf6f651f9bdde103f9bae2ad1ec14a03cd6b7 -->
