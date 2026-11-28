# speech 目录索引

音色和真实语音组件联调

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
speech/  # 音色和真实语音组件联调
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── stage_6_voices.cpp  # 旧音色恢复、参考 WAV 导入、编辑互斥及重启验证
└── stage_8_native_whisper.cpp  # Windows 本机 Whisper 转写、超时与取消验证
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: d64af42396d941880ae7af8d787a9d742277a89786db4b26aeea932c2bfae899 -->
