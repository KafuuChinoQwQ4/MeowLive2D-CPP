# training 目录索引

语音训练任务与转写服务

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
training/  # 语音训练任务与转写服务
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── training_execution.cpp  # 训练执行、取消与进度更新
├── training_jobs.cppm  # 训练任务管理接口和任务状态持久化
├── training_versions.cpp  # 训练版本选择与权重校验
├── training_worker.cpp  # 服务端 Windows 语音组件装配和转写流程
└── training_worker.cppm  # 转写队列、下载目录和训练作业接口
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: fdc81ccbec0237a61b38cfcb3a37eb1de8d82d9c23f8fce07e2b067e12d274e9 -->
