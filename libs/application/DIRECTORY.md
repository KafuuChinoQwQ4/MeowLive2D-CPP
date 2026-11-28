# application 目录索引

直播互动与播放调度用例，以及外部适配器实现的接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
application/  # 直播互动与播放调度用例，以及外部适配器实现的接口
├── concurrency/  # 有界后台任务队列
├── desktop/  # 桌面执行端状态查询接口
├── live/  # 直播用例及会话生命周期
├── memory/  # 观众记忆提取与查询
├── models/  # 语言模型调用接口
├── speech/  # 语音合成、播放调度和回复结算接口
├── training/  # 语音训练任务生命周期
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── xmake.lua  # 应用层命名模块静态库目标
```

## 子目录索引

- [concurrency/](concurrency/DIRECTORY.md)
- [desktop/](desktop/DIRECTORY.md)
- [live/](live/DIRECTORY.md)
- [memory/](memory/DIRECTORY.md)
- [models/](models/DIRECTORY.md)
- [speech/](speech/DIRECTORY.md)
- [training/](training/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 2319892e017a1e449e273c1baea10577ea63ef151c6833ae05896b906dfd7a3e -->
