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

# Output must not depend on the target CPU. GCC and Clang fuse a * b + c into
# a single FMA wherever the target has one -- every AArch64 core, so Apple
# Silicon and ARM Linux -- and an FMA rounds once where x86-64 rounds twice.
# That moves glyph points by a fraction of a pixel and changes the edges of
# animated (\move, \t, \org) lines. MSVC does not contract under its default
# /fp:precise; never build with /fp:fast or -ffast-math.
if cxx.get_id() != 'msvc'
    add_project_arguments(
        cxx.get_supported_arguments('-ffp-contract=off'),
        language : ['c', 'cpp'])
endif

# 32-bit x86 must do float arithmetic in SSE registers, not on the x87 stack
# with its 80-bit intermediates, or glyph positions would differ from every
# other build. x86-64 always uses SSE.
if host_cpu == 'x86'
    if cxx.get_id() == 'msvc'
        add_project_arguments('/arch:SSE2', language : ['c', 'cpp'])
    else
        add_project_arguments('-msse2', '-mfpmath=sse', language : ['c', 'cpp'])
    endif
endif

# A CSRI plugin is dlopen'd by the host application, which will not have the
# compiler's runtime DLLs on its search path. Link them in statically so the
# plugin is a single file that can just be dropped into a csri directory.
# (MSVC is handled by b_vscrt; only the GCC-family Windows toolchains need
# this.)
if host_system == 'windows' and cxx.get_id() != 'msvc'
    add_project_link_arguments(
        '-static-libgcc', '-static-libstdc++',
        '-Wl,-Bstatic,--whole-archive', '-lwinpthread',
        '-Wl,--no-whole-archive,-Bdynamic',
        language : ['c', 'cpp'])
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

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

vsfm_src = files(
    'src/compat/cstring_compat.cpp',
    'src/compat/text_compat.cpp',
    'src/compat/cpu_compat.cpp',
    'src/compat/det_float.cpp',
    'src/compat/num_compat.cpp',

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

# The AviSynth+ entry points (TextSubMod / MaskSubMod) live in the same
# library, as they did upstream, so one file serves both Aegisub (CSRI) and
# AviSynth scripts such as avs2bdnxml's. AviSynth is not linked: its C API is
# resolved at plugin init from the module that loaded us.
plugin_src = files('src/vsfilter/csriapi.cpp')
plugin_deps = []
if get_option('avisynth')
    plugin_src += files(
        'src/vsfilter/avisynth_plugin.cpp',
        'src/vsfilter/vfr.cpp',
        'src/subpic/mem_subpic.cpp',
    )
    if host_system != 'windows'
        plugin_deps += cxx.find_library('dl', required : false)
    endif
endif

csri_lib = shared_library('vsfiltermod',
    vsfm_src + plugin_src,
    include_directories : inc,
    dependencies : deps + plugin_deps,
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

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if get_option('tests')
    csri_loader_test = executable('csri_loader_test',
        files('tests/csri_loader_test.c'),
        dependencies : host_system == 'windows' ? [] : [cxx.find_library('dl', required : false)],
        build_by_default : false)

    test('csri-abi', csri_loader_test,
         args : [csri_lib.full_path()],
         depends : [csri_lib])

    det_float_test = executable('det_float_test',
        files('tests/det_float_test.cpp', 'src/compat/det_float.cpp'),
        include_directories : inc,
        build_by_default : false)

    test('det-float', det_float_test)

    num_compat_test = executable('num_compat_test',
        files('tests/num_compat_test.cpp', 'src/compat/num_compat.cpp'),
        include_directories : inc,
        build_by_default : false)

    test('num-compat', num_compat_test)

    # Renders tests/consistency/*.ass with the bundled font embedded, and
    # requires every event's pixels to match tests/consistency/expected.txt.
    # The same file is checked on every platform in CI, so a pass everywhere
    # means Windows, Linux and macOS render identically.
    if get_option('bdnxml')
        py = find_program('python3', 'python', required : false)
        if py.found()
            test('render-consistency', py,
                 args : [files('tests/consistency/run.py'),
                         '--renderer', vsfmrender.full_path(),
                         '--out', meson.current_build_dir() / 'consistency'],
                 depends : [vsfmrender],
                 timeout : 300)
        endif
    endif

    # Type-checks the POSIX branch of the compatibility layer while building
    # on Windows, by undefining _WIN32 after the system headers are in. It
    # cannot stand in for a real Linux or macOS build -- iconv, fontconfig,
    # CoreText and the 32-bit wchar_t assumption are all out of its reach --
    # but it does catch typos in code a Windows build never instantiates.
    #
    # Compile only: its objects use the POSIX manglings (DWORD is uint32_t
    # there, not unsigned long), so they cannot link against the rest of a
    # Windows build. Compiling is the whole point. It needs the POSIX
    # <strings.h>, which MinGW has and MSVC does not.
    if host_system == 'windows' and cxx.has_header('strings.h')
        static_library('posix_headers_probe',
            files('tests/posix_headers_probe.cpp'),
            include_directories : inc,
            dependencies : deps,
            build_by_default : true)
    endif
endif

summary({
    'font backend'   : font_backend,
    'target'         : host_system + '/' + host_cpu,
    'BDN+XML writer' : get_option('bdnxml'),
    'AviSynth+ entry': get_option('avisynth'),
    'tests'          : get_option('tests'),
}, section : 'VSFilterMod')
