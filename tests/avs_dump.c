/*
 *  Imports an AviSynth script through AviSynth+'s C API and writes one frame's
 *  raw planes, so TextSubMod's output on RGB32 / RGB24 / YUY2 / YV12 clips can
 *  be compared between upstream VSFilterMod and this build.
 *
 *  usage: avs_dump <script.avs> <frame> <out.raw>
 *  prints: "<width> <height> <pixel_type-hex>"; writes each plane in memory
 *  order (row by row, pitch stripped): the packed plane, then U and V if planar.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define LIBOPEN(p) LoadLibraryA(p)
#define LIBSYM(h, n) ((void*)GetProcAddress((HMODULE)(h), (n)))
#else
#include <dlfcn.h>
#define LIBOPEN(p) dlopen((p), RTLD_NOW)
#define LIBSYM(h, n) dlsym((h), (n))
#endif

typedef struct { short type, array_size; union { void* clip; const char* s; int i; float f; } d; } Value;
typedef struct { int width, height; unsigned num, den; int frames, pixel_type; int pad[6]; } VideoInfo;

#if defined(_WIN32) && !defined(_WIN64)
#define CC __stdcall
#else
#define CC
#endif

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s script.avs frame out.raw\n", argv[0]); return 2; }

#if defined(_WIN32)
    void* lib = LIBOPEN("avisynth.dll");
#elif defined(__APPLE__)
    void* lib = LIBOPEN("libavisynth.dylib");
#else
    void* lib = LIBOPEN("libavisynth.so");
#endif
    if (!lib) { fprintf(stderr, "cannot load AviSynth\n"); return 1; }

    void* (CC *create)(int) = (void* (CC *)(int))LIBSYM(lib, "avs_create_script_environment");
    Value (CC *invoke)(void*, const char*, Value, const char**) =
        (Value (CC *)(void*, const char*, Value, const char**))LIBSYM(lib, "avs_invoke");
    void* (CC *take_clip)(Value, void*) = (void* (CC *)(Value, void*))LIBSYM(lib, "avs_take_clip");
    const VideoInfo* (CC *get_vi)(void*) = (const VideoInfo* (CC *)(void*))LIBSYM(lib, "avs_get_video_info");
    void* (CC *get_frame)(void*, int) = (void* (CC *)(void*, int))LIBSYM(lib, "avs_get_frame");
    int (CC *get_pitch)(const void*, int) = (int (CC *)(const void*, int))LIBSYM(lib, "avs_get_pitch_p");
    int (CC *get_row)(const void*, int) = (int (CC *)(const void*, int))LIBSYM(lib, "avs_get_row_size_p");
    int (CC *get_height)(const void*, int) = (int (CC *)(const void*, int))LIBSYM(lib, "avs_get_height_p");
    const unsigned char* (CC *get_read)(const void*, int) =
        (const unsigned char* (CC *)(const void*, int))LIBSYM(lib, "avs_get_read_ptr_p");
    const char* (CC *clip_error)(void*) = (const char* (CC *)(void*))LIBSYM(lib, "avs_clip_get_error");

    void* env = create(6);
    Value arg; arg.type = 's'; arg.array_size = 0; arg.d.s = argv[1];
    Value r = invoke(env, "Import", arg, NULL);
    if (r.type == 'e') { fprintf(stderr, "Import: %s\n", r.d.s); return 1; }
    if (r.type != 'c') { fprintf(stderr, "Import did not return a clip\n"); return 1; }

    void* clip = take_clip(r, env);
    const VideoInfo* vi = get_vi(clip);
    void* frame = get_frame(clip, atoi(argv[2]));
    if (!frame) { fprintf(stderr, "get_frame: %s\n", clip_error ? clip_error(clip) : "?"); return 1; }
    if (clip_error && clip_error(clip)) { fprintf(stderr, "filter error: %s\n", clip_error(clip)); return 1; }

    FILE* out = fopen(argv[3], "wb");
    const int planes[3] = { 0, 1 << 1, 1 << 2 };      /* default, U, V */
    const int planar = (vi->pixel_type & (1 << 31)) != 0;   /* AVS_CS_PLANAR */
    for (int p = 0; p < (planar ? 3 : 1); ++p) {
        const int pitch = get_pitch(frame, planes[p]);
        const int row = get_row(frame, planes[p]);
        const int h = get_height(frame, planes[p]);
        const unsigned char* src = get_read(frame, planes[p]);
        for (int y = 0; y < h; ++y) fwrite(src + (size_t)y * pitch, 1, (size_t)row, out);
    }
    fclose(out);
    printf("%d %d %08x\n", vi->width, vi->height, (unsigned)vi->pixel_type);
    return 0;
}
