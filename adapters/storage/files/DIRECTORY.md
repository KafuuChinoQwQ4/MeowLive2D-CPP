# files 目录索引

本地文件写入及音色目录持久化

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
files/  # 本地文件写入及音色目录持久化
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── atomic_file.cppm  # 持久文件的同步写入和原子替换
└── voice_catalog.cppm  # 版本化音色目录与参考 WAV 存储
```

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 437bde8b2d59b71281da80aa371c69c287660ada60b606b7ef129ed97477495e -->
