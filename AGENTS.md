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

## 弧形文字（patch m017，v1.10.0 / Windows v2.11.0）实现要点

- ParseString 在 arc 激活时逐字符拆分词；CWord::Append 必须拒绝合并 arc 词
  （否则 CLine::Compact 把单字符词重新并回整词——弧形完全失效的隐性 bug）。
- 旋转经 fontAngleZ 变异实现，m_arcApplied 防止三次绘制 pass 重复累加。
- **旋转枢轴 org 必须跟随 dx/dy 位移**，否则旋转绕陈旧点甩飞字形（位移小时
  碰巧正确，位移大才暴露）。
- 绘制位置空间是 eighth-pixels；org 即 \pos（eighth 单位）；圆心=行锚点。
- 缩进替换陷阱再验证：对 indent-8 子串替换会误伤 indent-12 行（子串包含），
  必须带上下文锚定。

## 特效包路线图（2025 更新）

已完成并发布：
- v1.6.0 / Windows v2.7.0：\glow bloom 发光、\rgb 色差、\glowc/\glowa 发光颜色
  与透明度覆盖、\iglow(/\iglowc/\iglowa) 内发光、\mblur(angle,length) 方向
  动态模糊、\bevel(size[,strength]) 平滑浮雕、\clipf(radius) 羽化裁剪（矩形/矢量/反向）
- v1.7.0 / Windows v2.8.0：\vgrad(angle) 任意角度线性渐变
- v1.8.0 / Windows v2.9.0：\rgrad(scale[,cx,cy]) 径向渐变
- v1.9.0 / Windows v2.10.0：\matte/\imatte 轨道遮罩
- v1.10.0 / Windows v2.11.0：\arc(radius[,angle]) 弧形文字
- v1.11.0 / Windows v2.12.0：\wave(amp,freq[,phase]) 正弦波浪文字
- v1.12.0 / Windows v2.13.0：\extrude(depth[,angle]) 立体挤出、\echo(count,dx,dy[,falloff]) 残影拖尾

测试：tests/fxpackage.py + vgrad.py + rgrad.py + matte.py + arc.py + wave.py +
extrude.py + echo.py；meson test 20 通过 1 跳过。

剩余候选（未做，按优先级）：
1. [DONE] \pin 四角钉扎真透视——见下方 m021 章节（v1.13.0/v2.14.0）。
2. [DONE] 多色渐变——见下方 m022 章节（v1.14.0/v2.15.0）。
3. [DONE] 分层羽化——见下方 m023 章节（v1.15.0/v2.16.0）。
4. 混合模式（加算/屏幕）——CSRI/子图合成 ABI 限制，工程量最大。
5. [DONE] 真透视——见下方 m024 章节（v1.16.0/v2.17.0，`\persp` 相机距离）。

特效实现要点（arc/wave/extrude/echo 共性）：
- arc/wave 逐字符拆分：ParseString 按码点切分（代理对保持完整），
  CWord::Append 在 arc/wave 激活时拒绝合并（Compact 会重并同样式词）。
- 旋转 Pivot 必须跟随位移，否则字形绕陈旧枢轴甩飞。
- extrude 是 shadow pass 纯 Draw 重入（光栅化缓存复用，无重复变换）；
  阴影深度为零时须跳过零偏移阴影绘制，否则抗锯齿边缘被阴影色污染。
- echo 走事件级离屏捕获（matte 的 tspd 重定向基建），CompositeEchoCanvas
  预乘 alpha 均匀缩放+整数偏移合成；matte 消费优先于 echo。
- CWord::Paint 非重入（m_fDrawn 缓存变换）；整数偏移副本只能 Draw 重入，
  亚像素偏移副本会有相位误差（extrude/echo 均整数化）。

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

## patch m021: 四角钉扎透视（v1.13.0 / Windows v2.14.0，已完成）

- `\pin(x1,y1,x2,y2,x3,y3,x4,y4)`：行布局盒四角（自左上顺时针）经单应矩阵
  映射到四个输出像素角点。跨平台 `fd01b9e` / Windows `bf07d13`。
- **关键单位发现**：路径点是 1/64 像素单位，绘制位置与行布局盒是 1/8 像素——
  Transform 调用里 `(org-p)*8` 的枢轴换算就是同一关系。扭曲内全部提升到 64ths。
