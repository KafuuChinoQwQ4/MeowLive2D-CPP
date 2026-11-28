# contracts 目录索引

旧版与 C++ 版本的协议兼容测试及样本

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
contracts/  # 旧版与 C++ 版本的协议兼容测试及样本
├── fixtures/  # 协议契约与集成测试共用的固定样本
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── bilibili_live.cpp  # B 站开放平台签名、压缩消息和回环会话契约
├── llm_providers.cpp  # 四种模型协议和公开搜索的回环契约
├── protocol_v3.cpp  # Rust v3 样本和 C++ 控制、资源、PCM 契约测试
├── windows_update.cpp  # Windows 更新签名与版本兼容契约
└── xmake.lua  # 跨端协议契约测试目标
```

## 子目录索引

- [fixtures/](fixtures/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: e76521b573aa47d01e4f08928b0228316250f7dc4e0193eb845c69fcd30bee60 -->
