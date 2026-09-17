project('vsfiltermod', 'c', 'cpp',
    version : '1.1.90',
    license : 'GPL-2.0-or-later',
    meson_version : '>=0.59.0',
    default_options : [
        'cpp_std=c++17',
        'c_std=c11',
        'buildtype=release',
        'b_ndebug=if-release',
        'warning_level=1',
        # The CSRI loader resolves exactly eight symbols by name; everything
        # else stays private so nothing collides with the host application
        # (Aegisub already links libass, which has its own rasterizer).
        'cpp_visibility=hidden',
        'c_visibility=hidden',
    ])

cxx = meson.get_compiler('cpp')
host_system = host_machine.system()
host_cpu = host_machine.cpu_family()

# ---------------------------------------------------------------------------
# Compile flags
# ---------------------------------------------------------------------------

add_project_arguments('-D_VSMOD', language : ['c', 'cpp'])

if host_system == 'windows'
    add_project_arguments(
        '-DUNICODE', '-D_UNICODE',
        '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
        language : ['c', 'cpp'])
endif

if cxx.get_id() == 'msvc'
    add_project_arguments('/utf-8', '/wd4996', language : ['c', 'cpp'])
else
    add_project_arguments(
        '-Wno-unused-parameter',
        '-Wno-unused-variable',
        '-Wno-sign-compare',
        '-Wno-write-strings',
        '-fno-strict-aliasing',   # the rasterizer type-puns pixel buffers
        language : ['c', 'cpp'])
endif

# SSE2 is a baseline assumption of the blend kernels. It is implied by x86-64
# and by AArch64-through-sse2neon; only 32-bit x86 has to ask for it.
if host_cpu == 'x86'
    if cxx.get_id() == 'msvc'
        add_project_arguments('/arch:SSE2', language : ['c', 'cpp'])
    else
        add_project_arguments('-msse2', '-mfpmath=sse', language : ['c', 'cpp'])
    endif
endif

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

deps = []

deps += dependency('freetype2', version : '>=2.8', fallback : ['freetype2', 'freetype_dep'])
deps += dependency('libpng', version : '>=1.6', fallback : ['libpng', 'libpng_dep'])
deps += dependency('threads')

font_backend = get_option('font_backend')
if font_backend == 'auto'
    if host_system == 'windows'
        font_backend = 'dwrite'
    elif host_system == 'darwin'
        font_backend = 'coretext'
    else
        font_backend = 'fontconfig'
    endif
endif

font_src = []

if font_backend == 'fontconfig'
    deps += dependency('fontconfig', version : '>=2.11')
    add_project_arguments('-DVSFM_FONT_FONTCONFIG', language : ['c', 'cpp'])
    font_src += files('src/font/font_provider_fontconfig.cpp')
elif font_backend == 'coretext'
    deps += dependency('appleframeworks', modules : ['CoreFoundation', 'CoreText'])
    add_project_arguments('-DVSFM_FONT_CORETEXT', language : ['c', 'cpp'])
    font_src += files('src/font/font_provider_coretext.cpp')
elif font_backend == 'dwrite'
    deps += cxx.find_library('dwrite')
    deps += cxx.find_library('ole32')
    add_project_arguments('-DVSFM_FONT_DWRITE', language : ['c', 'cpp'])
    font_src += files('src/font/font_provider_dwrite.cpp')
else
    error('unknown font_backend: ' + font_backend)
endif

# iconv backs the legacy code page conversions that SSA's charset field needs.
# glibc has it built in; macOS and musl need the separate library.
if host_system != 'windows'
    if not cxx.has_function('iconv_open', prefix : '#include <iconv.h>')
        deps += dependency('iconv')
    endif
endif

# On AArch64 (Apple Silicon, ARM Linux) the SSE2 blend kernels go through
# sse2neon, a header-only shim, so Rasterizer stays single-sourced. It is
# optional: without it the scalar kernels are used instead, which is correct
# but slower. Point at it with -Dcpp_args=-I/path/to/sse2neon, or drop the
# header somewhere on the include path.
sse2neon_inc = []
if host_cpu == 'aarch64'
    if cxx.has_header('sse2neon.h')
        message('sse2neon found: using the SSE2 blend kernels on NEON')
    else
        warning('sse2neon.h not found; falling back to the scalar blend kernels')
    endif
endif

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

vsfm_src = files(
    'src/compat/cstring_compat.cpp',
    'src/compat/text_compat.cpp',
    'src/compat/cpu_compat.cpp',

    'src/font/font_engine.cpp',

    'src/subtitles/RTS.cpp',
    'src/subtitles/STS.cpp',
    'src/subtitles/Rasterizer.cpp',
    'src/subtitles/TextFile.cpp',
) + font_src

inc = include_directories('src', 'src/vsfilter', 'include')

# ---------------------------------------------------------------------------
# The CSRI plugin
#
# Aegisub's CSRI loader looks for shared objects on CSRI_PATH (POSIX) or beside
# the executable in csri/ (Windows), and resolves csri_* by name. No import
# library, no versioned soname, no 'lib' prefix on POSIX: the loader dlopen()s
# whatever files it finds.
# ---------------------------------------------------------------------------

csri_lib = shared_library('vsfiltermod',
    vsfm_src + files('src/vsfilter/csriapi.cpp'),
    include_directories : [inc] + sse2neon_inc,
    dependencies : deps,
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : true,
    install_dir : get_option('libdir') / 'csri')

# ---------------------------------------------------------------------------
# BDN+XML writer
# ---------------------------------------------------------------------------

if get_option('bdnxml')
    subdir('src/bdnxml')
endif

summary({
    'font backend'   : font_backend,
    'target'         : host_system + '/' + host_cpu,
    'BDN+XML writer' : get_option('bdnxml'),
}, section : 'VSFilterMod')
