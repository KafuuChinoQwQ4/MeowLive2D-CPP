# speech 目录索引

语音合成、播放调度和回复结算接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
speech/  # 语音合成、播放调度和回复结算接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── reply_completion.cppm  # 确认播毕的事件键和结算接口
├── scheduler.cppm  # 播报队列、代次取消与回执状态机
├── speech.cppm  # 语音合成接口、请求和交错 PCM 结果
├── speech_worker.cppm  # FIFO 合成、执行端派发、回执确认和代次取消
└── voice_models.cppm  # 语音模型选择、下载队列与取消用例
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 3a4baca12e416bec172610e94fbcfac2b8161f0d36e9249e56c6739fe0377f2e -->