- pin 角点用输出像素语义（同 glow 半径），排版师在视频帧上量角点。
- 扭曲在每词 m_fDrawn 缓存块内 Transform 后执行一次，无需防累积标志；
  行布局盒（p.x, p.y, m_width, m_ascent+m_descent，八分之一像素）在三个
  pass 注入点写入词样式。
- 解析器 `\pin` 必须位于 p 前缀分支之前。
- tests/pin.py 利用单应等变性（角点平移→墨水精确平移、矩形目标完全决定几何），
  无需预知布局盒。全套件 21/21 通过 1 跳过。
- **meson 测试批处理必须 `set PYTHONUTF8=0`**——中文 Windows 子进程输出是 GBK，
  UTF-8 模式下 meson 重配置因解码崩溃。

剩余候选：多色渐变、`\shadblur`/`\bordblur` 分层羽化、混合模式、真透视 3D。

## patch m022: 多色渐变（v1.14.0 / Windows v2.15.0，已完成）

- `\mgrad(c1,c2[,c3,...])`：2-8 个 ASS 十六进制色标沿当前渐变方向均布
  （vgrad 角度/rgrad 径向，未设置时水平）。跨平台/Windows 均已提交。
- **架构要点**：多色求值器只替换角度/径向分支的最终插值步骤，色标自动跟随
  当前激活的 t 参数化；两者都未设置时角度分支默认水平（cos1 sin0）。
- 色标存储用 vc 角色同款 RGB 反转字节序：((c&0xff0000)>>16)|(c&0xff00)|((c&0xff)<<16)。
- mgrad 会把 mode[0] 提升为渐变模式并播种样式 alpha（仿 vc 转换）。
- tests/mgrad.py：分带色相方向性、中间色标隆起、vgrad(90) 轴翻转、位精确禁用。
- Windows 移植零锚点失配——vgrad/rgrad 时代两树 getmixcolor 结构已完全同构。

剩余候选：`\shadblur`/`\bordblur` 分层羽化、混合模式、真透视 3D。

## patch m023: 分层羽化（v1.15.0 / Windows v2.16.0，已完成）

- `\shadblur(radius)`/`\bordblur(radius)`：阴影/描边层独立高斯羽化，
  与基础 `\blur` 加算。跨平台/Windows 均已提交。
- **架构**：Rasterize 公开可重入——各 pass 在 Paint 后以本层模糊级重光栅化
  覆盖层；扫描转换路径共享，仅最终模糊合成不同。层模糊任意激活时
  三个 pass 都重光栅化（body 用基础级），顺序无关正确。
- 解析器条目必须位于 shad/bord 前缀分支之前（arc/a、pin/p 同类冲突）。
- **转义陷阱重大教训**：文档生成脚本里 b 开头的标签（bordblur/blur）经
  python 字符串写入时 \b 变成退格符——且 bash 双引号层会再剥一层
  （退格替换退格=无操作）。**唯一免疫法：bytes([92, 98]) 字节数组构造**。
- 厚描边中心羽化后仍饱和——峰值软化断言只对薄特征有效（测试用 bord2）。

剩余候选：混合模式（ABI 限制）、真透视 3D（pin 单应基建已备）。

## patch m024: 真透视相机距离（v1.16.0 / Windows v2.17.0，已完成）

- `\persp(dist)`：frx/fry 旋转投影的用户相机距离（输出像素）。
  跨平台/Windows 均已提交。
- **关键发现**：遗留 Transform 本就有 3D 旋转+投影除法，只是相机距离写死
  （m_scale*·20000）。实证 fry60 宽度比 0.744 反推几何距离 ≈312px，
  与 2500/8=312 精确吻合——**投影数学在 1/8 像素单位域**，换算
  zoomf = dist·64·m_scale*。使距离可控即真透视。
- 宽度定律（回归测试解析验证）：width/W = cos(t)·d^2/(d^2-(W/2·sin(t))^2)。
  小距离时近端扩张主导→宽度反而增大；d→∞ 逼近正交极限 cos(t)。
- MaxSs(z3+zoomf, 1000) 钳制防止穿相机平面翻转——距离小于行宽一半时
  产生压缩伪影而非翻转，文档已注明。
- **Windows 特有陷阱**：Transform 有 SSE2 向量化路径，zoomf 存在标量与
  __m128 两份初始化——必须改为读取同一可变标量，否则 SSE 路径透视失效。
