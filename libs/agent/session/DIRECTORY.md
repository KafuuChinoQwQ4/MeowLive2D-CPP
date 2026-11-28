# session 目录索引

会话状态、候选选择与决策提交

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
session/  # 会话状态、候选选择与决策提交
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── candidate_selection.cpp  # 筛选、排序和聚合 Agent 待处理事件
├── decision_lifecycle.cpp  # 决策开始、失败及事件释放
├── decision_resolution.cpp  # 校验 Agent 决策并准备播报和提交结果
├── event_admission.cpp  # 直播事件准入与过期清理
├── playback.cpp  # Agent 播报结果与状态迁移
└── session.cppm  # Agent 会话接口、容量规则与状态记录
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: a6d63bb593a2616e7c5b57eca52ba1c5761e609bbd244b335f5e7a584c8cdab9 -->
