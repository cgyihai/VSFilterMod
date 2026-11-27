# VSFilterMod — 跨平台版

[![CI](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml/badge.svg)](https://github.com/cgyihai/VSFilterMod/actions/workflows/ci.yml)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)

[English](README.md) | **简体中文**

把 VSFilterMod 字幕渲染器移植到 **Windows、Linux 和 macOS**。
编译产物是一个库，既是 Aegisub 用的 **CSRI** 插件，也是 **AviSynth+** 插件
（`TextSubMod` / `MaskSubMod`）和 **VapourSynth** 插件（`vsfm.TextSubMod`）；
另外还有命令行工具 `vsfmrender`，可直接把字幕渲染成蓝光制作用的 **BDN+XML**。

ASS 解析器和光栅化器都是 VSFilterMod 原版代码，所以全部扩展标签照旧可用：
`\pngimg` 图片背景、`\glow` 发光、渐变、可移动矢量 clip、`\rnd` 抖动、distort、`\frs` 字符旋转、
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
- **VapourSynth 插件**（API 4），`vsfm.TextSubMod` 的参数与原版一致：8 位输出与
  原版相同，9–16 位的 YUV 和 RGB 按完整精度混合，并支持 4:2:2 和灰度。
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
| Windows x86_64 | `vsfiltermod-windows-x86_64-msvc` | `vsfiltermod.dll`、`vsfmrender.exe` |
| Windows x86（32 位） | `vsfiltermod-windows-x86-msvc` | `vsfiltermod.dll`、`vsfmrender.exe`，用于 32 位的 Aegisub、AviSynth+ 和 avs2bdnxml |
| Windows ARM64 | `vsfiltermod-windows-arm64-msvc` | `vsfiltermod.dll`、`vsfmrender.exe` |
| Linux x86_64 | `vsfiltermod-linux-x86_64` | `vsfiltermod.so`、`vsfmrender` |
| Linux aarch64 | `vsfiltermod-linux-aarch64` | `vsfiltermod.so`、`vsfmrender` |
| macOS 通用 | `vsfiltermod-macos-universal` | `vsfiltermod.dylib`、`vsfmrender`，Apple Silicon 与 Intel 二合一 |
| macOS Apple Silicon | `vsfiltermod-macos-arm64` | `vsfiltermod.dylib`、`vsfmrender` |
| macOS Intel | `vsfiltermod-macos-x86_64` | `vsfiltermod.dylib`、`vsfmrender` |

所有版本都静态链接了同一固定版本的 FreeType 和 libpng，因此渲染结果完全相同。
Windows 版本无需额外 DLL。macOS 版本不依赖系统以外的任何库，支持 macOS 11 及以上
（Apple Silicon）和 10.15 及以上（Intel）。Linux 版本只依赖 fontconfig 和 zlib
（所有桌面发行版都有），支持 glibc 2.35 及以上（Ubuntu 22.04、Debian 12、Fedora 36
及更新版本）。每个正式版都附带 `SHA256SUMS` 校验文件。

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

任何通过 AviSynth+ 加载脚本的 avs2bdnxml 都能使用：32 位的 avs2bdnxml 或
AviSynth+ 请用 32 位（`windows-x86`）版本，否则用 64 位版本。

### VapourSynth

同一个库也是 VapourSynth 插件（API 4，VapourSynth R55 或更新版本），命名空间和参数
与原版相同：

```python
import vapoursynth as vs
core = vs.core
core.std.LoadPlugin("vsfiltermod.dll")      # 已放入插件目录时可省略
clip = core.vsfm.TextSubMod(clip, "subtitle.ass")
# TextSubMod(clip, file, charset=1, fps=..., vfr="timecodes.txt")
```

支持 4:2:0 或 4:2:2 的 YUV、灰度和 RGB，每个采样 8 到 16 位。YUV420P8 和 RGB24
的输出与原版滤镜完全相同。原版把 10 位和 16 位片段拆成两个 8 位的一半来绘制；这里每种
位深都按自身精度混合，因此 `accurate=` 仍可传入，但已不再需要。4:4:4 请先转换为 RGB。

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
| `--fontdir DIR` | 优先使用 `DIR`（含子目录）中的字体，先于系统已安装字体；可重复指定 |
| `--font-report` | 列出字幕用到的每个字体实际解析到了哪个字体文件 |
| `--strict-fonts` | 只要有字体被替换，就以退出码 3 失败 |
| `-q, --quiet` | 不显示进度 |

### 特效包（\glowc、\glowa、\iglow、\mblur、\bevel、\clipf）

\glow 的可选伴侣与四个新特效；全部支持 \t 动画，数值为 0 即关闭：

```ass
{\glowc&HBBGGRR&}    发光颜色覆盖（默认跟随主要颜色）
{\glowa&HAA&}        发光透明度覆盖（ASS 约定：00 不透明、FF 透明）
{\iglow8}            内发光半径 8：光从字形边缘向内渗透
{\iglow(8,1.5)}      带强度；\iglowc / \iglowa 覆盖颜色/透明度（默认白色）
{\mblur45,10}        方向动态模糊：45 度方向拖尾 10 个输出像素
{\bevel2}            平滑斜面浮雕，尺寸 2
{\clipf15}           羽化裁剪半径 15：矩形、矢量与反向裁剪均可
```

\iglow 在主体之后合成且严格限制在字形内部，不会外溢。\mblur 对成品
位图做确定性整数采样。\clipf 柔化本行裁剪：矩形裁剪向外扩展光栅区域
并在名义矩形外渐隐；矢量裁剪蒙版在解析完成后统一模糊一次，所有绘制与
发光路径自动继承柔边。回归测试 tests/fxpackage.py 覆盖全部特效。

### 渐变（\1vc、\vgrad、\rgrad）

经典的 \1vc/\2vc/\3vc/\4vc 标签在每个词上做四角颜色双线性插值。两个
修饰标签可以改造插值方式；两者都用 vc 的前两个颜色作为起止色，支持
\t 动画，且互斥——后写生效：

```ass
{\1vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\vgrad45}沿 45 度的线性渐变
{\1vc(&H0000FF&,&HFF0000&,&H0000FF&,&HFF0000&)\rgrad1}居中径向渐变
{\rgrad(1.5,0.5,0.2)}半径 1.5 倍，中心在 50% 宽度 / 20% 高度处
{\rgrad0}回到双线性四角混合
```

\vgrad(angle) 把字形包围盒投影到指定角度方向（0 从左到右，90 从上到
下）。\rgrad(scale[,cx,cy]) 从中心点向外辐射：第一个 vc 颜色位于中心，
沿线性距离过渡到第二个颜色，半径上限为中心到包围盒最远角距离的 scale
倍；cx/cy 以屏幕方向的 UV 比例定位中心，默认为盒中心。回归测试
tests/vgrad.py 与 tests/rgrad.py 固定了这些语义。

### 弧形文字

`\arc(radius[,angle])` 把文本行基线缠绕到以行锚点（定位行即 `\pos` 点）
为圆心、`radius` 输出像素为半径的圆周上。angle 为 0 时行中心位于圆顶，
字符沿顺时针排布：短文本呈彩虹拱形，长文本可绕满整圈（光盘效果）。
半径为正时文字在圆外侧，负值在内侧；`\arc120` 为简写形式，支持 `\t`
 动画，`\arc0` 关闭。排版按单字符拆分，拉丁连字/字距微调失效（中日韩
不受影响）；`\fsp` 字距沿弧照常生效。

### `\wave(amp,freq[,phase])` — 正弦波浪文字

将行基线缠绕到正弦波上：每个字形按前进位置做 `amp*sin` 垂直位移，
并旋转到波切线方向（枢轴为位移后的基线中点）。`amp`/`freq` 为输出像素
（`freq` 为完整波长），`phase` 为度数。`\wave0` 关闭；`\t`
可动画全部三个参数（动画 `phase` 即波浪流动）。与 `\arc` 同样逐字符
拆分（拉丁连字失效、CJK 不受影响），可与 `\arc` 及全部位图特效组合。

### 轨道遮罩（\matte、\imatte）

轨道遮罩用一个事件的剪影去遮罩其上方图层的事件，After Effects 风格。
遮罩事件自身隐藏；目标层上的事件完整渲染——主体、描边、阴影、辉光及
所有后处理特效——但只在遮罩不透明处可见：

```ass
Dialogue: 0,...,{\pos(320,180)}TEXT        可见底层
Dialogue: 1,...,{\pos(320,180)\matte}TEXT  隐藏遮罩副本，遮罩第 2 层
Dialogue: 2,...,{\pos(320,180)\1vc(...)\rgrad1}WWWWW  渐变只显示在 TEXT 内部
```

- `\matte` / `\matte1` / `\matte(1)` 遮罩 layer+1 层；`\matte2` 遮罩
  layer+2 层（应对稀疏层号）；`\matte0` 关闭。
- `\imatte` 反向：效果显示在遮罩外部。
- 同层多个遮罩事件并集为单一蒙版。
- 蒙版取自遮罩事件的渲染剪影（含阴影/描边/主体）；遮罩上的模糊或辉光
  会相应软化蒙版边缘。
- 遮罩与被遮罩事件各自按帧动画，\move/\t 两边保持同步。

回归测试 tests/matte.py 覆盖 containment、反向、层偏移、隐藏与语法变体。

### `\extrude(depth[,angle])` — 立体挤出

沿挤出方向以 1px 步长绘制阴影色副本（默认屏幕坐标 45 度），
形成实心侧面。颜色/透明度取自阴影样式（`\4c`/`\4a`），阴影深度
为零时也可独立使用。`\extrude0` 关闭；`\t` 动画 depth 与 angle。

### `\echo(count,dx,dy[,falloff])` — 残影拖尾

将完整渲染的事件按步进整数偏移逐副本合成，alpha 逐层衰减
（默认 0.5），主副本最后合成。glow/渐变/模糊等全部特效精确捕获。
count 上限 32。`\echo0` 关闭；`\t` 动画全部参数。事件级：
最后覆写块的参数应用于整行。

### `\pin(x1,y1,x2,y2,x3,y3,x4,y4)` — 四角钉扎透视

投影（单应）变形，将行贴合到任意四点四边形：行布局盒四角自左上
顺时针映射到四个给定的输出像素角点。文字可钉扎到倾斜的招牌、
屏幕或书封上，具有真实的透视缩短。扭曲叠加在所有其他变换之上
（旋转、弧形、波浪在其下组合）。`\pin0` 关闭；`\t` 动画全部
八个坐标。与遗留 `\distort`（三点双线性、相对包围盒）不同，
`\pin` 是绝对输出像素中的真透视。

### RGB 色差分离（\rgb）

\rgb 标签把文字的红、蓝通道向相反方向错位，产生色差/glitch 效果。以下
写法等价：

```ass
{\rgb4}文字
{\rgb(4)}文字
```

距离按输出像素计算，取负值可交换边缘色方向，支持 \t 动画，上限 4096，
\rgb0 关闭。效果作用于整句；当各词数值不一致时取绝对值最大者。

### 文字发光（\glow）

\glow 会在文字后面绘制柔和光晕。下面两种写法等价：

```ass
{\glow20}文字
{\glow(20)}文字
```

第二个数值是线性强度倍率（默认值为 `1`）：

```ass
{\glow(20,1.5)}更强的发光
{\glow0}关闭发光
```

半径按输出画面的像素计算，这一点与 \blur 相同。发光默认使用文字主体颜色；
渐变和图片填充会保留自身填充颜色，描边只提供发光形状，阴影不参与发光。
文字透明度、\fade/\fad、矩形/矢量/移动 \clip/\iclip，以及 \t 动画都会正确影响发光。
半径最大为 4096 个输出像素，强度倍率最大为 64。

发光采用 bloom 式多尺度算法：发射形状在 16bit 线性光管线中按三种半径
（紧密亮核、中段、宽软外晕）分别做高斯近似模糊并加性累积，再经 Reinhard
色调曲线压缩，使光晕保持平滑的亮度渐变，而不会饱和成一团死白云。强度
倍率相当于曝光：它在色调映射之前缩放线性光强。它仍然是 alpha 混合的
光栅效果，不是对 After Effects Deep Glow 的真正 HDR 加算复刻；独立的
发光颜色/透明度标签（例如 \glowc、\glowa）只作为将来可能的扩展保留，
当前尚未实现。

### 让每台机器用上相同的字体

字幕指定的字体如果本机没有安装，就会被平台自动替换——Windows、macOS、Linux
替换成的字体各不相同。三种方法可以避免：

- **把字体嵌入字幕**（`[Fonts]`，例如 Aegisub 的“附件”功能）。
- **共享一个字体文件夹。** 其中的字体与内嵌字体一样被查找，并且优先于已安装字体。
  vsfmrender 用 `--fontdir DIR`；Aegisub 和 AviSynth 则设置环境变量 `VSFM_FONT_DIRS`
  （多个文件夹在 Windows 上用 `;` 分隔，其他平台用 `:`）。
- **检查。** vsfmrender 渲染结束后会列出所有被替换的字体，加 `--strict-fonts` 则视为错误。
  Aegisub 和 AviSynth 中可设置 `VSFM_FONT_LOG=1`，在解析每个字体时打印结果。

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

**各平台之间的输出完全相同，而不只是接近。** 只要字体文件相同，Windows、Linux、
macOS 以及 x86-64 和 ARM 上的每个构建都会输出相同的像素：渲染不存在依赖 SIMD、CPU
或编译器的代码路径；CI 在每个平台上渲染 `tests/consistency/`，任何像素与共用的
`expected.txt` 不同都会失败。不同机器之间仍可能不同的，只有安装了哪些字体（见“已知问题”）。

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
不再安装到系统里。GDI 能用哪些名字找到字体，这里就能用哪些名字找到：Windows 家族名
（如“Source Han Sans SC Medium”）、排版家族名、完整名称，以及任何语言的本地化名称
（包括老式中日韩字体里用 GBK/Big5/Shift-JIS 编码的名称）。同一家族内嵌了多个字重时，
会像 GDI 一样按请求的字重和斜体选择对应的字体文件。内嵌可变字体的命名实例也按 GDI
的方式列出（如“Family Black”）。常规回退字体里都没有的字符，会先在内嵌字体（以及
`--fontdir` 文件夹）中查找，再交给平台自己的回退机制，因此在每台机器上结果相同。

**去掉了 DirectShow 部分。** DirectShow 滤镜及其属性页、VobSub/DVB/HDMV/CC/USF/
SSF/RealText 解析器和 DirectX 字幕图层后端都已删除，Aegisub、AviSynth 和 BDN+XML
输出都用不到它们。AviSynth 和 VapourSynth 中的 `VobSub()` 都没有移植。

**VapourSynth 插件基于 API 4**（原版用的是已淘汰的 API 3），高位深按完整精度混合，
不再把采样拆成两半。

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
- Linux 和 macOS 版本与 Windows 版本逐像素一致（由 CI 检查），但如果字幕指定的字体
  本机没有安装，各平台会各自替换成不同的字体。把字体嵌入字幕（`[Fonts]`）即可避免。

## 致谢与许可证

基于 Gabest（Guliverkli）的 **VSFilter** 和 **VSFilterMod**。`include/avisynth`
中的 AviSynth+ C API 头文件来自 AviSynth+ 项目；`include/vapoursynth/VapourSynth4.h`
来自 VapourSynth（LGPL 2.1）。

与原版相同，采用 [GNU 通用公共许可证 2.0 或更高版本](LICENSE)。