- 解析器 persp 条目与 pin 一样必须位于 p 前缀分支之前。

剩余候选：~~混合模式~~（已否决，用户决议）：libass#347 证实渲染器看不到视频像素则
  渲染期混合结构上不可能；CSRI ABI 只输出 RGBA，预览必然错误（违反 WYSIWYG），
  Windows DirectShow 单树可做但破坏两树对等——投入产出比不成立。

## patch m025: 行级图像纹理与 \imgscale（v1.17.0 / Windows v2.18.0，已完成）

- `\1img`-`\4img` PNG 填充改为行级屏幕坐标采样，纹理跨词连续；新增
  `\imgscale(sx[,sy])`，在环绕平铺前除算采样坐标。两树均已移植。
- **坐标陷阱**：Rasterizer::Draw 的 `xsub/ysub` 形参实际是词绘制的
  绝对屏幕位置（1/8 像素），不是单纯亚像素相位；`mOffsetX/Y` 在
  Rasterize 时扣除了低 3 位相位。连续 overlay 原点必须写成
  `xsub + mOffsetX + (xsub & 7)`（y 同理），才能恢复 `CWord::Paint`
  传入的完整 1/8 像素屏幕偏移；已取整的 `x/y` 只能用于裁剪，不能作为
  纹理坐标原点。不要按 legacy `subpixx/subpixy` 的采样方向“修正”符号。
- 裁剪补偿：`getmixcolor` 函数体开头已有 `tx += xoffset`，mode==2 内不得
  重复加；垂直方向因 `ty` 是自底向上的行索引，顶部裁剪不需要加 `yoffset`，
  底部裁剪仍由既有 `clipDiff` 补偿。重复加会导致矩形裁剪恰好平移纹理。
- 采样器现在用 double 连续坐标统一环绕+双线性插值；纵向仍按 PNG 倒置行
  读取。`imgScaleX/Y` 是持久样式状态，`imgLine*`/`imgBase*` 是逐 pass
  渲染状态，必须参与 clear/相等比较但不要让渲染状态回流到样式重置。
- `\imgscale`：sy 省略等于 sx；0/非法输入恢复 1:1；负值取绝对值；
  钳制 [1/256,256]；支持 `\t`。legacy `\1img(file,xoffset,yoffset[,angle])`
  参数顺序不变。
- tests/img.py 验证跨词连续性、行锚定移动、等比/各向异性缩放、0/负/非法
  值、legacy xoffset、矩形裁剪稳定性与确定性。跨平台全套件 25 通过 1 跳过；
  Windows `Release (MOD)|x64` 解决方案构建通过。

## patch m026: 输出不变的图像/特效性能与内存优化（v1.18.0 / Windows v2.19.0，已完成）

- PNG 所有权：`MOD_PNGPIXELS` 由行指针表 `rows` 加单块连续像素 `data`
  组成，`MOD_PNGIMAGE::pixels` 以 `shared_ptr` 共享所有权；`pointer` 只是
  指向行表的非 owning alias。样式、词副本与 `mod_images` 缓存可以安全共享
  同一解码图像，最后一个引用销毁时才释放。`freeImage()` 只放弃当前引用，
  绝不能恢复逐行 `free()`。
- `mod_images` 两树均为 `std::vector<MOD_PNGIMAGE>`。不要改回 Windows 原生
  `CAtlArray<MOD_PNGIMAGE>`：ATL `GrowBuffer` 用 memmove 搬移元素并明确不调用
  旧元素析构，形式上不适合含 `shared_ptr` 的非平凡对象。两树
  `CSimpleTextSubtitle::Empty()` 都必须 `mod_images.clear()`。
- 内嵌 PNG 解码入口是带长度的 `initImage(BYTE*, size_t, CString)`；
  `MOD_PNGMEMSRC` 保存当前指针与 end 指针，libpng 回调越界即报错。不要恢复
  旧的无长度游标，否则截断 PNG 会越界读宿主字幕内存。libpng 错误经 longjmp
  跳过局部 C++ 析构，所以 `png_read_image` 前所有权必须已经转移到成员
  `pixels`，setjmp 错误路径再 `freeImage()`。
