# agent 目录索引

Agent 运行协调、模型调用和观测

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
agent/  # Agent 运行协调、模型调用和观测
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── agent_json.cppm  # 事件和 Agent 设置的 HTTP 数据映射
├── agent_model.cppm  # 按本机配置装配模型适配器
├── agent_observability.cppm  # 异步保存有界决策轨迹、用量和中断恢复
├── agent_runtime_settings.cppm  # 工具和运行设置的脱敏读取与原子保存
├── agent_worker.cppm  # Agent 运行入口、依赖和工作线程生命周期
├── decision_loop.cpp  # 模型决策运行、超时和提交
├── model_tools.cpp  # Agent 模型工具调用
├── playback_state.cpp  # 决策观测与播报状态同步
└── worker_status.cpp  # Agent 状态、轨迹和用量查询
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: b94d7c273a4e93d710b305a0e7461a848878dccf864f98820e8bfc0a55db0e5e -->
