# synthesis 目录索引

语音合成、权重切换与音频编码

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
synthesis/  # 语音合成、权重切换与音频编码
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── gpt_sovits.cppm  # 通过 API v2 调用 GPT-SoVITS，配置参考录音和分句批大小
├── http.cppm  # 支持总超时、取消和响应大小上限的语音引擎 HTTP 请求
├── model_weights.cppm  # 受管 GPT-SoVITS 推理的成对权重配置
├── trial_pcm.cppm  # 随程序提供的四频试音 PCM
└── wav.cppm  # RIFF WAV 校验与交错 PCM16 转换
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: fe48779490fa51c2503a88310686ea711608bd1e602f756e53575297f86fa397 -->
