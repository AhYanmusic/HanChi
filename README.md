# HanChi 汉齿 · 中文齿音 VST3 插件

双频段动态去齿音（De-Esser）插件，针对**中文人声**的齿音特性设计。
作者：张蓬（阿言 / A-Yan）× WorkBuddy ｜ v1.1.0

## 功能特性

- **双频段动态去齿音**：低区 4-9 kHz（嘶/嘘，s x c z）+ 高区 7-13 kHz（咝/刺感），
  Linkwitz-Riley 4 阶分频（两极级联 Butterworth Q=0.7071）
- **两种处理模式**：分频段 Split（只压齿音频段，音色透明）/ 宽带 Broadband（模拟老式全带 DeEsser）
- **前瞻 Lookahead**：0-10 ms，保证压缩起始瞬态不漏
- **强度**：0-100%，映射 3:1 压缩比的作用深度
- **内部精度**：32-bit / 64-bit 浮点手动可选（选 64-bit 后即使宿主总线 32-bit，内部也按 64-bit 运算）
- **超采样**：1x / 2x / 4x / 8x 多相半带滤波器（齿音建议 2x-4x，8x CPU 约为 1x 的 3-5 倍）
- **8 套中文人声模板**：男声标准 / 女声标准 / 齿音偏重 / 女声清亮 / 说唱强压 /
  民谣古风轻手 / 配音广播级 / 宽带模拟老 DeEsser
- **模板系统**：「存为模板 / 载入模板 / 删除模板」，用户模板存于
  `%APPDATA%\HanChi\Templates\*.xml`，可跨工程复用
- 其他去齿音插件参数迁移对照见 [docs/参数对照表.md](docs/参数对照表.md)

## 目录结构

```
HanChi/
├── CMakeLists.txt                    # CMake 工程（自动拉取 JUCE 8）
├── Source/
│   ├── PluginProcessor.h / .cpp      # DSP 核心 + 模板系统
│   └── PluginEditor.h / .cpp         # 界面
├── installer/HanChi.iss              # Inno Setup 安装器脚本
└── .github/workflows/build-windows.yml  # GitHub Actions 云端 MSVC 构建
```

## 如何得到 Windows 成品（无需本地安装任何开发工具）

JUCE 8 不支持 MinGW，本地无 MSVC 时推荐用 GitHub Actions 云端构建：

1. 在 [github.com](https://github.com) 注册账号
2. 右上角 **＋** → **New repository** → 命名 `HanChi` → 选 **Public** → Create
3. 仓库页点 **"uploading an existing file"**，把 `HanChi` 文件夹内**全部内容**
   （含 `.github` 隐藏文件夹——资源管理器需先开启「隐藏的项目」）拖进去 → **Commit changes**
4. 打开仓库 **Actions** 标签页，等自动编译（约 6-8 分钟；如无记录，点 **Run workflow** 手动触发）
5. 完成后进入该次运行页面，拉到底部 **Artifacts**，下载 `HanChi-Win64`（绿色版）和 `HanChi-Setup`（安装器版）

## 安装方式

- **绿色版**：解压后把整个 `HanChi.vst3` 文件夹复制到
  `C:\Program Files\Common Files\VST3\`（VST3 是文件夹格式，不是 DLL）
- **安装器版**：直接运行 `HanChi-Setup.exe`，自动安装到上述目录

## 从源码本地构建（可选，需 Visual Studio 2022 + CMake 3.22+）

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
# 产物: build/HanChi_artefacts/Release/VST3/HanChi.vst3
```

## License

个人项目，仅供作者本人使用与学习交流。
