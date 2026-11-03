# Changelog

**English** | [简体中文](CHANGELOG.zh-CN.md)

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
