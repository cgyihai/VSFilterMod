# VSFilterMod cross-platform — 开发备忘

## 构建与测试（Windows + MSVC）

- 不要复用旧的 `build/`（指向迁移前的旧源路径）。用 `build-glow/` 或新建目录配置。
- `set PYTHONUTF8=0` 必须：Python 3.15 默认 UTF-8 模式，而 MSVC lib.exe 输出 GBK 中文，
  meson 重配置按 UTF-8 解码失败（detect_static_linker AttributeError: NoneType）。
- 构建：
  ```bat
  call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  set "PATH=%APPDATA%\Python\Python315\Scripts;%PATH%"
  set PYTHONUTF8=0
  meson compile -C build-glow
  meson test -C build-glow --print-errorlogs
  ```
- 手工运行 `vsfmrender.exe` 需要 PATH 包含：`build-glow`、`build-glow/subprojects/freetype2`、`build-glow/subprojects/libpng-1.6.55`（DLL 依赖）。
- BDN+XML 输出的 PNG 是 **straight alpha（非预乘）**：合成公式是 `src*a + dst*(1-a)`。
- Windows-only 版构建：必须用解决方案 `VSFilterMod.sln`（不能单独构建 VSFilter.vcxproj），配置 `Release (MOD)|x64`，MSBuild 在 `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe`。

## 仓库约定

- 本目录对应 GitHub `cgyihai/VSFilterMod`；`../VSFilterMod_Only-Windows` 是独立嵌套 git 仓库（Windows 专属），两边特效/修复需同步移植，跨平台版解析用 `vsfm::WcsToD`，Windows 版用 `wcstod`。
- cross-platform 目录目前没有自己的 `.git`，git 命令会落到外层仓库（外层看到"删除+未跟踪"状态，提交前需先整理）。
- 特效标签移植锚点一致：`mod_glowRadius/mod_glowStrength` 在 STS.h/STS.cpp（默认值、相等比较、拷贝三处）、RTS.cpp（解析+渲染）、csriapi.cpp（TypeSub 元数据）。

## glow 实现要点（改特效前必读）

- 16-bit 线性光平面 + 32-bit 累积；三种半径高斯近似（r/6 权重0.7、2r/5 权重0.45、r 权重0.35，Q8）加性叠加；Reinhard 色调映射肩部 0.15×满量程；strength 是曝光（色调映射前缩放）。
- 调参锚点：RTS.cpp 中 `scaleRadius`/`scaleWeight`/`shoulder`。
- 渲染器像素内部是反转 alpha（pixmix 语义），vector clip 蒙版是 6-bit 覆盖率（0..64），`GlowClipAlpha` 保留了历史 y+1 行采样，不能"修正"。

## 特效包路线图（2025 更新）

已完成并发布：
- v1.6.0 / Windows v2.7.0：\glow bloom 发光、\rgb 色差、\glowc/\glowa 发光颜色
  与透明度覆盖、\iglow(/\iglowc/\iglowa) 内发光、\mblur(angle,length) 方向
  动态模糊、\bevel(size[,strength]) 平滑浮雕、\clipf(radius) 羽化裁剪（矩形/矢量/反向）
- v1.7.0 / Windows v2.8.0：\vgrad(angle) 任意角度线性渐变
- v1.8.0 / Windows v2.9.0：\rgrad(scale[,cx,cy]) 径向渐变
- v1.9.0 / Windows v2.10.0：\matte/\imatte 轨道遮罩

测试：tests/fxpackage.py + vgrad.py + rgrad.py + matte.py；meson test 16 通过 1 跳过。

剩余候选（未做）：混合模式（加算）、真透视 3D。

移植注意：Windows 树用 wcstod/wcstol/cos/sin/pow 替代 vsfm::WcsTo*/Det*；
提取跨平台补丁块时注意注释标记歧义（"patch m014. inner glow" 在解析器
前缀链与渲染循环各出现一次，提取必须带第二行上下文）。

**文档编辑教训**：用 python heredoc 写含反斜杠的文档时，\1 \r \v \t \b \f 会被
Python 转义成控制字符（本文件和 README/CHANGELOG 都被污染过）。安全做法：
edit/write 工具直写，或 python 里用 chr(92) 拼接。写完用控制字符扫描验证。

## 渐变实现要点（vgrad/rgrad，改渐变前必读）

架构（patch m004 + m015 + m016）：
- `MOD_GRADIENT`（STS.h）：`color[4][4]`/`alpha[4][4]` 四角颜色（顺序 [TL,TR,BL,BR]），
  `mode[i]` 1=双线性四角插值、2=PNG 背景；渲染用的 `width/height/xoffset/yoffset`
  是逐词 overlay 位图尺寸与裁剪偏移，在 Rasterizer.cpp ~L1430 设置（不是 RTS.cpp）。
- `getmixcolor(tx, ty, i)`（STS.cpp）逐像素算混合色，在 Rasterizer.cpp 的 6+ 个
  pixmix 调用点被调用；rgrad 分支在 vgrad 分支之前，两者都在 mode[i]==1 内。
- 坐标系：`tx += xoffset` 后是 overlay 空间；**ty 轴原点在屏幕底部**（向上递增），
  所以 radialCy（屏幕 UV，0=顶）必须翻转为 `(1-cy)*height`；水平方向不翻转。
  该结论由 tests/rgrad.py 中心偏移测试实证钉死，不要"修正"。
