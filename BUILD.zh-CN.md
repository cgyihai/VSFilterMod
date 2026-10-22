# 编译 VSFilterMod

[English](BUILD.md) | **简体中文**

本项目编译出两样东西：

| 产物 | 说明 |
| --- | --- |
| `vsfiltermod.dll` / `.so` / `.dylib` | **CSRI** 字幕渲染插件，Aegisub 在所有平台上都以这种形式加载；同时也是 **AviSynth+** 插件（`TextSubMod` / `MaskSubMod`）。 |
| `vsfmrender` | 命令行工具，把字幕渲染成 **BDN+XML**（一个索引文件，加上每个事件一张 PNG）。 |

两者使用同一套渲染器源码，所以导出到 BDN+XML 的字幕与 Aegisub 预览的画面逐像素相同。

## 环境要求

* Meson ≥ 0.59 和 Ninja
* 支持 C++17 的编译器（GCC 9+、Clang 10+、MSVC 2019+）
* FreeType ≥ 2.8 和 libpng ≥ 1.6：系统里有就直接使用，没有就作为 Meson 子项目自动下载
* 仅 Linux/BSD：**fontconfig** ≥ 2.11
* macOS 和 Windows 分别使用 CoreText 和 DirectWrite，无需额外安装

## 编译

```bash
meson setup build
ninja -C build
meson test -C build
```

常用选项：

```bash
meson setup build -Dfont_backend=fontconfig   # 覆盖平台默认的字体后端
meson setup build -Dbdnxml=false              # 只编译插件
meson setup build -Dtests=false
```

### Apple Silicon 与 ARM Linux

