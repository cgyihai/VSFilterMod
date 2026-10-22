# VSFilterMod — 跨平台版

[![CI](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml/badge.svg)](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)

[English](README.md) | **简体中文**

把 VSFilterMod 字幕渲染器移植到 **Windows、Linux 和 macOS**。
编译产物是一个库，既是 Aegisub 用的 **CSRI** 插件，也是 **AviSynth+** 插件
（`TextSubMod` / `MaskSubMod`）；另外还有命令行工具 `vsfmrender`，可直接把字幕
渲染成蓝光制作用的 **BDN+XML**。

ASS 解析器和光栅化器都是 VSFilterMod 原版代码，所以全部扩展标签照旧可用：
`\pngimg` 图片背景、渐变、可移动矢量 clip、`\rnd` 抖动、distort、`\frs` 字符旋转、
Z 坐标、竖向间距等。

## 目录

- [特性](#特性)
- [下载](#下载)
- [使用方法](#使用方法)
- [编译](#编译)
- [与原版的一致程度](#与原版的一致程度)
- [与原版的区别](#与原版的区别)
- [已知问题](#已知问题)
- [致谢与许可证](#致谢与许可证)

## 特性

- **三个平台同一套渲染器。** 字形统一由 FreeType 提供，同一份字幕在
  Windows、Linux、macOS 上渲染结果相同。
- **还原原版效果，并经过实测。** VSFilterMod 的排版依赖 GDI 的地方——按字格高度
  定字号、合成粗体和斜体、`\frs`、`@` 竖排字体、缺字回退——引擎都按 GDI 的实际行为
  复现。这些规则是直接测量 GDI 得出的，并与原版逐像素比对过。
- **Aegisub 插件**（CSRI），用于实时预览。
- **AviSynth+ 插件**，`TextSubMod` / `MaskSubMod` 的参数与原版一致，现有脚本和
  avs2bdnxml 可以照常使用。
- **BDN+XML 输出**：`vsfmrender` 的结果与 avs2bdnxml 调用本插件 `MaskSubMod`
  得到的结果逐字节相同。
- **内嵌字体**（`[Fonts]` 附件）可用，通过 CSRI 也可以。
- **输出不随机器变化**：DPI 和代码页都是固定的，不读取本机设置。

## 下载

每次推送都会由 GitHub Actions 在所有平台上自动编译。打了版本标签的正式版发布在
[Releases](https://github.com/cgyihai/VSFilterMod/releases) 页面；任意一次提交的编译结果，
可以在 [Actions](https://github.com/cgyihai/VSFilterMod/actions) 页面对应运行的
*Artifacts* 里下载。

| 平台 | 压缩包 | 内容 |
| --- | --- | --- |
| Windows x86_64 | `vsfiltermod-windows-x86_64-msvc` | `vsfiltermod.dll`、`vsfmrender.exe`，单文件即可用，无需额外 DLL |
| Linux x86_64 | `vsfiltermod-linux-x86_64` | `vsfiltermod.so`、`vsfmrender` |
| Linux aarch64 | `vsfiltermod-linux-aarch64` | `vsfiltermod.so`、`vsfmrender` |
| macOS Apple Silicon | `vsfiltermod-macos-arm64` | `vsfiltermod.dylib`、`vsfmrender` |
| macOS Intel | `vsfiltermod-macos-x86_64` | `vsfiltermod.dylib`、`vsfmrender` |

Linux 和 macOS 版本链接系统的 FreeType、libpng，Linux 还需要 fontconfig，
请用包管理器或 Homebrew 安装。

## 使用方法

### Aegisub

**Windows：** 把 `vsfiltermod.dll` 放进 `aegisub.exe` 旁边的 `csri` 文件夹，
重启 Aegisub，在 *选项 → 高级 → 视频 → 字幕渲染器* 中选择
**CSRI/vsfiltermod_textsub**。

**Linux / macOS：** 官方 Aegisub 在这两个平台上不加载 CSRI 插件。需要先给
Aegisub 打上 [`docs/aegisub-csri-posix.patch`](docs/aegisub-csri-posix.patch)
并重新编译，然后：

```bash
mkdir -p ~/.csri
cp vsfiltermod.so ~/.csri/          # macOS 上是 vsfiltermod.dylib
chmod +x ~/.csri/vsfiltermod.so     # 加载器会跳过没有执行权限的文件
```

详见 [BUILD.zh-CN.md](BUILD.zh-CN.md#安装到-aegisub)。

### AviSynth+ 与 avs2bdnxml

同一个库也是 AviSynth+ 插件：

```avs
LoadPlugin("vsfiltermod.dll")
MaskSubMod("subtitle.ass", 1920, 1080, 23.976, 3000)   # RGBA 输出，给 avs2bdnxml 用
# 或者叠加到视频上：
# TextSubMod("subtitle.ass")
```

任何通过 AviSynth+ 加载脚本的 64 位 avs2bdnxml 都能使用。
32 位程序无法加载 64 位插件。

### vsfmrender

不经过 AviSynth，直接把字幕渲染成 BDN+XML：

```bash
vsfmrender --size 1920x1080 --fps 23.976 input.ass output.xml
```

| 参数 | 含义 |
| --- | --- |
| `-s, --size WxH` | 画面尺寸（默认 `1920x1080`） |
| `-f, --fps RATE` | `23.976`、`24`、`25`、`29.97`、`50`、`59.94`，或精确分数 |
| `-l, --lang CODE` | ISO 639-2 语言代码（默认 `und`） |
| `-t, --title TEXT` | 写入索引的标题 |
| `--first N` / `--last N` | 导出的帧范围 |
| `--no-crop` | 输出整帧图片（相当于 avs2bdnxml 的 `-a0`） |
| `-q, --quiet` | 不显示进度 |

## 编译

```bash
meson setup build
meson compile -C build
meson test -C build
```

需要：Meson ≥ 0.59、Ninja、支持 C++17 的编译器、FreeType ≥ 2.8、libpng ≥ 1.6，
Linux 上还需要 fontconfig。Windows 上 FreeType、libpng、zlib 会作为 Meson 子项目
自动下载。完整说明、编译选项和 CI 矩阵见 [BUILD.zh-CN.md](BUILD.zh-CN.md)。

## 与原版的一致程度

在 Windows 上与原版 VSFilterMod 的 CSRI 插件对比，逐行计算两边字形覆盖区域的
重合度（IoU），1.000 表示像素完全一致。

| 测试字幕 | 覆盖内容 | IoU |
| --- | --- | --- |
| `tests/smoke.ass` | 基本样式与定位 | 1.000 |
| `tests/frs.ass` | 各角度的 `\frs`；中日韩文字、`\fsp`、粗体、斜体、换行 | 0.977–0.999 |
| `tests/vertical.ass` | `@` 竖排字体；标点、拼音、希腊字母、全角字符 | 0.967–1.000 |
| `tests/fallback.ass` | 拉丁字体中的中日韩、韩文字符（缺字回退） | 0.985–0.999 |
| `tests/bold.ass` | 合成粗体，含斜体、`\frs` 和缺字回退 | 0.977–1.000 |

事件时间完全按 `MaskSubMod` 的时钟计算（包括单精度取整），所以 BDN+XML 中每个
事件的起止帧与原版相同。剩余差异是字形边缘的抗锯齿，在一个像素以内。
Linux 和 macOS 运行的是同一套代码，那里的差异主要取决于系统安装了哪些字体。

## 与原版的区别

**字形来自 FreeType，而不是 GDI。** 排版代码依赖 GDI 的地方，引擎都做了模拟：
`LOGFONT` 的正数高度是字格高度而不是 em 大小（这正是 VSFilter 字幕在其他渲染器里
字号不对的常见原因）；`\frs` 下 GDI 会改变字体度量，并按未公开的规则排列、旋转和
测量字形；`@` 字体会把中日韩字形横躺，并换用 GSUB `vert` 竖排字形。这些都已复现，
详见 `src/font/font_engine.cpp`。按名字查找字体仍使用各平台原生接口：DirectWrite、
CoreText 或 fontconfig。

**缺字回退与 GDI 一致。** 样式字体缺少的字符，依次从该字体的 Windows SystemLink
列表、宋体和 Malgun Gothic、macOS 与 Linux 的中日韩字体、平台自身的回退机制中
获取。回退字形的字号、基线以及 GDI 对这类文字做的字距调整都与 GDI 相同。

**合成粗体按 GDI 的规则实现**，逐点测量得出，包括旋转文字时更重的笔画。

**内嵌字体可用**，通过 CSRI 和在所有平台上都可以：附件字体直接交给 FreeType，
不再安装到系统里。

**去掉了 DirectShow 部分。** DirectShow 滤镜及其属性页、VobSub/DVB/HDMV/CC/USF/
SSF/RealText 解析器和 DirectX 字幕图层后端都已删除，Aegisub、AviSynth 和 BDN+XML
输出都用不到它们。`VobSub()` 和 VapourSynth 插件没有移植。

**渲染结果不依赖机器。** DPI 固定为 96，字符集到代码页的映射使用固定表格。
唯一的例外是 Windows 上的缺字回退，它和 GDI 一样遵循本机的 SystemLink 设置。

## 已知问题

- OpenType-CFF 格式的 `@` 竖排字体（如 `@Source Han Sans`）在这里用该字体渲染。
  原版的 GDI 无法以竖排方式打开 CFF 字体，会悄悄换成系统字体（中文 Windows 上是
  宋体）；照抄这一点会让输出依赖 Windows 的语言设置。
- `@MS Gothic` 和 `@Malgun Gothic` 不旋转希腊字母和西里尔字母，而本移植会把它们
  横躺；宋体和黑体在 `\frs` 下的假名间距与其他字体不同。这两处由无法还原的 GDI
  内部表格决定。
- 像 `@Arial` 这样 GDI 不认识的字体名，会让 GDI 把整行（包括拉丁字母）换成宋体；
  这里拉丁字母仍使用样式字体。
- Linux 和 macOS 版本由 CI 编译并做冒烟测试，但尚未与原版逐像素比对。

## 致谢与许可证

基于 Gabest（Guliverkli）的 **VSFilter** 和 **VSFilterMod**。`include/avisynth`
中的 AviSynth+ C API 头文件来自 AviSynth+ 项目。

与原版相同，采用 [GNU 通用公共许可证 2.0 或更高版本](LICENSE)。
