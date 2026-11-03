# 更新日志

[English](CHANGELOG.md) | **简体中文**

## 未发布

### VapourSynth

- 新增：插件同时也是 VapourSynth 插件，`vsfm.TextSubMod(clip, file, charset, fps,
  vfr, accurate)`，标识符、命名空间和参数都与原版相同，基于 API 4（VapourSynth R55
  及更新版本）。
- YUV420P8 和 RGB24 的渲染结果与原版滤镜完全相同。9 到 16 位的 4:2:0 与 4:2:2 YUV、
  YUV422P8、灰度和高位深 RGB 按同样的算法以片段自身精度混合，不再像原版那样拆成两个
  8 位的一半；`accurate=` 仍可传入，但不起作用。
- CI 在 Linux、macOS 和 Windows 上把插件载入 VapourSynth，并用同一个
  `tests/vsplugin/expected.txt` 检查输出。
- AviSynth+ 与 VapourSynth 入口共用一套渲染流程（`src/vsfilter/sub_source.cpp`），
  AviSynth 的输出不变。

### 修复

- 被拉伸到几十万像素的字形（`\fscy10000000` 或样式的 ScaleY）在描边时可能耗费
  数分钟和数 GB 内存。现在宽或高超过 131072 像素的图形不再绘制（原上限为 2^20），
  描边扩展的计算量也设了上限，约为 1080p 全屏字形加 `\bord100` 的 45 倍。由模糊测试发现。
- 没有值的标头行（`YCbCr Matrix:`、`Collisions:`、`ScaledBorderAndShadow:`）
  会让解析器抛出异常并导致 Aegisub 崩溃（原版同样如此）。现在这些字段保持默认值。
- C++ 异常不会再传入宿主程序：CSRI、AviSynth 和 VapourSynth 入口都会捕获异常，
  抛出异常的格式解析器按语法错误处理。
- `tests/regressions/`：曾导致崩溃或卡死的字幕，每个都有单独的测试，并作为模糊测试的种子。
- `fuzz_render` 会按迭代编号报告耗时 5 秒以上的输入，`VSFM_FUZZ_DUMP=N` 可保存第 N
  次迭代的输入。

## 1.2.0

### 所有平台输出一致

只要字体相同，Windows、Linux、macOS（x86 与 ARM，32 位与 64 位）现在把每一条字幕
渲染成完全相同的像素。每次改动都会由 CI 在所有平台上检查。

- 禁止 FMA 合并（`-ffp-contract=off`）：ARM 构建对 `a * b + c` 的舍入与 x86 不同，
  会让动画行偏移不到一个像素。
- 字形变换只保留一条路径。原来的 SSE 与 C 两条路径在 `\distort` 下结果不同，
  `\rnd` 抖动的抽取顺序也不同。原版使用的近似倒数指令（RCPPS）在 Intel 与 AMD
  上结果不同，现在按 Intel 的结果逐位复现。
- 混合绘制只保留一套。原来 SSE2 与 C 两份已经分叉（`\1vc` 渐变、裁剪下的 `\kf`
  变色）。
- `sin`、`cos`、`exp`、`log`、`pow` 改用内置的逐位确定的实现，不再依赖各平台的
  C 库。
- `\c&H80FF00FF&` 等超范围数值在所有平台上都按 Windows（32 位 `long`）的方式解析；
  无论宿主程序设置了什么系统语言，小数点一律按 `.` 读取。
- 极端数值导致的有符号整数溢出按原版的方式回绕，不再是未定义行为。
- 字号的浮点转整数越界时按 x86 的行为处理。

### 字体

- `[Fonts]` 内嵌字体能用 GDI 认的所有名字找到：Windows 家族名、排版家族名、
  完整名称、各语言的本地化名称，以及老式中日韩字体里 GBK/Big5/Shift-JIS 编码的名称。
- 同一家族内嵌了多个字体文件时，按请求的字重和斜体选择（原来是最后一个生效）。
- 内嵌可变字体的命名实例按 GDI 的方式列出。
- 缺字回退时先查内嵌字体，再交给平台自己的回退。
- 新增共享字体文件夹：`vsfmrender --fontdir DIR`，Aegisub 与 AviSynth 使用环境变量
  `VSFM_FONT_DIRS`。
- 新增字体诊断：`vsfmrender --font-report`、`--strict-fonts`，以及 `VSFM_FONT_LOG=1`。

### 修复

- 格式错误的 SubViewer 风格行可能让解析器（以及 Aegisub）陷入死循环。
- Scroll 特效会错误地进入移动矢量裁剪的代码（原版 VSFilterMod 的 bug）。
- 读取未初始化的光栅化器标志；对负数进行未定义的移位。
- vsfmrender 判断相同帧时会逐像素比较，而不只比较哈希值。

### 构建

- 新增平台：Windows x86（32 位）、Windows ARM64、macOS 通用二进制（arm64 + x86_64）。
- macOS 版本自包含（FreeType 与 libpng 已静态链接），支持 macOS 11+（Apple Silicon）
  与 10.15+（Intel）。Linux 版本同样私有地静态链接 FreeType 与 libpng，支持
  glibc 2.35+（Ubuntu 22.04 及更新的发行版）。
- 所有平台使用同一个固定版本的 FreeType。
- 不再使用 sse2neon。
- 新增 `vsfmrender --version`；Windows 二进制文件带有版本信息。
- 发布包附带 `SHA256SUMS` 校验文件。

### 测试

- `tests/consistency`：覆盖各种 Mod 标签的文字与绘图测试用例，使用内置字体渲染，
  在每个平台上与同一个 `expected.txt` 逐像素比较。
- 用于字体查找测试的生成字体（`tests/consistency/fonts`）。
- AddressSanitizer / UndefinedBehaviorSanitizer CI 任务，以及针对字幕解析与渲染的
  模糊测试（`tests/fuzz_render.cpp`）。
