# 更新日志

[English](CHANGELOG.md) | **简体中文**

## 未发布

### 新渲染功能

- 新增字幕覆盖标签 \rgb：色差分离，把整句的红/蓝通道向相反方向错位。
  \rgb4 与 \rgb(4) 等价，支持 	 动画，\rgb0 关闭。

- 新增字幕覆盖标签 \glow。\glow20 与 \glow(20) 按 20 个输出像素半径绘制
  主体颜色光晕；\glow(20,1.5) 还可以设置线性强度倍率。效果包含主体和描边
  形状但不包含阴影，遵循字幕透明度/淡入淡出与裁剪规则，支持 \t 动画，
  \glow0 可关闭。半径和强度上限分别为 4096 个输出像素和 64。效果采用 bloom 式
  多尺度管线：三种半径的高斯近似在 16bit 线性光中加性累积，再经 Reinhard
  色调曲线压缩，光晕保持平滑渐变而不饱和成死白；强度相当于曝光，在色调
  映射前缩放光强。TypeSub
  标签元数据已加入该标签。回归测试覆盖语法等价、强度、颜色、透明度、
  \t 动画以及矩形/矢量/反向/移动矢量裁剪。

### 任意角度渐变

- \vgrad(angle)：在 \1vc 系列渐变激活时，把四角双线性插值替换为沿
  指定角度（度）的线性双色渐变。vc 的前两个颜色作为起止色；角度 0 从左
  到右，90 从上到下。支持 \vgrad45 简写与 \t 动画，去掉标签即恢复
  经典双线性混合。回归测试 tests/vgrad.py 验证方向、反向与语法等价。

### 径向渐变

- \rgrad(scale[,cx,cy])：在 \1vc 系列渐变激活时，颜色从中心点向外辐射：
  第一个 vc 颜色位于中心，沿线性距离过渡到第二个颜色，半径上限为中心
  到包围盒最远角距离的 scale 倍（CSS farthest-corner 语义）。cx/cy 以
  屏幕方向的 UV 比例定位中心（默认 0.5/0.5 即盒中心，允许超出 [0,1]）。
  \rgrad1 为简写，\rgrad0 关闭，支持 \t 动画；\vgrad 与 \rgrad 互斥，
  后写生效。回归测试 tests/rgrad.py 验证中心/边缘衰减、缩放、中心方向、
  语法等价、模式互斥与重置。

### 特效包

- \glowc&HBBGGRR& 与 \glowa&HAA&：可选的发光颜色与透明度覆盖。
  颜色替代默认的主要颜色发光，透明度遵循 ASS 约定（00 不透明、FF 透明），
  均支持 \t 动画并随样式重置。

- \iglow 内发光：\iglow8 与 \iglow(8[,strength]) 让光从字形边缘向内
  渗透，限制在字形内部并在主体之后合成。\iglowc/\iglowa 覆盖颜色
  与透明度（默认白色不透明）。\iglow0 关闭。

- \mblur(angle,length) 方向动态模糊：沿指定方向（角度、输出像素）
  拖尾整句位图，整数采样保持确定性。支持 \mblur45,10 简写。

- \bevel(size[,strength]) 平滑斜面浮雕：由字形覆盖率生成高光/阴影
  立体感。

- \clipf(radius) 羽化裁剪：按半径柔化矩形与矢量裁剪边缘（含反向
  裁剪），\clipf15 为简写。矢量裁剪蒙版在解析完成后统一模糊，所有
  绘制与发光路径自动继承柔边。

- 新增回归测试 tests/fxpackage.py 覆盖以上全部效果。

### 上游对齐

- 引入上游的非阻塞未知样式诊断：不同的缺失样式会合并为一条警告，不再用
  模态提示框报告。`vsfmrender` 会打印该警告，VapourSynth 滤镜会把它记录为
  warning；渲染结果不变。

## 1.4.0

### CSRI

- 新增：插件导出 `csri_typesub_tags_json()`——以静态 UTF-8 JSON 文档自我描述
  渲染器认识的扩展标签（`\1img`..`\4img`、`\1vc`/`\1va` 渐变色与渐变透明度、
  `\jitter`、`\distort`、`\mover`/`\moves3`/`\moves4`/`\movevc`、`\rnd`
  家族、`\z`、`\frs`、`\fsc`）。编辑器（TypeSub）加载插件后查询该符号并把
  标签表合并进自动补全，今后新增标签不再需要编辑器跟版。该符号是可选的：
  不认识它的宿主不受影响。

### 修复

- VFR 时间码读取器按 C 区域设置的 `%f` 解析帧率列：在小数点用逗号的系统上
  解析错误、帧计时漂移。现已改为区域无关解析。
- `csri_open_file`/`csri_open_mem` 合并为同一实例初始化路径；其余 POSIX
  移植与构建文档工作。

## 1.3.0

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
- 到达画面最后一行的 `\clip` / `\iclip` 行会越过裁剪遮罩末尾读取一行（堆越界读，
  原版的 bug，由 AddressSanitizer 下的模糊测试发现）。原版遮罩错开一行的行为保持不变，
  被裁剪的行外观与以前相同；在画面底边改用最后一行。
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
