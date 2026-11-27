# VSFilterMod cross-platform — 开发备忘

## 构建与测试（Windows + MSVC）

- 不要复用旧的 `build/`（指向迁移前的旧源路径）。用 `build-glow/` 或新建目录配置。
- 构建：
  ```bat
  call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  set "PATH=%APPDATA%\Python\Python315\Scripts;%PATH%"
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

已完成并发布（v1.6.0 / Windows v2.7.0）：
- \glow bloom 发光、
gb 色差
- \glowc/\glowa 发光颜色与透明度覆盖
- \iglow(/\iglowc/\iglowa) 内发光
- \mblur(angle,length) 方向动态模糊
- evel(size[,strength]) 平滑浮雕
- \clipf(radius) 羽化裁剪（矩形/矢量/反向）

测试：tests/fxpackage.py 覆盖全部特效；meson test 13 通过 1 跳过。

剩余候选（未做）：径向/任意角度渐变、混合模式（加算）、真透视 3D、轨道遮罩。

移植注意：Windows 树用 wcstod/wcstol/cos/sin/pow 替代 vsfm::WcsTo*/Det*；
提取跨平台补丁块时注意注释标记歧义（"patch m014. inner glow" 在解析器
前缀链与渲染循环各出现一次，提取必须带第二行上下文）。

## 下一个史诗：任意角度/径向渐变（已分析，未实施）

现有渐变架构（patch m004）：
- `MOD_GRADIENT`（STS.h ~L87）：`color[4][4]`/`alpha[4][4]` 存四角颜色，
  `mode[i]` 1=双线性四角插值、2=PNG 背景；`width/height` 是字形包围盒。
- 渲染时 `getmixcolor(tx, ty, i)`（STS.cpp ~L3866）逐像素算混合色，
  在 Rasterizer.cpp 的 6+ 个 pixmix 调用点被调用（~L1176-1283）。
- 解析标签 `vc`/`vc`/`va` 等在 RTS.cpp ParseSSATag。

设计方案（供实施者参考）：
- 新标签建议 `vg(angle)` 或给 vc 加角度参数——注意与现有 vc 语义兼容。
- 实现要点：在 mode[i]==1 分支前加 angle 模式（建议 mode=3），把双线性
  权重替换为沿角度方向的投影 t=(x*cos+y*sin)/len，两端颜色插值。
- 必须用 det_math 的 DetCos/DetSin 保持跨平台确定性；Windows 树用 cos/sin。
- 渐变的 width/height 在 RTS.cpp L1646 附近随 subpix 一起设置。
- 移植注意：两树的 Rasterizer.cpp pixmix 调用点结构一致但行号不同。

其他候选史诗（均未做，各需完整会话）：
- 混合模式（加算/屏幕）——需改 CSRI/子图合成架构，工程量最大
- 真透视 3D（rxry 目前是正交近似）——研究级
- 轨道遮罩——语法设计+实现都复杂

## 旧版路线图存档
（用户已批准，按序逐个实现）

语法原则：一个数字搞定默认效果，括号形式加进阶参数，0 关闭（与 `\glow` 一致）。

1. `\iglow<radius>` / `\iglow(radius[,strength])` — 内发光。注意：默认主体色在实体填充上几乎不可见，需要设计颜色语义（候选：默认用白色/描边色/反色，或 `\iglow(radius,strength,color)`；合成必须在 body 之后且限制在字形覆盖率内部；强度=曝光）。
2. `\clipf<radius>` — 羽化裁剪。架构侵入最深：矩形 clip 目前是硬边裁剪不经蒙版，需要把 clip 形状光栅化成蒙版+扩大光栅化 clipRect+模糊蒙版；矢量 clip 已有 6-bit 覆盖率蒙版可复用。
3. [DONE] `\rgb<distance>` — RGB 分离/色差。已完成：整句通道偏移、句内取最大绝对值、tests/rgb.py 回归测试通过（质心法验证通道分离方向和距离）。教训：ASS `\r` 样式重置标签的前缀匹配会吞噬 r 开头的新标签，新标签解析必须置于 `\r` 分支之前（见 RTS.cpp 中 glow/rgb 前缀块位置）。
4. `\mblur(angle,length)` — 方向模糊。
5. `\bevel<size>` — 斜面浮雕。
6. 径向/角度渐变、混合模式（additive）、真透视 3D、轨道遮罩（第三档，各需独立评估）。
