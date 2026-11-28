# unit 目录索引

应用层与其他独立模块的单元测试

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
unit/  # 应用层与其他独立模块的单元测试
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── agent_session.cpp  # Agent 公平、事件时效和调度的固定时钟测试
├── http_work.cpp  # HTTP 排队拒绝、异常分类、日志和回复次数测试
├── live_session.cpp  # 直播事件时效、存储积压和会话恢复测试
├── memory_extraction.cpp  # 记忆证据、候选生命周期和取消边界单元测试
├── reply_completion.cpp  # 播毕结算与停止、重复回执及断线顺序验证
├── speech_wav.cpp  # WAV 格式、转换及边界校验单元测试
├── voice_download_test.py  # 语音组件下载校验和训练运行环境解包测试
├── work_queue.cpp  # 工作队列容量、任务顺序与关闭行为测试
└── xmake.lua  # C++ 单元测试目标
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: e8ad1393a37264e77f3fbe385db16c10ee91e7daa94d958590d048fa30cf13ee -->
