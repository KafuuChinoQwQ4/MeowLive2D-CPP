# MeowLive2D-cpp 目录索引

MeowLive2D 的 C++ 重构工程

本文件由 `npm run tree:update` 生成，列出当前目录的直属文件、子目录及其用途。

```text
MeowLive2D-cpp/  # MeowLive2D 的 C++ 重构工程
├── adapters/  # 实现应用接口的外部服务适配器
├── apps/  # 服务端与桌面端程序入口
├── config/  # 可发布的服务端和桌面端配置模板
├── docs/  # 本地设计文档的独立索引边界
├── libs/  # 可独立构建和测试的 C++ 模块
├── packaging/  # 发布所需的运行环境与安装包材料
├── scripts/  # 开发与目录索引工具
├── tests/  # 跨模块的协议和集成验证
├── .clang-format  # 项目 C++ 源码与命名模块的 Allman 格式规则
├── .gitignore  # 排除构建产物、本地文档、SDK、运行数据和私有配置
├── AGENTS.md  # 项目协作规则、C++ 编码约定与格式风格
├── DIRECTORY.md  # 本目录的直属文件与子目录索引（自动生成）
├── README.md  # 工程结构、构建入口与当前迁移范围
├── package.json  # 目录索引管理命令
└── xmake.lua  # C++26 工程入口，配置跨平台主服务和 Windows 桌面端
```

## 子目录索引

- [adapters/](adapters/DIRECTORY.md)
- [apps/](apps/DIRECTORY.md)
- [config/](config/DIRECTORY.md)
- [docs/](docs/DIRECTORY.md)
- [libs/](libs/DIRECTORY.md)
- [packaging/](packaging/DIRECTORY.md)
- [scripts/](scripts/DIRECTORY.md)
- [tests/](tests/DIRECTORY.md)

在 `scripts/directory-descriptions.json` 中维护用途说明。增删、移动条目或调整职责后运行生成命令。

内容变更会刷新所在目录及祖先目录的索引指纹。运行 `npm run tree:check` 检查索引是否最新。

<!-- directory-tree-sha256: 998b2132aaaeeeb6e4cca54538e4ed8b9342ad8bbda24120e254828699a0acfb -->
