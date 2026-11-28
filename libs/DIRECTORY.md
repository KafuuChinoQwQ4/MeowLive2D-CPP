# libs 目录索引

可独立构建和测试的 C++ 模块

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
libs/  # 可独立构建和测试的 C++ 模块
├── agent/  # 链接进主服务的 Agent 决策、工具调用与取消逻辑
├── application/  # 直播互动与播放调度用例，以及外部适配器实现的接口
├── desktop-runtime/  # Windows 的设备与直播软件执行能力
├── domain/  # 直播事件、人物卡和发言等领域类型
├── platform/  # 跨平台标准库接入与异常诊断
├── protocol/  # 跨端 HTTP、控制消息、PCM 音频帧和执行回执的契约
└── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
```

## 子目录索引

- [agent/](agent/DIRECTORY.md)
- [application/](application/DIRECTORY.md)
- [desktop-runtime/](desktop-runtime/DIRECTORY.md)
- [domain/](domain/DIRECTORY.md)
- [platform/](platform/DIRECTORY.md)
- [protocol/](protocol/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 9158fbdb85ffecae4d13cea4b138bd2adb02f3b2b88f0e6ab171e4e0347a4cb1 -->
