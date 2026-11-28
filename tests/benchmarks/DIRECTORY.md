# benchmarks 目录索引

Windows 播报性能测量及测试替身

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
benchmarks/  # Windows 播报性能测量及测试替身
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── audio_loopback.cppm  # Windows WASAPI 回环采样与测试音检测
├── controlled_tts.cppm  # 返回合成参考 WAV 的本机测试 TTS 服务
├── playback_baseline.cpp  # 阶段 1.5 播报基线测量与 JSON 报告
└── xmake.lua  # Windows 原生播报测量目标
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: c04b5460087015e33edc0bb7d3c8c81cc5a2332fa3d5c92e78134a4c66b9bb22 -->
