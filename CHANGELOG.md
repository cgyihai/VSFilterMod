# Changelog

**English** | [简体中文](CHANGELOG.zh-CN.md)

## Unreleased

### New renderer feature

- Added the \rgb subtitle override tag: chromatic aberration that shifts the
  red and blue channels of the sentence in opposite directions. \rgb4 and
  \rgb(4) are equivalent, \t animation is supported and \rgb0 disables it.

- Added the \glow subtitle override tag. \glow20 and \glow(20) draw a
  primary-colour halo with a radius of 20 output pixels; \glow(20,1.5) also
  sets a linear strength multiplier. The effect includes body and outline shapes but
  not the shadow, follows subtitle alpha/fading and clipping, supports \t
  animation, and is disabled by \glow0. Radius and strength are capped at 4096
  output pixels and 64 respectively. The effect is rendered with a bloom-style
  multi-scale pipeline: Gaussian approximations at three radii are accumulated
  in 16-bit linear light and compressed with a Reinhard tone curve, so the halo
  keeps a smooth gradient instead of saturating into a flat cloud; strength
  acts as exposure before tone mapping. TypeSub tag metadata includes the new tag.
  The regression suite checks syntax equivalence, strength, colour,
  transparency, \t animation and rectangular/vector/inverse/moving-vector
  clipping.

### Arbitrary-angle gradient

- \vgrad(angle): when a \1vc-style gradient is active, the bilinear
  four-corner interpolation is replaced by a linear two-colour gradient along
  the given direction in degrees. The first two vc colours act as start and
  end; angle 0 runs left to right, 90 top to bottom. \vgrad45 shorthand form
  works, \t animation is supported, and removing the tag restores the
  classic bilinear mix. Regression suite tests/vgrad.py verifies direction,
  reversal and syntax equivalence.

### Radial gradient

- \rgrad(scale[,cx,cy]): with a \1vc-style gradient active, colours radiate
  from a centre point: the first vc colour sits at the centre and blends
  linearly into the second at the cap radius, which is the scaled distance
  from the centre to the farthest box corner (CSS farthest-corner semantics).
  cx/cy position the centre as a UV fraction of the glyph box in screen
  orientation (0.5/0.5, the default, is the box centre; values outside [0,1]
  are allowed). \rgrad1 is the shorthand, \rgrad0 disables, \t animation is
  supported, and \vgrad/\rgrad are mutually exclusive with the later tag
  winning. Regression suite tests/rgrad.py verifies the centre/edge falloff,
  scale, centre orientation, syntax equivalence, mode exclusion and reset.

### Arc text

- `\arc(radius[,angle])`: wraps the line baseline onto a circle of
  `radius` output pixels centred on the line anchor (the `\pos` point for
  positioned lines). At angle zero the centre of the line sits on top of
  the circle and glyphs proceed clockwise, so short text forms a
  rainbow-like arch while long text wraps around the circle disc-style.
  Positive radius puts the text outside the circle, negative inside;
  the angle parameter rotates the whole arrangement, `\arc120` is
  shorthand for `\arc(120)`, `\t` animation is supported and `\arc0`
  disables the effect. Text is split into single characters for
  layout, so Latin ligatures/kerning are lost while CJK text is
  unaffected; `\fsp` spacing still applies along the arc. TypeSub tag
  metadata includes the new tag.

### Track matte

- \matte[offset] / \imatte[offset]: the tagged event becomes a hidden alpha
  matte whose rendered silhouette (shadow, outline and body) masks events on
  layer (matte layer + offset, default 1), After Effects style. Matted events
  paint into an offscreen canvas and composite into the frame modulated
  per-pixel by the matte coverage, so every effect - glow, gradients, motion
  blur - stays inside the mask. \imatte inverts the mask; several matte
  events on one layer union into a single mask; \matte0 disables.
  Regression suite tests/matte.py.

### Effect package

- \glowc&HBBGGRR& and \glowa&HAA&: optional glow colour and alpha
  overrides. The colour replaces the default primary-colour emission and the
  alpha uses the ASS convention (00 opaque, FF transparent); both support \t
  animation and reset with the style.

