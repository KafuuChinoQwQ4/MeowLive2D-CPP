# training 目录索引

转写和训练任务接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
training/  # 转写和训练任务接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── training_http.cpp  # 录音转写与语音组件路由实现
├── training_http.cppm  # 语音训练、转写及模型路由入口
├── training_job_http.cppm  # 训练任务管理和音频上传路由
├── training_limits.cppm  # 训练上传总量及 multipart 请求体上限
└── voice_model_http.cpp  # 语音模型目录与下载任务的本机管理路由
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: f42babac57447ffe9e7f54bce6f718836767cb09e0d897d3d03c12ad6edefcc4 -->
