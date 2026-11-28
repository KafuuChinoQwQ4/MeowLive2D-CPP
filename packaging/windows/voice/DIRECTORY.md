# voice 目录索引

Windows 原生语音模型、组件安装和运行脚本

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
voice/  # Windows 原生语音模型、组件安装和运行脚本
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── engine_workspace.py  # 按训练任务准备独立 GPT-SoVITS 工作目录
├── gpt_training_runtime.py  # 解包和探测 Windows GPT-SoVITS CUDA 训练环境
├── install-gpt-sovits.py  # 核对摘要后从兼容离线包部署 Windows GPT-SoVITS v2
├── install-voice-model.py  # 从固定资产安装转写环境和模型并发布收据
├── install-whisper.py  # 下载及离线安装 Windows Whisper 组件
├── train-gpt-sovits.py  # Windows GPT-SoVITS 权重训练和产物导出入口
├── voice-models.json  # Windows 转写、训练底模与运行环境的固定修订和 SHA-256
├── voice_download.py  # 按摘要缓存组件，核对范围响应并恢复下载
├── voice_recognition.py  # 本机加载探测和按模型转写 WAV
└── whisper-components.json  # Windows Whisper 依赖、模型版本与 SHA-256 摘要
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 8a7320f918560e5ec936f53f5c5becf4b49f84678b4017987ab7a7e2c1dde7d4 -->