- \iglow inner glow: \iglow8 and \iglow(8[,strength]) draw light that
  bleeds inward from the glyph edges, limited to the glyph interior and
  composited after the body. \iglowc/\iglowa override colour and alpha
  (default white, opaque). \iglow0 disables.

- \mblur(angle,length) directional motion blur: spreads the finished
  bitmap along the given direction (degrees, output pixels) with deterministic
  integer sampling. \mblur45,10 or the shorthand suffix form.

- \bevel(size[,strength]) smooth bevel/emboss relief derived from the
  glyph coverage, composited as highlight/shadow shading.

- \clipf(radius) feathered clipping: softens rectangular and vector
  clip edges (including inverse clips) by the given radius; \clipf15 is the
  shorthand. Vector clip masks are blurred once after parsing so all drawing
  and glow paths inherit the soft edge.

- New regression suite tests/fxpackage.py covers all of the above.

### Upstream alignment

- Carried over upstream's non-blocking unknown-style diagnostic: distinct
  missing styles are collected into one warning rather than reported by a
  modal prompt. `vsfmrender` prints the warning and the VapourSynth filter
  logs it as a warning; rendering is unchanged.

## 1.4.0

### CSRI

- New: the plugin exports `csri_typesub_tags_json()`, a self-description of
  the extension tags this renderer understands (`\1img`..`\4img`, the
  `\1vc`/`\1va` gradient colors and alphas, `\jitter`, `\distort`,
  `\mover`/`\moves3`/`\moves4`/`\movevc`, the `\rnd` family, `\z`,
  `\frs`, `\fsc`) as a static UTF-8 JSON document. Editors (TypeSub) look
  the symbol up after loading the plugin and merge the table into
  autocompletion, so adding a tag here no longer requires an editor release.
  The symbol is optional: hosts that do not know it are unaffected.

### Fixes

- The VFR timecode reader parsed the fps column with the C locale's `%f`:
  on systems whose locale uses a comma decimal separator the values misread
  and frame timing drifted. Parsing is locale-independent now.
- `\wave(amp,freq[,phase])`: displaces each glyph vertically on a sine
  wave running along the line advance direction and rotates it to the
  wave tangent, pivoting on the displaced baseline midpoint. `amp` and
  `freq` are output pixels (`freq` is the full wavelength), `phase` is
  in degrees; `\wave0` disables the effect and `\t` animation is
  supported on all three parameters. Text is split per character like
  the arc layout (same ligature caveat; CJK unaffected), UTF-16
  surrogate pairs are kept whole, and the wave shape is intrinsic to
  the line: it does not depend on screen position. TypeSub tag
  metadata includes the new tag.

- `\extrude(depth[,angle])`: 3D extrusion -- shadow-colour copies of
  each glyph at 1px steps along the extrusion direction (default 45
  degrees in screen coordinates: 0 = right, 90 = down), forming the
  solid side faces of the text. The copies reuse the rasterized word
  (pure compositing, no re-transform), render even with zero shadow
  depth, take their colour and alpha from the shadow style (`\4c`/`\4a`),
  support `\t` animation, and `\extrude0` disables the effect.

- `\echo(count,dx,dy[,falloff])`: motion-trail copies of the whole event.
  The event renders into an offscreen canvas which is composited
  once per copy at stepped integer offsets with alpha scaled by
  `falloff` per step (default 0.5), then once more at full alpha;
  every effect (glow, gradients, blur, transforms) is captured
  exactly. `count` caps at 32 copies, `\t` animation is supported,
  `\echo0` disables the effect. The effect is event-level: the
  parameters of the last override block apply to the whole line,
  and a track-matte consumption takes precedence over echo when
  both apply to the same event. TypeSub tag metadata includes the
  new tags.