- 图像采样缓存只在 `Rasterizer::Draw` 确定当前 clipped overlay 后准备。
  横向缓存必须使用已加过一次 `xoffset` 的列坐标，纵向缓存必须使用已加
  `clipDiff` 的行坐标；每个缓存元素都必须保留与原逐像素路径完全相同的
  double 表达式。`prepareImageTextureCache` 先清空四通道缓存，不要把缓存
  跨 overlay、跨图像实例或跨帧复用。
- 快速路径只允许在 `fx == 0` 或 `fy == 0` 的精确零权重时删除对应项；
  这些项是精确 +0，删除不改变 IEEE 结果。通用双线性分支的表达式顺序
  `c00*w00 + c10*w10 + c01*w01 + c11*w11` 不得重排，不得改 float、FMA、
  fast-math、固定点或近似插值。
- 跨平台 8 个、Windows 16 个 `Draw_Grad_*` helper 使用 `MOD_GRADIENT&`，
  不能恢复按值拷贝：拷贝会连同四个通道的坐标缓存一起复制，抵消优化。
  helper 本身不修改该对象。
- matte/echo/glow scratch 复用只复用容量，不携带语义状态：每个事件/平面
  使用前都会 `assign/clear` 重置。matte pool 必须是
  `std::vector<std::unique_ptr<MatteScratchBuffers>>`，因为 `MatteLayerState`
  持有 pool 内部 vector 的指针；若改成直接存放对象的 vector，扩容搬移会使
  指针失效。事件合成必须在下一次复用同一 event scratch 前完成。
- scratch 释放策略：`ReleaseRenderingScratch()` 在 Deinit 与无字幕段时释放；
  Render 末尾若本帧未使用 event/glow scratch 则释放对应容量，matte pool 收缩到
  本帧实际使用数。这是内存驻留与分配复用的折中，不影响像素语义。
- Windows 移植补齐了 `\imgscale2` 紧凑后缀归一化；该块必须位于 `\i` 前缀
  分支之前。跨树 CSRI 对比曾发现 Windows 忽略紧凑形式而括号形式正常，
  这类解析差异不能仅靠跨平台测试发现。
- 验证：7 个图像/特效场景与优化前 manifest 位级一致；跨平台 Meson 25 通过、
  0 失败、1 个 VapourSynth 环境测试跳过；img-base/img-scale2/img-clip/
  img-glow/img-iglow/img-echo/img-fx 七个跨树 CSRI 帧逐字节一致；Windows
  `Release (MOD)|x64` 解决方案构建通过。
- 开发压力探针（640x360、20 个重叠全帧矢量事件、3 次取中位数，含进程启动
  与 PNG 输出开销）约：`\1img` 2.68 s → 0.61 s，`\imgscale2` 2.73 s →
  0.91 s；组合特效约 2.91 s → 2.72 s。数值随机器/构建波动，只能说明图像
  采样路径大致数倍提速，不能当作用户可见渲染管线的严格 benchmark。
- 内存探针（1024x1024 RGBA 内嵌 PNG，125 次 open/close）：最终跨平台 post-warm
  增长约 16 KiB 且平台期稳定，Windows 约 40 KiB 平台期；持有 10 个实例约增加
  42 MB（预期约 40 MiB 解码图像加实例开销），关闭后回到距基线几十 KiB。
  结论是图像所有权已释放；单次 RSS 残余属于分配器/运行时驻留，不能脱离
  重复 open 平台期直接解释为泄漏。截断 PNG 错误路径另行跑过 505 次 open/close，
  Windows RSS 在首批后停于约 1.8 MiB 平台期而不是随次数增长（跨平台 105 次
  约 16 KiB），说明 longjmp 错误清理没有持续泄漏像素块。

## patch m027: 编辑器标签元数据文件（v1.19.0 / Windows v2.20.0，已完成）

- 构建产物新增 `vsfiltermod-tags.json`：内容与 `csri_typesub_tags_json()`
  导出的 UTF-8 JSON 逐字节一致，但以纯文件形式落在输出目录（cross 在
  build 根目录，Windows 在 DLL 旁）。编辑器两个通道任选其一实现一次即可，
  以后加标签编辑器零改动。
- **架构**：csriapi.cpp 内嵌的 raw string JSON 仍是唯一权威来源；
  `tools/extract_typesub_tags.py`（两树同步同一文件）构建时提取 payload、
  json.loads 校验、结构校验（version=1/renderer/tags、name/signature/help
  非空、无重名），**失败即构建失败**——损坏的清单绝不能静默发布。
