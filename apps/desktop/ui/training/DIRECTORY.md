# training 目录索引

语音训练、转写和任务界面

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
training/  # 语音训练、转写和任务界面
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── training_jobs_panel.cppm  # 训练任务进度、产物管理和试听界面
├── training_panel.cppm  # 参考录音转写、文本校对与音色导入
├── transcription_choices.cppm  # 从目录选择就绪的转写模型并记忆上次选择
└── voice_models_panel.cppm  # 可选模型、下载规划和安装状态界面
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: f39135520b240af12a2a9efd480403180425c8dbfd5bac8e212da9a1951added -->