- `\pin(x1,y1,x2,y2,x3,y3,x4,y4)`: corner-pin perspective warping.
  The four corners of the line layout box (clockwise from top-left)
  are mapped through a projective transform (homography) onto the
  four given output-pixel points, so text can be fitted onto
  arbitrarily tilted rectangular surfaces -- signboards, screens,
  book covers -- with true perspective foreshortening rather than
  the orthogonal approximations of `\frx`/`\fry`/`\fax`. The warp
  applies to the fully transformed glyph paths (rotation, scaling,
  arc and wave compose underneath), solving an 8x8 linear system
  per word in IEEE double arithmetic that is bit-identical across
  platforms. `\pin0` or a single zero parameter disables the effect
  and `\t` animation is supported on all eight coordinates. Unlike
  the legacy `\distort` tag (three-point bilinear, bounding-box
  relative), `\pin` is a true perspective transform in absolute
  output-pixel coordinates. TypeSub tag metadata includes the new
  tag.

- `\mgrad(c1,c2[,c3,...])`: multi-stop gradients. Two to eight
  colour stops in ASS hex order (`&HBBGGRR`), evenly spaced along the
  active gradient direction: the `\vgrad` angle or the `\rgrad`
  radius when one is set, horizontal otherwise. The stops apply to
  the body channel and interpolate piecewise-linearly in IEEE double
  arithmetic (bit-identical across platforms); `\mgrad0` or fewer
  than two colours disables the effect. TypeSub tag metadata
  includes the new tag.

- `\shadblur(radius)` / `\bordblur(radius)`: per-layer gaussian
  blur. The shadow / outline pass re-rasterizes the word overlay
  with the layer-specific radius added to the base `\blur` value,
  so the body can stay crisp while the shadow diffuses (or vice
  versa). Both forms support compact syntax (`\shadblur4`), `	`
  animation, and zero disables the extra blur. TypeSub tag metadata
  includes the new tags.

- `\persp(dist)`: true-perspective camera distance for the
  `\frx`/`\fry` rotations, in output pixels. The legacy projection
  uses a fixed weak distance (about 312 px at unit scale); smaller
  distances produce dramatic foreshortening -- near-end expansion
  and far-end compression -- while large distances approach the
  orthogonal limit. The projected line width follows the
  homographic width law cos(t) d^2 / (d^2 - (W/2 sin(t))^2), which
  the regression test validates analytically. `\persp0` restores
  the legacy projection, `\t` animation is supported. TypeSub tag
  metadata includes the new tag.

- `csri_open_file`/`csri_open_mem` share one instance-setup path; remaining
  POSIX portability and build documentation work.

## 1.3.0

### VapourSynth

- New: the plugin is also a VapourSynth plugin, `vsfm.TextSubMod(clip, file,
  charset, fps, vfr, accurate)`, with the identifier, namespace and arguments
  of upstream's, written against API 4 (VapourSynth R55 and later).
- YUV420P8 and RGB24 render exactly as upstream's filter did. 9- to 16-bit
  YUV 4:2:0 and 4:2:2, YUV422P8, Gray and high-bit-depth RGB are blended with
  the same arithmetic at the clip's own precision, instead of upstream's
  split into two 8-bit halves; `accurate=` is accepted and has no effect.
- CI loads it into VapourSynth on Linux, macOS and Windows and checks the
  output against one `tests/vsplugin/expected.txt`.
- The AviSynth+ and VapourSynth entry points share one render path
  (`src/vsfilter/sub_source.cpp`); AviSynth output is unchanged.

### Fixes