- **字节一致契约**：文件按 raw string payload 原样二进制写出，但先做
  CRLF→LF 规范化——C++ translation phase 1 会把 raw string 内的行尾序列
  统一映射为 `\n`，编译器嵌入的永远是 LF 形式，与 cpp 文件本身行尾无关。
- **重大编码教训**：Windows 树 csriapi.cpp 无 BOM，MSVC 默认按系统代码页
  （GBK）解读源文件，JSON 中文大多数字节对碰巧往返还原，但"于""）"等字
  在 DLL 导出中损坏为 `?`——v1.4.0 引入该导出时就存在的预存 bug。
  cross 树不受影响是因为 meson 编译带 `/utf-8`。修复：vcxproj 给
  csriapi.cpp **单文件**加 `/utf-8`（不要全局加，避免改变其他文件既有
  字面量的 execution charset 语义）。是 typesub-tags 端到端测试抓到这个
  bug 的——文件与 DLL 导出字节比对值得永远保留。
- **测试** tests/typesub_tags.py：文件==cpp payload 字节、结构校验、
  ctypes 加载插件比对导出==文件（Windows 需 os.add_dll_directory 加
  build root + subprojects 各一级子目录解析 freetype/libpng 依赖；
  目录必须是绝对路径；加载失败返回 77=skip）。meson 全量 26 通过 1 跳过。
- **构建集成**：meson 用 custom_target（input=csriapi.cpp，build_by_default，
  无 python 时 warning 跳过）；MSBuild 在 4 个 ItemDefinitionGroup 加
  PostBuildEvent 调 `tools\gen_typesub_tags.cmd`（python→py 探测链，
  找不到 python 警告退出 0 不阻塞构建，提取失败 exit 1 失败构建）。
  OutDir 带空格括号（`Release (MOD)`），传参必须加引号，`%~2` 去引号。
- **转义陷阱再次命中**：用 python heredoc 给 vcxproj 写 `..\..\tools` 时
  `\t` 变成 tab 字符写入 XML——chr(92) 字节拼接是唯一免疫法，写完必须
  扫描控制字符验证。/tmp 在本机 bash 是 MSYS 路径而 Windows python 的
  os.environ TEMP 是 Windows 路径，写构建 bat 要显式用 C:\Users\...\Temp。


## v1.19.1 / Windows v2.20.1.0：POSIX 路径分隔符修复（已完成）

- **CI 全红根因**：RTS.cpp 对 `\1img`–`\4img` 的 fpath 计算只用
  `m_path.ReverseFind('\\')`——POSIX 下返回 -1、fpath 为空、相对文件名
  退化为进程工作目录→PNG 加载失败→mode[i] 不为 2→纯色填充→img 测试
  全部断言失败。修复：取 `\\` 和 `/` 两者的较大索引。测试把 tex.png 写在
  workdir、但 vsfmrender 的 CWD 是 meson build 目录，不同目录才暴露此 bug。
  教训：跨平台树的所有路径拆分不能只找 `\\`。
- 类似隐患位置已查：cross 树仅剩 param.Find('\\') 一处（标签参数解析非路径）。
- **MinGW 警告无害**："rsrc merge failure: duplicate leaf: type: 10
  (VERSION) name: 1 lang: 409" 是 binutils ld 合并 .rsrc 节时的警告，
  链接仍成功且测试正常跑——不用追。
- **ASan 下 ctypes 不可达**：带插桩的 .so 被 dlopen 进未插桩的 python 时
  ASan runtime 直接 abort，进程级退出、ctypes 无法捕获。typesub_tags.py
  现在检测到 ASAN_OPTIONS/MSAN_OPTIONS/LSAN_OPTIONS 时跳过端到端检查
  （文件==cpp 字节与结构校验仍跑，返回 0 非 77）。
- **同一陷阱第三次命中**：python heredoc 里 `\1`/`\4`/`\`` 都被吃掉成
  控制字符。本节的教训：写含 `\tag` 字面量的中文段落必须用 edit/write 工具。

## 候选史诗 2/3 备注
- 打字机/逐字显现：\k 家族已覆盖逐字填充；真打字机（逐字出现+光标）
  语义上是新功能，需求证据不足，优先级降档。
- 收尾项（随时可做）：更广泛的性能 profile、更多标签的两树渲染输出位级
  交叉验证、csriapi 元数据完整性审计、README 示例图补全。
