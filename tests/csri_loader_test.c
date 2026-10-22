/*
 *  Exercises the plugin the way Aegisub's CSRI provider does: load the shared
 *  object by hand, resolve the csri_* entry points by name, feed it a script
 *  from memory, request CSRI_F_BGR_ and render a frame.
 *
 *  Deliberately written against the raw ABI rather than linking the library,
 *  so that a missing or mis-decorated export fails here instead of silently
 *  making the renderer invisible inside Aegisub.
 *
 *  build:  cc -o csri_loader_test csri_loader_test.c -ldl     (POSIX)
 *          cc -o csri_loader_test.exe csri_loader_test.c      (Windows)
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define LIBHANDLE HMODULE
#define LIBOPEN(p) LoadLibraryA(p)
#define LIBSYM(h, n) ((void*)GetProcAddress((h), (n)))
#define LIBCLOSE(h) FreeLibrary(h)
#else
#include <dlfcn.h>
#define LIBHANDLE void*
#define LIBOPEN(p) dlopen((p), RTLD_NOW)
#define LIBSYM(h, n) dlsym((h), (n))
#define LIBCLOSE(h) dlclose(h)
#endif

/* Mirrors csri.h without including it, so the test checks the ABI, not the
 * header we happened to compile the plugin with. */
enum csri_pixfmt {
    CSRI_F_RGBA = 0, CSRI_F_ARGB, CSRI_F_BGRA, CSRI_F_ABGR,

    CSRI_F_RGB_ = 0x100, CSRI_F__RGB,
    CSRI_F_BGR_,                    /* Windows "RGB32" -- what Aegisub asks for */
    CSRI_F__BGR,

    CSRI_F_RGB = 0x200, CSRI_F_BGR,

    CSRI_F_AYUV = 0x1000, CSRI_F_YUVA, CSRI_F_YVUA,
    CSRI_F_YUY2 = 0x1100,
    CSRI_F_YV12A = 0x2011, CSRI_F_YV12 = 0x2111
};

struct csri_fmt { enum csri_pixfmt pixfmt; unsigned width, height; };
struct csri_frame { enum csri_pixfmt pixfmt; unsigned char *planes[4]; ptrdiff_t strides[4]; };
struct csri_info { const char *name, *version, *longname, *author, *copyright; };

typedef void csri_rend;
typedef void csri_inst;

static const char kScript[] =
    "\xEF\xBB\xBF[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 640\n"
    "PlayResY: 480\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,40,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,1,2,20,20,20,1\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\\b1}CSRI{\\b0} loader test\n";

int main(int argc, char** argv)
{
    const char* path = (argc > 1) ? argv[1] : "vsfiltermod.dll";
    int failures = 0;

    LIBHANDLE lib = LIBOPEN(path);
    if (!lib) {
        fprintf(stderr, "FAIL: cannot load %s\n", path);
#if !defined(_WIN32)
        fprintf(stderr, "      %s\n", dlerror());
#endif
        return 1;
    }
    printf("ok   loaded %s\n", path);

    struct csri_info *(*renderer_info)(csri_rend*);
    csri_rend *(*renderer_default)(void);
    csri_rend *(*renderer_next)(csri_rend*);
    csri_inst *(*open_mem)(csri_rend*, const void*, size_t, void*);
    void (*close_inst)(csri_inst*);
    int (*request_fmt)(csri_inst*, const struct csri_fmt*);
    void (*render)(csri_inst*, struct csri_frame*, double);

#define RESOLVE(var, name)                                    \
    do {                                                      \
        *(void**)(&var) = LIBSYM(lib, name);                  \
        if (!var) { fprintf(stderr, "FAIL: missing %s\n", name); ++failures; } \
        else printf("ok   export %s\n", name);                \
    } while (0)

    RESOLVE(renderer_info, "csri_renderer_info");
    RESOLVE(renderer_default, "csri_renderer_default");
    RESOLVE(renderer_next, "csri_renderer_next");
    RESOLVE(open_mem, "csri_open_mem");
    RESOLVE(close_inst, "csri_close");
    RESOLVE(request_fmt, "csri_request_fmt");
    RESOLVE(render, "csri_render");

    /* The loader also insists on these two before it will accept a plugin. */
    if (!LIBSYM(lib, "csri_open_file")) { fprintf(stderr, "FAIL: missing csri_open_file\n"); ++failures; }
    if (!LIBSYM(lib, "csri_query_ext")) { fprintf(stderr, "FAIL: missing csri_query_ext\n"); ++failures; }

    if (failures) { LIBCLOSE(lib); return 1; }

    csri_rend* rend = renderer_default();
    if (!rend) { fprintf(stderr, "FAIL: no default renderer\n"); LIBCLOSE(lib); return 1; }

    struct csri_info* info = renderer_info(rend);
    if (!info || !info->name) { fprintf(stderr, "FAIL: no renderer info\n"); LIBCLOSE(lib); return 1; }
    printf("ok   renderer \"%s\" (%s) version %s\n", info->name, info->longname, info->version);

    if (renderer_next(rend) != NULL) {
        fprintf(stderr, "FAIL: csri_renderer_next should return NULL for the only renderer\n");
        ++failures;
    }

    csri_inst* inst = open_mem(rend, kScript, sizeof(kScript) - 1, NULL);
    if (!inst) { fprintf(stderr, "FAIL: csri_open_mem returned NULL\n"); LIBCLOSE(lib); return 1; }
    printf("ok   csri_open_mem parsed the script\n");

    const unsigned W = 640, H = 480;
    unsigned char* buf = (unsigned char*)calloc((size_t)W * H, 4);

    struct csri_fmt fmt;
    fmt.pixfmt = CSRI_F_BGR_;
    fmt.width = W;
    fmt.height = H;

    if (request_fmt(inst, &fmt) != 0) {
        fprintf(stderr, "FAIL: csri_request_fmt rejected CSRI_F_BGR_ %ux%u\n", W, H);
        ++failures;
    } else {
        printf("ok   csri_request_fmt accepted CSRI_F_BGR_ %ux%u\n", W, H);

        struct csri_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.pixfmt = CSRI_F_BGR_;
        frame.planes[0] = buf;
        frame.strides[0] = (ptrdiff_t)W * 4;

        render(inst, &frame, 1.0);

        /* Aegisub composites in place, so "it rendered" means the buffer
         * changed where the subtitle is and nowhere else. */
        size_t touched = 0;
        for (size_t i = 0; i < (size_t)W * H; ++i)
            if (((unsigned*)buf)[i] != 0) ++touched;

        if (touched == 0) {
            fprintf(stderr, "FAIL: csri_render drew nothing\n");
            ++failures;
        } else if (touched == (size_t)W * H) {
            fprintf(stderr, "FAIL: csri_render touched the whole frame\n");
            ++failures;
        } else {
            printf("ok   csri_render drew %zu of %zu pixels\n", touched, (size_t)W * H);
        }

        /* A time outside every line must leave the frame untouched. */
        memset(buf, 0, (size_t)W * H * 4);
        render(inst, &frame, 60.0);
        touched = 0;
        for (size_t i = 0; i < (size_t)W * H; ++i)
            if (((unsigned*)buf)[i] != 0) ++touched;
        if (touched != 0) {
            fprintf(stderr, "FAIL: csri_render drew %zu pixels outside any line\n", touched);
            ++failures;
        } else {
            printf("ok   csri_render drew nothing at t=60s, as expected\n");
        }
    }

    free(buf);
    close_inst(inst);
    LIBCLOSE(lib);

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