- A glyph stretched to hundreds of thousands of pixels (`\fscy10000000`,
  or a style's ScaleY) could take minutes and gigabytes to outline. Shapes
  wider or taller than 131,072 pixels are no longer drawn (the limit was
  2^20), and widening an outline is capped at a cost about 45 times that of
  a full-frame 1080p glyph with `\bord100`. Found by the fuzzer.
- A header line with no value -- `YCbCr Matrix:`, `Collisions:`,
  `ScaledBorderAndShadow:` -- threw an exception out of the parser and
  crashed Aegisub (upstream does the same). The field keeps its default.
- A `\clip` / `\iclip` line reaching the bottom row of the frame read one
  row past the end of the clip mask (a heap over-read; upstream's bug, found
  by the fuzzer under AddressSanitizer). Upstream's one-row offset of the
  mask is kept, so clipped lines look as before; at the bottom edge the last
  row is used.
- No C++ exception can escape into a host any more: the CSRI, AviSynth and
  VapourSynth entry points catch them, and a format reader that throws fails
  like one that finds a syntax error.
- `tests/regressions/`: scripts that once crashed or hung, each rendered by
  a test of its own and used as fuzzer seeds.
- `fuzz_render` reports inputs that take 5 s or more by iteration number, and
  `VSFM_FUZZ_DUMP=N` saves iteration N's input.

## 1.2.0

### Identical output on every platform

Given the same fonts, Windows, Linux and macOS — on x86 and ARM, 32- and
64-bit — now render every subtitle to the same pixels. CI checks this on
every platform on every change.

- No FMA contraction (`-ffp-contract=off`): ARM builds rounded `a * b + c`
  differently from x86 and moved animated lines by a fraction of a pixel.
- One glyph-transform path instead of an SSE and a C one that disagreed under
  `\distort` and drew `\rnd` jitter in a different order. Upstream's
  approximate reciprocal (RCPPS), which differs between Intel and AMD CPUs, is
  reproduced bit for bit as Intel computes it.
- One set of blend routines instead of SSE2 and C copies that had drifted
  apart (`\1vc` gradients, `\kf` colour switching under clips).
- `sin`, `cos`, `exp`, `log` and `pow` are computed by built-in, bit-exact
  implementations instead of each platform's C library.
- `\c&H80FF00FF&` and other out-of-range values parse as on Windows (32-bit
  `long`) everywhere; numbers are read with `.` as the decimal point whatever
  the host application's locale.
- Signed integer overflow on extreme values wraps, as it did upstream, instead
  of being undefined.
- Out-of-range float-to-integer conversion of font sizes follows x86.

### Fonts

- `[Fonts]` attachments are found by every name GDI would find them by: the
  Windows family name, the typographic family, the full name, localized names
  in every language, and GBK/Big5/Shift-JIS-encoded names in older CJK fonts.
- Several attached faces of one family: the one matching the requested weight
  and slant is used (previously the last attached won).
- Named instances of attached variable fonts are listed as GDI lists them.
- Glyph fallback tries attached fonts before the platform's own fallback.
- New: a shared font folder — `vsfmrender --fontdir DIR`, or the
  `VSFM_FONT_DIRS` environment variable for Aegisub and AviSynth.
- New: font diagnostics — `vsfmrender --font-report`, `--strict-fonts`, and
  `VSFM_FONT_LOG=1`.

### Fixes

- A malformed SubViewer-style line could hang the parser (and Aegisub) in an
  infinite loop.
- A Scroll effect fell through into the moving-vector-clip code (an upstream
  VSFilterMod bug).
- Reads of an uninitialized rasterizer flag; undefined shifts of negative
  values.
- vsfmrender confirms identical frames by comparing pixels, not only a hash.

### Builds

- New platforms: Windows x86 (32-bit) and Windows ARM64; macOS universal
  (arm64 + x86_64).
- macOS builds are self-contained (FreeType and libpng linked in) and run on
  macOS 11+ (Apple Silicon) and 10.15+ (Intel). Linux builds link FreeType and
  libpng in privately too and run on glibc 2.35+ (Ubuntu 22.04 and later).
- Every platform uses the same pinned FreeType.
- sse2neon is no longer used.
- `vsfmrender --version`; Windows binaries carry version information.
- Release archives come with a `SHA256SUMS` file.

### Testing

- `tests/consistency`: text and drawing fixtures covering the mod tags,
  rendered with bundled fonts and compared pixel for pixel against a single
  `expected.txt` on every platform.
- Generated test fonts for font lookup (`tests/consistency/fonts`).
- AddressSanitizer / UndefinedBehaviorSanitizer CI job and a fuzzer for the
  script parser and renderer (`tests/fuzz_render.cpp`).
