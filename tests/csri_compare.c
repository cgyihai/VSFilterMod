/*
 *  Renders the same script through two CSRI plugins and dumps the raw
 *  surfaces, so the port can be compared pixel for pixel against the upstream
 *  VSFilterMod.dll that Aegisub ships.
 *
 *  Both plugins go through the same csriapi.cpp, so the only things that can
 *  differ are the glyph source (GDI vs FreeType), font matching, and any bug in
 *  the port -- which is exactly what the comparison is for.
 *
 *  The frame is pre-filled with 0xFF000000, the rasterizer's "fully
 *  transparent" value (its alpha channel is inverted), so what comes back is
 *  the renderer's native surface with nothing composited underneath.
 *
 *  usage: csri_compare <plugin.dll> <script.ass> <width> <height> <outprefix> t1 [t2 ...]
 *  writes <outprefix>_<t>.raw  (width*height*4 bytes, BGRA, top-down)
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
#else
#include <dlfcn.h>
#define LIBHANDLE void*
#define LIBOPEN(p) dlopen((p), RTLD_NOW)
#define LIBSYM(h, n) dlsym((h), (n))
#endif

enum { CSRI_F_BGR_ = 0x102 };

struct csri_fmt { int pixfmt; unsigned width, height; };
struct csri_frame { int pixfmt; unsigned char* planes[4]; ptrdiff_t strides[4]; };

int main(int argc, char** argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s plugin script width height outprefix t1 [t2...]\n", argv[0]);
        return 2;
    }

    LIBHANDLE lib = LIBOPEN(argv[1]);
    if (!lib) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }

    void* (*rdefault)(void) = (void* (*)(void))LIBSYM(lib, "csri_renderer_default");
    void* (*open_mem)(void*, const void*, size_t, void*) =
        (void* (*)(void*, const void*, size_t, void*))LIBSYM(lib, "csri_open_mem");
    int (*request_fmt)(void*, const struct csri_fmt*) =
        (int (*)(void*, const struct csri_fmt*))LIBSYM(lib, "csri_request_fmt");
    void (*render)(void*, struct csri_frame*, double) =
        (void (*)(void*, struct csri_frame*, double))LIBSYM(lib, "csri_render");
    void (*close_inst)(void*) = (void (*)(void*))LIBSYM(lib, "csri_close");

    if (!rdefault || !open_mem || !request_fmt || !render || !close_inst) {
        fprintf(stderr, "missing CSRI entry points in %s\n", argv[1]);
        return 1;
    }

    FILE* sf = fopen(argv[2], "rb");
    if (!sf) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    fseek(sf, 0, SEEK_END);
    long slen = ftell(sf);
    fseek(sf, 0, SEEK_SET);
    char* script = (char*)malloc((size_t)slen);
    fread(script, 1, (size_t)slen, sf);
    fclose(sf);

    const unsigned W = (unsigned)atoi(argv[3]);
    const unsigned H = (unsigned)atoi(argv[4]);

    void* inst = open_mem(rdefault(), script, (size_t)slen, NULL);
    if (!inst) { fprintf(stderr, "open_mem failed\n"); return 1; }

    struct csri_fmt fmt = { CSRI_F_BGR_, W, H };
    if (request_fmt(inst, &fmt) != 0) { fprintf(stderr, "request_fmt failed\n"); return 1; }

    unsigned* buf = (unsigned*)malloc((size_t)W * H * 4);

    for (int i = 6; i < argc; ++i) {
        const double t = atof(argv[i]);
        for (size_t p = 0; p < (size_t)W * H; ++p) buf[p] = 0xFF000000u;

        struct csri_frame fr;
        memset(&fr, 0, sizeof(fr));
        fr.pixfmt = CSRI_F_BGR_;
        fr.planes[0] = (unsigned char*)buf;
        fr.strides[0] = (ptrdiff_t)W * 4;
        render(inst, &fr, t);

        char name[1024];
        snprintf(name, sizeof(name), "%s_%s.raw", argv[5], argv[i]);
        FILE* of = fopen(name, "wb");
        fwrite(buf, 4, (size_t)W * H, of);
        fclose(of);
    }

    close_inst(inst);
    free(buf);
    free(script);
    return 0;
}
