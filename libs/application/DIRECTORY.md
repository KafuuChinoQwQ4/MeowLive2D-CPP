# application 目录索引

直播互动与播放调度用例，以及外部适配器实现的接口

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
application/  # 直播互动与播放调度用例，以及外部适配器实现的接口
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── llm.cppm  # 模型决策请求、历史回合及决策结果类型
├── scheduler.cppm  # 播报队列、代次取消与状态转换模块入口
├── speech.cppm  # 语音合成请求与交错 PCM 结果类型
└── xmake.lua  # 应用层命名模块静态库目标
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 6504401392e910a210a2806b895ebfa1fe8f71701064dc04cda7cd73185291e1 -->
