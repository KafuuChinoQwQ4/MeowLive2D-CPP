# live 目录索引

直播会话联调

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
live/  # 直播会话联调
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
└── stage_5_live.cpp  # 直播管理接口鉴权、凭据脱敏、写入回滚与重启测试
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 044a60030ed012ca100e7cad2137093032fa38a6238c9eacfce67a4e9bdf3253 -->
