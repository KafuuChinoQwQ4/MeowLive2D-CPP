# training 目录索引

本机训练和语音组件运行环境

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
training/  # 本机训练和语音组件运行环境
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── native_training.cppm  # Windows GPT-SoVITS 训练进程与版本产物校验
├── training.cppm  # 参考录音转写、训练作业与产物管理模块入口
├── voice_components.cppm  # 本机语音运行时和模型路径探测
├── windows_process.cppm  # 带进程树清理、超时与输出限制的 Windows 子进程
└── windows_voice_models.cppm  # Windows 模型目录、安装收据和下载进程适配
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 8553393eecc4df5a0f8e182fa66336ffc53975b3af82b1b10a606b0ef594f09f -->