光栅化器的混合内核是按 SSE2 写的。在 AArch64 上可以通过单头文件
[sse2neon](https://github.com/DLTcollab/sse2neon) 运行：

```bash
curl -LO https://raw.githubusercontent.com/DLTcollab/sse2neon/master/sse2neon.h
meson setup build -Dcpp_args=-I$PWD
```

没有它也能编译，渲染结果完全相同，只是改用较慢的标量内核。Meson 会打印实际选用的是哪一种。

## 安装到 Aegisub

### Linux / macOS

Aegisub 3.5.6 在这两个平台上默认不编译 CSRI 支持，有两处阻碍，
`docs/aegisub-csri-posix.patch` 都已修补：

* `meson.build` 只在 `host_machine.system() == 'windows'` 时启用该功能
* `subprojects/csri/meson.build` 从未定义 `CSRI_PATH`，导致 POSIX 枚举器
  （`lib/posix/enumerate.c`）无法编译——它的第一条语句就是
  `static const char csri_path[] = CSRI_PATH;`

```bash
cd /path/to/Aegisub
git apply /path/to/VSFilterMod/docs/aegisub-csri-posix.patch
meson setup build && ninja -C build
```

然后把插件放到搜索路径中的任意位置。编译时内置的默认路径是 `~/.csri` 和
`<prefix>/<libdir>/csri`，环境变量 `CSRI_PATH` 会追加在后面：

```bash
mkdir -p ~/.csri
cp build/vsfiltermod.so ~/.csri/        # macOS 上是 .dylib
chmod +x ~/.csri/vsfiltermod.so         # 见下方说明
```

> POSIX 枚举器会跳过 `access(path, X_OK)` 检查失败的文件，所以插件**必须**有执行权限。
> `meson install` 会自动设置；手动 `cp` 则不一定。

### Windows

Aegisub 的 Windows 枚举器在程序旁边的 `csri` 文件夹中查找插件：

```
Aegisub\
  aegisub.exe
  csri\
    vsfiltermod.dll
```

### 选用插件

重启 Aegisub，在 *选项 → 高级 → 视频 → 字幕渲染器* 中选择
**CSRI/vsfiltermod_textsub**。如果列表里没有，说明插件没找到或加载失败；在 Linux 和
macOS 上可以从终端启动 Aegisub，查看加载器的警告信息。

## 持续集成

`.github/workflows/ci.yml` 编译并测试六种配置：

| 任务 | 工具链 | 字体后端 | 依赖来源 |
| --- | --- | --- | --- |
| Linux x86_64 | GCC | fontconfig | apt |
| Linux aarch64 | GCC | fontconfig | apt + sse2neon |
| macOS arm64（`macos-15`） | Clang | CoreText | Homebrew + sse2neon |
| macOS x86_64（`macos-15-intel`） | Clang | CoreText | Homebrew |
| Windows x86_64（MSVC） | MSVC | DirectWrite | Meson 子项目，全部静态链接 |
| Windows x86_64（MinGW） | GCC | DirectWrite | MSYS2 |

每个任务都会运行 `csri-abi` 测试（按 Aegisub 加载器的方式加载插件并渲染），然后把
`tests/smoke.ass` 渲染成 BDN+XML，并用 `tests/check_bdnxml.py` 校验结果。校验器会拒绝
完全透明的图片，所以某个平台的字体查找如果悄悄失败，编译就会报错，而不是输出空白字幕。

两点说明：

* **ARM Linux 编译机只对公开仓库免费。** 如果仓库是私有的，请删除 `ubuntu-24.04-arm`
  这一项，否则该任务会一直排队，而不是报错。
* **可分发的是 MSVC 编译产物**：freetype、libpng、zlib 和 C 运行库全部静态链接，
  `vsfiltermod.dll` 是单个文件，只依赖 `DWrite.dll`、`KERNEL32.dll`、`USER32.dll` 和
  `ADVAPI32.dll`。MinGW 产物依赖 MSYS2 的 DLL，仅用于检查能否编译。

### 发布

推送以 `v` 开头的标签，会运行同样的编译矩阵，然后发布一个 GitHub Release，每个平台一个
压缩包（插件、`vsfmrender`、许可证和 README）：

```bash
git tag v1.0.0
git push origin v1.0.0
```

MinGW 版本不会发布，它只用于检查能否编译。

## 在 Aegisub 之外验证插件

`tests/csri_loader_test.c` 完全按 CSRI 加载器的方式加载动态库（按路径加载、按名字解析
每个入口函数），然后从内存解析一份字幕，请求 Aegisub 使用的像素格式并渲染一帧。
它已注册为 Meson 测试：

```bash
meson test -C build csri-abi
```

## 渲染 BDN+XML

```bash
build/vsfmrender --size 1920x1080 --fps 23.976 input.ass output.xml
```

在同一目录下写出 `output.xml` 和 `output_NNNN.png`。

事件列表与 avs2bdnxml 从原版 `MaskSubMod` 得到的一致：按
`(REFERENCE_TIME)(10000000 * n / fps)` 采样帧，并且像原版一样用单精度计算；相邻相同的帧
合并，空帧结束一个事件；像素使用 `MaskSubMod` 的反预乘公式。已用 avs2bdnxml-rs + 原版
VSFilterMod 和 `bdn-diff` 核对：`tests/smoke.ass` 的每个事件 InTC 和 OutTC 都相同。

`--fps` 接受 `分子/分母` 或小数；23.976、29.97 和 59.94 表示对应的 `/1001` 分数，这正是
AviSynth 传给 `MaskSub` 的片段帧率。如果你的脚本用的是别的帧率，请传入精确分数。
每一帧都按各自的时间单独渲染，与 `MaskSubMod` 相同：它的字幕源把每一行都报告为动画。

`vsfmrender` **不做**、而 avs2bdnxml 会做的事：调色板（8 位 PNG）输出、把一个事件拆成
两张图、PGS `.sup` 输出，以及强制 Y 坐标为偶数。它为每个事件写一张 RGBA 图片。

运行 `vsfmrender --help` 查看全部选项。

## AviSynth+

同一个库也是 AviSynth+ C 插件：

```
LoadPlugin("vsfiltermod.dll")
MaskSubMod("subs.ass", width=1920, height=1080, fps=24000.0/1001.0, length=34000)
# 或者叠加到视频上：
TextSubMod(clip, "subs.ass", charset=1, fps=-1, vfr="timecodes.txt")
```

`TextSub`、`MaskSub` 和 `TextSubModSwapUV` 也已注册，参数与原版相同。视频片段可以是
RGB32、RGB24、YUY2、YV12 或 I420；YUV 输出与原版一样，使用字幕 `YCbCr Matrix` 头指定的矩阵。

渲染流程逐步对应原版的 `CFilter::Render`——单精度时间戳、在内存字幕图层上清除上一次的
脏矩形、只合成实际渲染的包围盒、RGB32 和 RGB24 上下翻转而 `MaskSubMod` 的 RGBA 不翻转——
因为这些正是决定 avs2bdnxml 拿到什么像素的因素。已通过 avs2bdnxml-rs 和
`tests/avs_dump.c` 与原版核对：事件时间相同；`MaskSubMod` 的 RGBA，以及 `TextSubMod` 在
RGB32、RGB24、YUY2 和 YV12 上的字幕行相同（I420 与 YV12 共用代码路径，未单独测试）；
VFR 选帧相同。

AviSynth 不是链接时依赖。插件在加载时从加载它的 AviSynth 模块解析 C API，所以在没有安装
AviSynth 的机器上，同一个文件仍能作为 CSRI 插件加载。用 `-Davisynth=false` 编译可以去掉
这些入口函数。

## 与原版对比

`tests/csri_compare.c` 通过任意 CSRI 插件渲染字幕并导出原始画面；`tests/compare_raw.py`
用 `MaskSubMod` 的公式转换两份导出结果，报告覆盖区域 IoU、透明度差异，并生成并排对比图。
第一份指向 Aegisub 的 `csri/VSFilterMod.dll`，第二份指向本项目的编译结果。给原版的字幕
**必须带 UTF-8 BOM**：通过 CSRI 读取没有 BOM 的文件时，原版会抛出未处理的 C++ 异常。

`tests/frs.ass` 和 `tests/vertical.ass`（每秒一行，在 `n.5` 秒处取样）覆盖引擎复现
GDI 未公开排版规则的两个地方：各个角度的 `\frs`（含中日韩文字、`\fsp`、粗体、斜体、缩放
和换行），以及 `@` 竖排字体（含标点、拼音、希腊字母和全角字符）。它们用到宋体、黑体、
微软雅黑、等线、MS Gothic、Yu Gothic、Malgun Gothic 和 Times New Roman，请在装有这些字体
的机器上运行。与原版对比的 IoU 分别为 0.977–0.999 和 0.967–1.000。

`tests/fallback.ass` 覆盖缺字回退：Arial、Times New Roman、Tahoma 和 Segoe UI 中的
中日韩和韩文字符，含斜体、下划线删除线、`\fsp`、`\frz`、`\frs`，以及类似 SRT 的默认样式。
在中文版 Windows 11 上与原版对比，全部 11 行的 IoU 为 0.985–0.999（加入缺字回退前为
0.12–0.67）。`tests/bold.ass` 覆盖黑体、宋体和楷体的合成粗体，含斜体和 `\frs`，以及
`\frs` 下的粗体回退字形：0.977–1.000（之前为 0.93–0.95）。

## 删除了哪些内容

本项目是精简版的 VSFilterMod。DirectShow 播放专用的部分全部删除，因为 Aegisub 和
BDN+XML 输出都用不到：DirectShow 滤镜及其属性页、VobSub/DVB/HDMV/CC/USF/SSF 和
RealText 解析器、DirectX 字幕图层后端、`VobSub()`，以及 VapourSynth 插件。

保留的是真正重要的部分：完整的 VSFilterMod 标签集，包括扩展标签（`\pngimg` 图片背景、
渐变、可移动矢量 clip、抖动、distort、字符旋转、Z 坐标、竖向间距）。