- `sqrt()` 是 IEEE-754 正确舍入运算，跨平台位一致，无需 det_math 包装；
  sin/cos/pow 才需要 DetCos/DetSin/DetPow（Windows 树用 libm 对应物）。
- \vgrad 与 \rgrad 互斥（后写赢）：解析时启用一个清除另一个；裸标签各自从 org
  重置自己的字段，不触碰对方。radialScale 钳制 [0,64]，cx/cy 不钳制（允许盒外中心）。
- \r 前缀吞噬教训同样适用于 rgrad：前缀链必须置于 `\r` 分支之前（rgb 旁）。
- 省略的可选参数回默认值而非当前值（bevel/mblur 惯例）：\rgrad(2) 会把 cx/cy 重置为 0.5。

其他候选史诗（均未做，各需完整会话）：
- 混合模式（加算/屏幕）——需改 CSRI/子图合成架构，工程量最大
- 真透视 3D（\frx\fry 目前是正交近似）——研究级

## 轨道遮罩实现要点（matte/imatte，patch m016，改遮罩前必读）

语义：`\matte[N]`/`\imatte[N]` 把事件标记为隐藏遮罩源，遮罩 layer+N（默认+1）
层上的事件；0 关闭。事件级标志存在 `CSubtitle::m_matteMode/m_matteOffset`
（不是样式属性，不支持 \t 动画；事件内容本身照常动画）。

架构核心——**tspd/tbbox 重定向**（RTS.cpp Render 循环）：
- 事件绘制全部参数化经过 spd（PaintShadow/Outline/Body、辉光/内辉光合成、
  bevel/mblur/rgb 后处理都读写 spd）。循环开头 `SubPicDesc tspd = spd` 是
  结构体拷贝，matte/被遮罩事件只改 `tspd.bits/tspd.pitch` 指向离屏画布；
  尺寸完全相同所以一切维度读取不受影响，正常路径位完全一致（render-consistency 证实）。
- `CRect& tbbox` 三元引用：重定向事件用局部 localBBox，正常事件别名共享 bbox2
  （保留跨事件累积的历史语义，后处理区域依赖它，不能"修正"）。
- **循环体内 spd/bbox2 已全部机械替换为 tspd/tbbox**；新增使用帧缓冲的代码
  必须用 tspd/tbbox。警告：`\bspd\b` 正则替换会误伤 `mod_vc.spd` 成员访问，
  移植时手工恢复过一处。
- matte 事件：绘制进按层共享的持久画布（matteLayers map），每次事件后重新
  提取覆盖率（提取是画布状态的纯函数，同层多事件自然并集）。自身不可见。
- 被遮罩事件：绘制进 per-event 临时画布，结束后 `CompositeMattedCanvas`
  按 6-bit 覆盖率逐像素调制合成进真帧。合成是预乘 alpha 的 over：
  蒙版同时缩放 rgb 贡献与不透明度（预乘下保色）；反转 alpha 通道乘法累积。
- 层序：LSub 升序自底向上单遍迭代，matte 在低位层先绘制，target=layer+N
  （N>=1）保证先产后用——**负/零 offset 会破坏时序所以解析时钳掉**；
  无需预扫描（AllocRect 有副作用不能重复调用）。
- 覆盖率提取：反转 alpha 画布 opacity=(0xff-alphaByte)，cov=(opacity+2)>>2
   cap 64；inverse 存 64-cov。格式与矢量 clip 蒙版一致。
- 为何不复用 pAlphaMask 蒙版通道：glow 等后处理绕过蒙版合成，只有离屏
  画布+调制合成能正确捕获事件全部输出。
- `\imatte` 前缀链必须置于 `\i`（斜体）分支之前；`\matte` 与 move/mover/mblur
  无前缀冲突。测试陷阱：文本默认带黑色描边，效果事件的描边会出现在遮罩内
  （正确行为）；反向遮罩的洞里会透出下层基底——写断言前先想几何。

## 旧版路线图存档
（用户已批准，按序逐个实现）

语法原则：一个数字搞定默认效果，括号形式加进阶参数，0 关闭（与 `\glow` 一致）。

1. `\iglow<radius>` / `\iglow(radius[,strength])` — 内发光。注意：默认主体色在实体填充上几乎不可见，需要设计颜色语义（候选：默认用白色/描边色/反色，或 `\iglow(radius,strength,color)`；合成必须在 body 之后且限制在字形覆盖率内部；强度=曝光）。
2. `\clipf<radius>` — 羽化裁剪。架构侵入最深：矩形 clip 目前是硬边裁剪不经蒙版，需要把 clip 形状光栅化成蒙版+扩大光栅化 clipRect+模糊蒙版；矢量 clip 已有 6-bit 覆盖率蒙版可复用。
3. [DONE] `\rgb<distance>` — RGB 分离/色差。已完成：整句通道偏移、句内取最大绝对值、tests/rgb.py 回归测试通过（质心法验证通道分离方向和距离）。教训：ASS `\r` 样式重置标签的前缀匹配会吞噬 r 开头的新标签，新标签解析必须置于 `\r` 分支之前（见 RTS.cpp 中 glow/rgb 前缀块位置）。
4. `\mblur(angle,length)` — 方向模糊。
5. `\bevel<size>` — 斜面浮雕。
6. 径向/角度渐变、混合模式（additive）、真透视 3D、轨道遮罩（第三档，各需独立评估）。
